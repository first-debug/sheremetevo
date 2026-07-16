#include "gst/gstcaps.h"
#include "gst/gstelement.h"
#include "gst/gstelementfactory.h"
#include "gst/gstobject.h"
#include "gst/gstpad.h"
#include "gst/gststructure.h"
#include "gst/gstpipeline.h"
#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>

#include "gst/gstutils.h"

typedef struct {
    GMainLoop *loop;
    GstElement *pipeline;
} BusCallData;

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data);
static void cb_newpad(GstElement *bin, GstPad *pad, gpointer data);
static gboolean cb_removepad(GstElement *bin, GstPad *pad, gpointer data);
static GstElement *create_source_bin(gchar *uri, gint index);
static GstElement *create_rtsp_sink_bin(gchar *uri, gint index);

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    BusCallData *data_struct = (BusCallData *)data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("End of stream.\n");
            g_main_loop_quit(data_struct->loop);
            break;
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            g_printerr("Error: %s\n%s\n", error->message, debug);
            g_error_free(error);
            g_free(debug);
            g_main_loop_quit(data_struct->loop);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC (msg) != GST_OBJECT (data_struct->pipeline))
                return TRUE;

            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

            g_print("%-21s state has been changed from %s to %s, pending state %s\n",
                    msg->src->name, gst_element_state_get_name(old_state), gst_element_state_get_name(new_state),
                    gst_element_state_get_name(pending_state));
            break;
        }
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *structer = gst_message_get_structure(msg);
            if (structer == NULL)
                return TRUE;
            const gchar *elem_name = gst_structure_get_name(structer);
            GstObject *src_object = GST_MESSAGE_SRC(msg);

            gchar *src_name = gst_object_get_name(src_object);
            gchar *struct_str = gst_structure_to_string(structer);

            g_print("Catched MESSAGE_ELEMENT from = %s\n", src_name);
            g_print("%s\n", struct_str);
            g_free(src_name);
            g_free(struct_str);
            break;
        }
        default: {
        }
    }
    return TRUE;
}

static void cb_newpad(GstElement *bin, GstPad *pad, gpointer data) {
    GstElement *depay = (GstElement *)data;

    GstCaps *new_pad_caps = gst_pad_get_current_caps(pad);
    GstStructure *structure = gst_caps_get_structure(new_pad_caps, 0);
    gst_caps_unref(new_pad_caps);

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
}

static gboolean cb_removepad(GstElement *bin, GstPad *pad, gpointer data) {
    gchar *bin_name = GST_OBJECT_NAME(bin);
    gchar *pad_name = gst_pad_get_name(pad);

    g_print("Remove pad %s from %s\n", pad_name, bin_name);
    g_free(bin_name);
    g_free(pad_name);
    return TRUE;
}

// TODO: rtspsrc может обрабатывать несколько потоков, исходя из
//  этого - можно создавать бин только для конвертации rtp
//  пакетов в raw видео.
static GstElement *create_source_bin(gchar *uri, gint index) {
    GstElement *bin = NULL, *source = NULL, *depay = NULL,
               *decoder = NULL;
    gchar bin_name[16];
    snprintf(bin_name, 15, "source-bin-%1d", index);

    bin = gst_bin_new(bin_name);
    source = gst_element_factory_make("rtspsrc", "source");
    depay = gst_element_factory_make("rtph264depay", "depay");
    decoder = gst_element_factory_make("nvv4l2decoder", "decoder");

    if (!bin || !source || !depay || !decoder) {
        g_print("Cannot create rtsp source bin for uri = %s\n", uri);
        if (bin) gst_object_unref(bin);
        if (source) gst_object_unref(source);
        if (depay) gst_object_unref(depay);
        if (decoder) gst_object_unref(decoder);
        return NULL;
    }

    gst_bin_add_many(GST_BIN(bin), source, depay, decoder, NULL);

    g_object_set(G_OBJECT(source), "location", uri, NULL);
    // g_object_set(G_OBJECT(source), "use-buffering", TRUE, NULL);

    g_signal_connect(source, "pad-added", G_CALLBACK(cb_newpad), depay);
    g_signal_connect(source, "pad-removed", G_CALLBACK(cb_removepad), NULL);

    if (!gst_element_link_many(depay, decoder, NULL)){
        g_printerr("Cannot link elements.\n");
        gst_object_unref(bin);
        return NULL;
    }

    GstPad *decoder_src = gst_element_get_static_pad(decoder, "src");
    if (!decoder_src) {
        g_printerr ("Failed to get static pad of %s\n", gst_element_get_name(decoder));
        gst_object_unref(bin);
        return NULL;
    }

    GstPad *ghost_pad = gst_ghost_pad_new("src", decoder_src);
    gst_object_unref(decoder_src);

    if (!ghost_pad) {
        g_print("Failed to create ghost pad for %s\n", gst_element_get_name(bin));
        gst_object_unref(bin);
        return NULL;
    }

    if (!gst_element_add_pad(bin, ghost_pad)) {
        g_printerr ("Failed to add ghost pad in %s\n", gst_element_get_name(bin));
        gst_object_unref(bin);
        return NULL;
    }

    return bin;
}

