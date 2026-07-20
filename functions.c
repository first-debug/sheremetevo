#include "gst/gstcaps.h"
#include "gst/gstcapsfeatures.h"
#include "gst/gststructure.h"
#include "gst/gstelement.h"
#include "gst/gstelementfactory.h"
#include "gst/gstobject.h"
#include "gst/gstpad.h"
#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>

#include "gst/gstutils.h"

#include "structs.h"

gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
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

void cb_newpad(GstElement *bin, GstPad *pad, gpointer data) {
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

gboolean cb_removepad(GstElement *bin, GstPad *pad, gpointer data) {
    gchar *bin_name = gst_object_get_name(GST_OBJECT(bin));
    gchar *pad_name = gst_pad_get_name(pad);

    g_print("Remove pad %s from %s\n", pad_name, bin_name);
    g_free(bin_name);
    g_free(pad_name);
    return TRUE;
}

GstElement *create_source_bin(gchar *uri, gint index) {
    GstElement *bin = NULL,
               *source = NULL, *depay = NULL, *decoder = NULL,
               *dewarp_converter = NULL, *dewarp_filter = NULL, *dewarper = NULL,
               *out_converter = NULL, *out_filter = NULL;
    GstCaps *dewarp_caps = NULL, *out_caps = NULL;
    gchar bin_name[16];
    snprintf(bin_name, 15, "source-bin-%1d", index);

    bin = gst_bin_new(bin_name);
    source = gst_element_factory_make("rtspsrc", "source");
    depay = gst_element_factory_make("rtph264depay", "depay");
    decoder = gst_element_factory_make("nvv4l2decoder", "decoder");
    dewarp_converter = gst_element_factory_make("nvvideoconvert", "dewarp-converter");
    dewarp_filter = gst_element_factory_make("capsfilter", "dewarp_filter");
    dewarp_caps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, "RGBA",
            "width", G_TYPE_INT, 1728,
            "height", G_TYPE_INT, 2752,
            NULL);
    dewarper = gst_element_factory_make("nvdewarper", "warper");
    out_converter = gst_element_factory_make("nvvideoconvert", "out-converter");
    out_filter = gst_element_factory_make("capsfilter", "out_filter");
    out_caps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, "NV12",
            "width", G_TYPE_INT, 1728,
            "height", G_TYPE_INT, 2752,
            NULL);

    if (!bin || !source || !depay || !decoder || !dewarp_converter || !dewarp_filter ||
            !dewarp_caps || !dewarper || !out_converter || !out_filter ||
            !out_caps) {
        if (bin) gst_object_unref(bin);
        if (source) gst_object_unref(source);
        if (depay) gst_object_unref(depay);
        if (decoder) gst_object_unref(decoder);

        if (dewarp_converter) gst_object_unref(dewarp_converter);
        if (dewarp_filter) gst_object_unref(dewarp_filter);
        if (dewarp_caps) gst_caps_unref(dewarp_caps);
        if (dewarper) gst_object_unref(dewarper);

        if (out_converter) gst_object_unref(out_converter);
        if (out_filter) gst_object_unref(out_filter);
        if (out_caps) gst_caps_unref(out_caps);

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

    g_object_set(G_OBJECT(dewarp_converter), "flip_method", 1, NULL);
    gst_caps_set_features(dewarp_caps, 0, gst_caps_features_new("memory:NVMM", NULL));
    g_object_set(G_OBJECT(dewarp_filter), "caps", dewarp_caps, NULL);
    g_object_set(G_OBJECT(dewarper),
            "config-file", "configs/warper.toml",
            "num-batch-buffers", 1,
            "num-output-buffers",1,
            NULL);


    gst_caps_set_features(out_caps, 0, gst_caps_features_new("memory:NVMM", NULL));
    g_object_set(G_OBJECT(out_filter), "caps", out_caps, NULL);

    g_signal_connect(source, "pad-added", G_CALLBACK(cb_newpad), depay);
    g_signal_connect(source, "pad-removed", G_CALLBACK(cb_removepad), NULL);

    gst_bin_add_many(GST_BIN(bin), source, depay, decoder, dewarp_converter,
            dewarp_filter, dewarper, out_converter, out_filter, NULL);


    if (!gst_element_link_many(depay, decoder, dewarp_converter,
                dewarp_filter, dewarper, out_converter, out_filter, NULL)){
        g_printerr("Cannot link internal elements in %s.\n", bin_name);
        gst_object_unref(bin);
        return NULL;
    }

    GstPad *bin_src = gst_element_get_static_pad(out_filter, "src");
    gchar *elem_name;
    if (!bin_src) {
        elem_name = gst_element_get_name(out_filter);
        g_printerr("Failed to get pad of %s\n", elem_name);

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

GstElement *create_rtsp_sink_bin(gchar *uri, gint index) {
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
        g_printerr ("Failed to get pad of %s\n", elem_name);

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

