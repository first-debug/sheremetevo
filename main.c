#include "gst/gstcaps.h"
#include "gst/gstelement.h"
#include "gst/gstelementfactory.h"
#include "gst/gstobject.h"
#include "gst/gstpad.h"
#include "gst/gststructure.h"
#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>

#include "gst/gstutils.h"

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("End of stream.\n");
            g_main_loop_quit(loop);
            break;
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            g_printerr("Error: %s\n", error->message);
            g_error_free(error);
            g_free(debug);
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

            g_print("%-21s state has been changed from %s to %s, pending state %s\n",
                    msg->src->name, gst_element_state_get_name(old_state), gst_element_state_get_name(new_state),
                    gst_element_state_get_name(pending_state));
            break;
        }
        default:
            break;
    }
    return TRUE;
}

static void cb_newpad(GstElement *decodebin, GstPad *pad, gpointer data) {
    GstElement *depay = (GstElement *)data;

    GstCaps *new_pad_caps = gst_pad_get_current_caps(pad);
    GstStructure *structure = gst_caps_get_structure(new_pad_caps, 0);
    const gchar *caps_name = gst_structure_get_name(structure);
    if (!g_str_has_suffix(caps_name, "x-rtp")) {
        g_print("Skip pad with capability = %s.\n", caps_name);
        return;
    }

    const gchar *media = gst_structure_get_string(structure, "media");
    if (g_strcmp0(media, "video") != 0) {
        g_print("Skip pad with media property = %s.\n", media);
        return;
    }

    GstPad *sinkpad = gst_element_get_static_pad(depay, "sink");
    if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
        g_printerr("Cannot link source and depay.\n");
    }
    gst_object_unref(sinkpad);
    gst_object_unref(structure);
    gst_caps_unref(new_pad_caps);
}

int main(int argc, char *argv[]) {
    GMainLoop *loop = NULL;
    GstElement *pipeline = NULL, *source = NULL, *depay = NULL, *parser = NULL,
               *decoder = NULL, *streammux = NULL, *pgie = NULL,
               *nvosd = NULL, *streamdemux = NULL,
               *sink = NULL;
    GstBus *bus = NULL;
    guint bus_watch_id;

    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    if (argc != 3) {
        g_printerr("Usage: %s <video_path> <path_to_config_file_nvinfer>\n", argv[0]);
        return -1;
    }

    // Elements initialization
    pipeline = gst_pipeline_new("sheremetevo");

    source = gst_element_factory_make("rtspsrc", "source");
    depay = gst_element_factory_make("rtph264depay", "depay");
    parser = gst_element_factory_make("h264parse", "parser");
    decoder = gst_element_factory_make("nvv4l2decoder", "decoder");

    streammux = gst_element_factory_make("nvstreammux", "stream-muxer");
    pgie = gst_element_factory_make("nvinfer", "primary-infer");
    streamdemux = gst_element_factory_make("nvstreamdemux", "streamdemux");

    nvosd = gst_element_factory_make("nvdsosd", "nvosd");
    sink = gst_element_factory_make("autovideosink", "sink");

    // TODO: add all elements
    if (!pipeline || !source || !depay || !parser || !decoder ||
            !streammux || !pgie || !streamdemux ||
            !nvosd || !sink) {
        g_printerr("Cannot create some modules.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(pipeline), source, depay,
            parser, decoder, streammux, pgie, streamdemux, nvosd, sink,
            NULL);

    g_signal_connect(source, "pad-added", G_CALLBACK(cb_newpad), depay);

    g_object_set(G_OBJECT(source), "location", argv[1], NULL);
    // g_object_set(G_OBJECT(source), "use-buffering", TRUE, NULL);

    // TODO: add resolution to config file
    g_object_set(G_OBJECT(streammux),
            "batch-size", 1,
            "width", 854,
            "height", 480, NULL);

    g_object_set(G_OBJECT(pgie), "config-file-path", argv[2], NULL);

    // Dynamic linking
    GstPad *src_pad = gst_element_get_static_pad(decoder, "src");
    GstPad *sink_pad = gst_element_request_pad_simple(streammux, "sink_0");

    if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
        g_printerr("Cannot link decoder_src and streammux.\n");
        gst_object_unref(src_pad);
        gst_object_unref(sink_pad);
        return -1;
    }
    gst_object_unref(src_pad);
    gst_object_unref(sink_pad);

    src_pad = gst_element_request_pad_simple(streamdemux, "src_0");
    sink_pad = gst_element_get_static_pad(nvosd, "sink");

    if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
        g_printerr("Cannot link streamdemux and nvosd.\n");
        gst_object_unref(src_pad);
        gst_object_unref(sink_pad);
        return -1;
    }
    gst_object_unref(src_pad);
    gst_object_unref(sink_pad);

    if (!gst_element_link_many(depay, decoder, NULL)){
        g_printerr("Cannot link elements.\n");
        return -1;
    }
    if (!gst_element_link_many(streammux, pgie, streamdemux, NULL)) {
        g_printerr("Cannot link elements.\n");
        return -1;
    }


    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    bus_watch_id = gst_bus_add_watch(bus, bus_call, loop);
    gst_object_unref(bus);

    g_print("\nStarting pipline...\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline));
    g_source_remove(bus_watch_id);
    g_main_loop_unref(loop);

    return 0;
}
