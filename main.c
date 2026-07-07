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
    GstElement *pipeline = NULL, *source = NULL, *streammux = NULL,
               *pgie = NULL, *nvvidconv = NULL, *nvosd = NULL, *sink = NULL;
    GstElement *rtp_video_caps = NULL, *depay = NULL, *video_caps = NULL, *parser = NULL, *decoder = NULL;
    GstCaps *caps = NULL;
    GstBus *bus = NULL;
    guint bus_watch_id;

    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    if (argc != 3) {
        g_printerr("Usage: %s <video_path> <path_to_config_файлу_nvinfer>\n", argv[0]);
        return -1;
    }

    // Elements initialization
    // source = gst_element_factory_make("uridecodebin", "source");
    source = gst_element_factory_make("rtspsrc", "source");
    rtp_video_caps = gst_element_factory_make("capsfilter", "rtp-video-caps");
    depay = gst_element_factory_make("rtph264depay", "depay");
    parser = gst_element_factory_make("h264parse", "parser");
    decoder = gst_element_factory_make("nvv4l2decoder", "decoder");
    // nvvidconv_src = gst_element_factory_make("nvvideoconvert", "nvvidconv-src");
    video_caps = gst_element_factory_make("capsfilter", "video-caps");
    streammux = gst_element_factory_make("nvstreammux", "stream-muxer");
    pgie = gst_element_factory_make("nvinfer", "primary-infer"); // Вот он, наш nvinfer!
    nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvidconv");
    nvosd = gst_element_factory_make("nvdsosd", "nvosd");
    sink = gst_element_factory_make("autovideosink", "sink");
    // sink = gst_element_factory_make("fakesink", "sink");

    // TODO: add all elements
    if (!source || !depay || !parser || !decoder ||
            !streammux || !pgie || !nvvidconv || !nvosd || !sink) {
        g_printerr("Cannot create some modules.\n");
        return -1;
    }

    pipeline = gst_pipeline_new("sheremetevo");
    if (!pipeline) {
        g_printerr("Cannot create pipeline.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(pipeline), source, rtp_video_caps, depay, video_caps, parser, decoder, streammux, pgie, nvvidconv, nvosd, sink, NULL);

    caps = gst_caps_new_simple("application/x-rtp",
            "media", G_TYPE_STRING, "video", NULL);
    g_object_set(G_OBJECT(rtp_video_caps), "caps", caps, NULL);
    gst_caps_unref(caps);

    caps = gst_caps_new_simple("video/x-h264",
        "stream-foramt", G_TYPE_STRING, "byte-stream",
        "aligmet", G_TYPE_STRING, "au", NULL);
    g_object_set(G_OBJECT(video_caps), "caps", caps, NULL);
    gst_caps_unref(caps);

    g_signal_connect(source, "pad-added", G_CALLBACK(cb_newpad), rtp_video_caps);

    g_object_set(G_OBJECT(source), "location", argv[1], NULL);
    // g_object_set(G_OBJECT(source), "use-buffering", TRUE, NULL);

    g_object_set(G_OBJECT(streammux), "batch-size", 1, NULL);
    g_object_set(G_OBJECT(streammux), "width", 1280, NULL);
    g_object_set(G_OBJECT(streammux), "height", 720, NULL);

    g_object_set(G_OBJECT(pgie), "config-file-path", argv[2], NULL);

    // Dynamic linking
    GstPad *mux_sink_pad = gst_element_request_pad_simple(streammux, "sink_1");
    GstPad *decoder_src_pad = gst_element_get_static_pad(decoder, "src");

    if (gst_pad_link(decoder_src_pad, mux_sink_pad) != GST_PAD_LINK_OK) {
        g_printerr("Cannot link decoder_src and streammux.\n");
        gst_object_unref(decoder_src_pad);
        gst_object_unref(mux_sink_pad);
        return -1;
    }

    gst_object_unref(decoder_src_pad);
    gst_object_unref(mux_sink_pad);
    if (!gst_element_link_many(rtp_video_caps, depay, decoder, NULL)){
        g_printerr("Cannot link elements.\n");
        return -1;
    }
    if (!gst_element_link_many(streammux, pgie, nvvidconv, nvosd, sink, NULL)) {
        g_printerr("Cannot link elements.\n");
        return -1;
    }

    // Getting src pad to catch output from pgie. Why?
    // GstPad *sink_pad = gst_element_get_static_pad(pgie, "src");
    // if (!sink_pad) {
    //     g_printerr("Cannot get src pad from pgie.\n");
    //     return -1;
    // }
    //
    // gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER, probe_callback, &H, NULL);
    // gst_object_unref(sink_pad);

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
