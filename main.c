#include "gst/gstcaps.h"
#include "gst/gstcapsfeatures.h"
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
    gchar *bin_name = gst_object_get_name(GST_OBJECT(bin));
    gchar *pad_name = gst_pad_get_name(pad);

    g_print("Remove pad %s from %s\n", pad_name, bin_name);
    g_free(bin_name);
    g_free(pad_name);
    return TRUE;
}

static GstElement *create_source_bin(gchar *uri, gint index) {
    GstElement *bin = NULL, *source = NULL, *depay = NULL,
               *decoder = NULL, *dewarp_converter = NULL, *dewarp_filter = NULL,
               *dewarper = NULL, *out_converter = NULL, *out_filter = NULL;
    GstCaps *caps = NULL;
    GstCapsFeatures *caps_feature = NULL;
    gchar bin_name[16];
    snprintf(bin_name, 15, "source-bin-%1d", index);

    bin = gst_bin_new(bin_name);
    source = gst_element_factory_make("rtspsrc", "source");
    depay = gst_element_factory_make("rtph264depay", "depay");
    decoder = gst_element_factory_make("nvv4l2decoder", "decoder");
    dewarp_converter = gst_element_factory_make("nvvideoconvert", "dewarp-converter");
    dewarp_filter = gst_element_factory_make("capsfilter", "dewarp_filter");
    dewarper = gst_element_factory_make("nvdewarper", "warper");
    out_converter = gst_element_factory_make("nvvideoconvert", "out-converter");
    out_filter = gst_element_factory_make("capsfilter", "out_filter");

    caps_feature = gst_caps_features_new("memory:NVMM", NULL);

    if (!bin || !source || !depay || !decoder || !dewarp_converter ||
            !dewarp_filter || !dewarper || !out_converter || !out_filter ||
            !caps_feature) {
        if (bin) gst_object_unref(bin);
        if (source) gst_object_unref(source);
        if (depay) gst_object_unref(depay);
        if (decoder) gst_object_unref(decoder);
        if (dewarp_converter) gst_object_unref(dewarp_converter);
        if (dewarp_filter) gst_object_unref(dewarp_filter);
        if (dewarper) gst_object_unref(dewarper);
        if (out_converter) gst_object_unref(out_converter);

        if (out_filter) gst_object_unref(out_filter);
        if (caps_feature) gst_caps_features_free(caps_feature);

        return NULL;
    }

    g_object_set(G_OBJECT(source), "location", uri, NULL);
    g_object_set(G_OBJECT(source), "rtp-blocksize", 1440, NULL);
    // g_object_set(G_OBJECT(source), "async-handling", TRUE, NULL);
    // g_object_set(G_OBJECT(source), "buffer-mode", 3, NULL);
    g_object_set(G_OBJECT(source), "latency", 200, NULL);
    // g_object_set(G_OBJECT(source), "drop-on-latency", TRUE, NULL);
    // g_object_set(G_OBJECT(source), "use-buffering", TRUE, NULL);

    g_object_set(G_OBJECT(decoder), "disable-dpb", TRUE, NULL);
    g_object_set(G_OBJECT(decoder), "discard-corrupted-frames", TRUE, NULL);
    g_object_set(G_OBJECT(decoder), "low-latency-mode", TRUE, NULL);

    caps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, "RGBA",
            NULL);
    gst_caps_set_features(caps, 0, caps_feature);
    g_object_set(G_OBJECT(dewarp_filter), "caps", caps, NULL);

    g_object_set(G_OBJECT(dewarper), "config-file", "configs/warper.toml", NULL);

    g_object_set(G_OBJECT(out_converter), "flip_method", 1, NULL);

    caps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, "NV12",
            "width", G_TYPE_INT, 1728,
            "height", G_TYPE_INT, 2752,
            NULL);
    gst_caps_set_features(caps, 0, caps_feature);
    g_object_set(G_OBJECT(out_filter), "caps", caps, NULL);

    g_signal_connect(source, "pad-added", G_CALLBACK(cb_newpad), depay);
    g_signal_connect(source, "pad-removed", G_CALLBACK(cb_removepad), NULL);

    gst_bin_add_many(GST_BIN(bin), source, depay, decoder, dewarp_converter, dewarp_filter, dewarper, out_converter, out_filter, NULL);

    if (!gst_element_link_many(depay, decoder, dewarp_converter, dewarp_filter, dewarper, out_converter, out_filter, NULL)){
        g_printerr("Cannot link internal elements in %s.\n", bin_name);
        gst_object_unref(bin);
        return NULL;
    }

    GstPad *bin_src = gst_element_get_static_pad(out_filter, "src");
    gchar *elem_name;
    if (!bin_src) {
        elem_name = gst_element_get_name(out_filter);
        g_printerr("Failed to get static pad of %s\n", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    GstPad *ghost_pad = gst_ghost_pad_new("src", bin_src);
    gst_object_unref(bin_src);

    if (!ghost_pad) {
        elem_name = gst_element_get_name(bin);
        g_print("Failed to create ghost pad for %s\n", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    if (!gst_element_add_pad(bin, ghost_pad)) {
        elem_name = gst_element_get_name(bin);
        g_printerr ("Failed to add ghost pad in %s\n", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    return bin;
}

static GstElement *create_rtsp_sink_bin(gchar *uri, gint index) {
    GstElement *bin = NULL, *encoder = NULL, *parser = NULL,
               *sink = NULL;
    gchar buffer[20];
    snprintf(buffer, 20, "sink-bin-%1d", index);

    bin = gst_bin_new(buffer);
    encoder = gst_element_factory_make("nvv4l2h264enc", "encoder");
    parser = gst_element_factory_make("h264parse", "parser");
    sink = gst_element_factory_make("rtspclientsink", "sink");

    if (!bin || !encoder || !parser || !sink) {
        g_print("Cannot create rtsp source bin for uri = %s\n", uri);
        if (bin) gst_object_unref(bin);
        if (encoder) gst_object_unref(encoder);
        if (parser) gst_object_unref(parser);
        if (sink) gst_object_unref(sink);

        return NULL;
    }

    gst_bin_add_many(GST_BIN(bin), encoder, parser, sink, NULL);

    g_object_set(G_OBJECT(sink), "location", uri, NULL);

    GstPad *encoder_sink = gst_element_get_static_pad(encoder, "sink");
    GstPad *ghost_pad = gst_ghost_pad_new("sink", encoder_sink);
    gst_object_unref(encoder_sink);

    gchar *elem_name;

    if (!ghost_pad) {
        elem_name = gst_element_get_name(bin);
        g_print("Failed to create ghost pad for %s\n", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    if (!gst_element_add_pad(bin, ghost_pad)) {
        elem_name = gst_element_get_name(bin);
        g_printerr ("Failed to add ghost pad in %s\n", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    if (!gst_element_link_many(encoder, parser, NULL)){
        g_printerr("Cannot link elements in rtsp sink for uri = %s.\n", uri);
        gst_object_unref(bin);
        return NULL;
    }

    GstPad *parser_src = gst_element_get_static_pad(parser, "src");
    if (!parser_src) {
        elem_name = gst_element_get_name(parser);
        g_printerr ("Failed to get static pad of %s\n", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    snprintf(buffer, 17, "sink_%1d", index);
    GstPad *sink_pad = gst_element_request_pad_simple(sink, buffer);
    if (!sink_pad) {
        elem_name = gst_element_get_name(sink);
        g_printerr ("Failed to request sink pad of %s\n", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    if (gst_pad_link(parser_src, sink_pad) != GST_PAD_LINK_OK) {
        elem_name = gst_element_get_name(parser);
        g_printerr ("Cannot link %s and ", elem_name);
        g_free(elem_name);

        elem_name = gst_element_get_name(sink);
        g_printerr("%s in the ", elem_name);
        g_free(elem_name);

        elem_name = gst_element_get_name(bin);
        g_printerr("%s\n", elem_name);
        g_free(elem_name);

        gst_object_unref(bin);
        return NULL;
    }
    gst_object_unref(parser_src);
    gst_object_unref(sink_pad);

    return bin;
}

int main(int argc, char *argv[]) {
    GMainLoop *loop = NULL;
    GstElement *pipeline = NULL, *streammux = NULL, *pgie = NULL,
               *nvosd = NULL, *tiler = NULL, *sink = NULL;
    GstPad *src_pad = NULL, *sink_pad = NULL;
#ifdef SAVE_TO_FILE
    GstElement *nvconv = NULL, *encoder = NULL, *sink_parser = NULL,
               *formatmux = NULL;
#endif
    GstBus *bus = NULL;
    guint bus_watch_id;

    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    if (argc < 3) {
        g_printerr("Usage: %s <path_to_config_file_nvinfer> <video_output_path> <video_path1> <video_path2> ...\n", argv[0]);
        return -1;
    }

    // Elements initialization
    pipeline = gst_pipeline_new("sheremetevo");

    streammux = gst_element_factory_make("nvstreammux", "stream-muxer");
    pgie = gst_element_factory_make("nvinfer", "primary-infer");
    tiler = gst_element_factory_make("nvmultistreamtiler", "tiler");

    nvosd = gst_element_factory_make("nvdsosd", "nvosd");
#if SAVE_TO_FILE
    nvconv = gst_element_factory_make("nvvideoconvert", "nvconv");
    encoder = gst_element_factory_make("nvv4l2h264enc", "encoder");
    sink_parser = gst_element_factory_make("h264parse", "sink_parser");
    formatmux = gst_element_factory_make("matroskamux", "formatmux");
    sink = gst_element_factory_make("filesink", "sink");
#else
    sink = create_rtsp_sink_bin(argv[2], 0);
#endif

    if (!pipeline || !streammux || !pgie || !tiler || !nvosd || !sink) {
        g_printerr("Cannot create some modules.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(pipeline), streammux, pgie,
            tiler, nvosd, sink, NULL);
#if SAVE_TO_FILE
    if (!nvconv || !encoder || !sink_parser || !formatmux) {
        g_printerr("Cannot create some modules for saving to file.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(pipeline), nvconv, encoder, sink_parser, formatmux, NULL);
#endif

    g_object_set(G_OBJECT(streammux),
            "batch-size", argc - 3,
            "width", 1728,
            "height", 2752,
            NULL);
            // "width", 854,
            // "height", 480, NULL);

    g_object_set(G_OBJECT(pgie), "config-file-path", argv[1], NULL);
    // 1728 2752
    // 3656 5504
    g_object_set(G_OBJECT(tiler),
            "width", 2720,
            "height", 4096, NULL);

    // TODO: add one more option to sink (file, display, rtsp, none)
#if SAVE_TO_FILE
    g_object_set(G_OBJECT(sink), "location", "media/output.mkv", NULL);
#else
    // g_object_set(G_OBJECT(sink), "sync", FALSE, NULL);
#endif

    // Dynamic linking
    gchar buffer[20];
    GstElement *src_bin = NULL;
    for (int i = 3, index; i < argc; i++) {
        index = i - 3;
        src_bin = create_source_bin(argv[i], index);

        if (src_bin == NULL) {
            g_printerr("Cannot create rtsp source bin for uri = %s\n", argv[i]);
            continue;
        }

        gst_bin_add_many(GST_BIN(pipeline), src_bin, NULL);

        snprintf(buffer, 17, "sink_%1d", index);
        src_pad = gst_element_get_static_pad(src_bin, "src");
        sink_pad = gst_element_request_pad_simple(streammux, buffer);

        if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
            g_printerr("Cannot link bin_src and streammux.\n");
            gst_object_unref(src_pad);
            gst_object_unref(sink_pad);
            return -1;
        }

        gst_object_unref(src_pad);
        gst_object_unref(sink_pad);
    }

    if (streammux->numsinkpads == 0) {
        g_printerr("Couldn't create at least one source.\n");
        return -1;
    }

#if SAVE_TO_FILE
    src_pad = gst_element_get_static_pad(sink_parser, "src");
    sink_pad = gst_element_request_pad_simple(formatmux, "video_0");

    if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
        g_printerr("Cannot link sink_parser and mp4mux.\n");
        gst_object_unref(src_pad);
        gst_object_unref(sink_pad);
        return -1;
    }
    gst_object_unref(src_pad);
    gst_object_unref(sink_pad);
#endif

    if (!gst_element_link_many(streammux, pgie, tiler, nvosd, NULL)) {
        g_printerr("Cannot link elements.\n");
        return -1;
    }

#if SAVE_TO_FILE
    if (!gst_element_link_many(nvosd, encoder, sink_parser, NULL)) {
        g_printerr("Cannot link elements.\n");
        return -1;
    }
    if (!gst_element_link_many(formatmux, sink, NULL)) {
        g_printerr("Cannot link mp4mux and sink.\n");
        return -1;
    }
#else
    if (!gst_element_link_many(nvosd, sink, NULL)) {
        g_printerr("Cannot link tiler and sink.\n");
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