static GstElement *create_rtsp_sink_bin(gchar *uri, gint index) {
    GstElement *bin = NULL, *encoder = NULL, *parser = NULL,
               *queue = NULL, *sink = NULL;
    gchar buffer[20];
    snprintf(buffer, 20, "sink-bin-%1d", index);

    bin = gst_bin_new(buffer);
    encoder = gst_element_factory_make("nvv4l2h264enc", "encoder");
    parser = gst_element_factory_make("h264parse", "parser");
    queue = gst_element_factory_make("queue", "queue");
    sink = gst_element_factory_make("rtspclientsink", "sink");

    if (!bin || !encoder || !parser || !queue || !sink) {
        g_print("Cannot create rtsp source bin for uri = %s", uri);
        if (bin) gst_object_unref(bin);
        if (encoder) gst_object_unref(encoder);
        if (parser) gst_object_unref(parser);
        if (queue) gst_object_unref(queue);
        if (sink) gst_object_unref(sink);
        return NULL;
    }

    gst_bin_add_many(GST_BIN(bin), encoder, parser, queue, sink, NULL);

    g_object_set(G_OBJECT(sink), "location", uri, NULL);

    GstPad *encoder_sink = gst_element_get_static_pad(encoder, "sink");
    GstPad *ghost_pad = gst_ghost_pad_new("sink", encoder_sink);
    gst_object_unref(encoder_sink);

    if (!ghost_pad) {
        g_print("Failed to create ghost pad for %s\n", gst_element_get_name(bin));
        gst_object_unref(bin);
        return NULL;
    }

    if (!gst_element_add_pad(bin, ghost_pad)) {
        g_printerr ("Failed to add ghost pad in %s\n", gst_element_get_name(bin));
        gst_object_unref(bin);
        return NULL;
    }

    if (!gst_element_link_many(encoder, parser, queue, NULL)){
        g_printerr("Cannot link elements.\n");
        gst_object_unref(bin);
        return NULL;
    }

    GstPad *queue_src = gst_element_get_static_pad(queue, "src");
    if (!queue_src) {
        g_printerr ("Failed to get static pad of %s\n", gst_element_get_name(queue));
        gst_object_unref(bin);
        return NULL;
    }

    snprintf(buffer, 17, "sink_%1d", index);
    GstPad *sink_pad = gst_element_request_pad_simple(sink, buffer);
    if (!sink_pad) {
        g_printerr ("Failed to request sink pad of %s\n", gst_element_get_name(sink));
        gst_object_unref(bin);
        return NULL;
    }

    if (gst_pad_link(queue_src, sink_pad) != GST_PAD_LINK_OK) {
        g_printerr("Cannot link %s and %s in the %s\n", gst_element_get_name(parser), gst_element_get_name(sink), gst_element_get_name(bin));
        gst_object_unref(bin);
        return NULL;
    }
    gst_object_unref(queue_src);
    gst_object_unref(sink_pad);

    return bin;
}

