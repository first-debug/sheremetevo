#include "gst/gstbin.h"
#include "gst/gstclock.h"
#include "gst/gstghostpad.h"
#include "gst/gstutils.h"
#include <stdio.h>

#include "callbacks.hpp"
#include "contexts.hpp"
#include "custom_bins.hpp"

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
    g_object_set(G_OBJECT(source), "latency", 200, NULL);

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

GstElement *create_sink_bin(gchar *uri, gint index) {
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

gboolean attach_source(AppCtx *ctx, guint index) {
    SourceCtx *src = &ctx->sources[index];
    gchar padname[16];

    GstElement *bin = create_source_bin(src->uri, (gint)index);
    if (!bin) {
        g_printerr("[source %u] create_source_bin() return NULL\n", index);
        return FALSE;
    }

    if (!gst_bin_add(GST_BIN(ctx->pipeline), bin)) {
        g_printerr("[source %u] Cannot addd new src bin to pipeline\n", index);
        gst_object_unref(bin);
        return FALSE;
    }

    g_snprintf(padname, sizeof(padname), "sink_%u", index);
    GstPad *mux_sinkpad = gst_element_request_pad_simple(ctx->streammux, padname);
    if (!mux_sinkpad) {
        g_printerr("[source %u] Cannot get pad '%s' from streammux\n", index, padname);
        gst_bin_remove(GST_BIN(ctx->pipeline), bin);
        return FALSE;
    }

    GstPad *bin_srcpad = gst_element_get_static_pad(bin, "src");
    if (gst_pad_link(bin_srcpad, mux_sinkpad) != GST_PAD_LINK_OK) {
        g_printerr("[source %u] Cannot link bin src and streammux\n", index);
        gst_object_unref(bin_srcpad);
        gst_element_release_request_pad(ctx->streammux, mux_sinkpad);
        gst_object_unref(mux_sinkpad);
        gst_bin_remove(GST_BIN(ctx->pipeline), bin);
        return FALSE;
    }

    gst_object_unref(bin_srcpad);

    src->bin = bin;
    src->mux_sinkpad = mux_sinkpad;
    src->last_buffer_time = g_get_monotonic_time();

    gst_element_sync_state_with_parent(bin);

    src->mutex.lock();
    src->reconnecting = FALSE;
    src->mutex.unlock();

    ctx->num_sources++;
    g_print("[source %u] Created source: %s\n", index, src->uri);
    return TRUE;
}

gboolean retry_attach_cb(gpointer data)
{
    RetryArg *r = (RetryArg *)data;
    if (!attach_source(r->app, r->index)) {
        g_timeout_add_seconds(RECONNECT_RETRY_DELAY_SEC, retry_attach_cb, r);
        return G_SOURCE_REMOVE;
    }
    g_free(r);
    return G_SOURCE_REMOVE;
}

void do_reconnect_source(AppCtx *app, guint index)
{
    SourceCtx *src = &app->sources[index];

    src->mutex.lock();
    if (src->reconnecting) {
        src->mutex.unlock();
        return;
    }
    src->reconnecting = TRUE;
        src->mutex.unlock();

    g_print("[source %u] reconnecting...\n", index);

    if (src->bin) {
        gst_element_set_state(src->bin, GST_STATE_NULL);
        gst_element_get_state(src->bin, NULL, NULL, 3 * GST_SECOND);
        gst_bin_remove(GST_BIN(app->pipeline), src->bin); // отдаёт последний ref, bin разрушится
        src->bin = NULL;
    }

    if (src->mux_sinkpad) {
        gst_element_release_request_pad(app->streammux, src->mux_sinkpad);
        gst_object_unref(src->mux_sinkpad);
        src->mux_sinkpad = NULL;
    }

    if (!attach_source(app, index)) {
        RetryArg *r = g_new0(RetryArg, 1);
        r->app = app; r->index = index;
        g_timeout_add_seconds(RECONNECT_RETRY_DELAY_SEC, retry_attach_cb, r);
    }
}