int main(int argc, char *argv[]) {
    GMainLoop *loop = NULL;
    GstElement *pipeline = NULL, *streammux = NULL, *pgie = NULL,
               *nvosd = NULL, *streamdemux = NULL,
               *sink = NULL;
    GstPad *src_pad = NULL, *sink_pad = NULL;
#ifdef SAVE_TO_FILE
    GstElement *encoder = NULL, *sink_parser = NULL, *mp4mux = NULL;
#endif
    GstBus *bus = NULL;
    guint bus_watch_id;

    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    if (argc < 3) {
        g_printerr("Usage: %s <path_to_config_file_nvinfer> <video_path1> <video_path2> ...\n", argv[0]);
        return -1;
    }

    // Elements initialization
    pipeline = gst_pipeline_new("sheremetevo");

    streammux = gst_element_factory_make("nvstreammux", "stream-muxer");
    pgie = gst_element_factory_make("nvinfer", "primary-infer");
    streamdemux = gst_element_factory_make("nvstreamdemux", "streamdemux");

    nvosd = gst_element_factory_make("nvdsosd", "nvosd");
#if SAVE_TO_FILE
    // TODO: switch media type from mp4 to another witch not need EOS to valid save.
    encoder = gst_element_factory_make("nvv4l2h264enc", "encoder");
    sink_parser = gst_element_factory_make("h264parse", "sink_parser");
    mp4mux = gst_element_factory_make("mp4mux", "mp4mux");
    sink = gst_element_factory_make("filesink", "sink");
#else
    sink = gst_element_factory_make("autovideosink", "sink");
    sink = create_rtsp_sink_bin("rtspt://localhost:8554/output", 0);
#endif

    // TODO: add all elements
    if (!pipeline || !streammux || !pgie || !streamdemux ||
            !nvosd || !sink) {
        g_printerr("Cannot create some modules.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(pipeline), streammux, pgie, streamdemux,
            nvosd, sink, NULL);
#if SAVE_TO_FILE
    gst_bin_add_many(GST_BIN(pipeline), encoder, sink_parser, mp4mux, NULL);
#endif

    // TODO: add resolution to config file
    g_object_set(G_OBJECT(streammux),
            "batch-size", 1,
            "width", 854,
            "height", 480, NULL);

    g_object_set(G_OBJECT(pgie), "config-file-path", argv[1], NULL);

#if SAVE_TO_FILE
    g_object_set(G_OBJECT(sink), "location", "media/output.mp4", NULL);
#endif

    // Dynamic linking
    GstElement *src_bin = create_source_bin(argv[2], 0);
    gst_bin_add(GST_BIN(pipeline), src_bin);

    src_pad = gst_element_get_static_pad(src_bin, "src");
    sink_pad = gst_element_request_pad_simple(streammux, "sink_0");

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

#if SAVE_TO_FILE
    src_pad = gst_element_get_static_pad(sink_parser, "src");
    sink_pad = gst_element_request_pad_simple(mp4mux, "video_0");

    if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
        g_printerr("Cannot link sink_parser and mp4mux.\n");
        gst_object_unref(src_pad);
        gst_object_unref(sink_pad);
        return -1;
    }
    gst_object_unref(src_pad);
    gst_object_unref(sink_pad);
#endif

    if (!gst_element_link_many(streammux, pgie, streamdemux, NULL)) {
        g_printerr("Cannot link elements.\n");
        return -1;
    }

#if SAVE_TO_FILE
    if (!gst_element_link_many(nvosd, encoder, sink_parser, NULL)) {
        g_printerr("Cannot link elements.\n");
        return -1;
    }
    if (!gst_element_link_many(mp4mux, sink, NULL)) {
        g_printerr("Cannot link mp4mux and sink.\n");
        return -1;
    }
#else
    if (!gst_element_link(nvosd, sink)) {
        g_printerr("Cannot link nvosd and sink.\n");
        return -1;
    }
#endif

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "sheremetevo");
    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    BusCallData data = { loop, pipeline };
    bus_watch_id = gst_bus_add_watch(bus, bus_call, &data);
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
