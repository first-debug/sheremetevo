#include "gst/gstbin.h"
#include "gst/gstghostpad.h"
#include "gst/gstutils.h"
#include <stdio.h>
#include <vector>

#include "callbacks.hpp"
#include "custom_bins.hpp"
#include "log.hpp"

// Создаёт элемент и сразу кладёт его в bin, чтобы при ошибке достаточно
// было освободить только bin.
static GstElement *add_element(GstElement *bin, const gchar *factory, const gchar *name) {
    GstElement *element = gst_element_factory_make(factory, name);
    if (element == NULL) {
        LOG_ERROR("PIPELINE", "Cannot create element %s (%s).", name, factory);
        return NULL;
    }
    gst_bin_add(GST_BIN(bin), element);
    return element;
}

static GstElement *add_nvmm_capsfilter(GstElement *bin, const gchar *name,
        const gchar *format, gint width, gint height) {
    GstElement *filter = add_element(bin, "capsfilter", name);
    if (filter == NULL) {
        return NULL;
    }

    GstCaps *caps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, format, NULL);
    if (width > 0 && height > 0) {
        gst_caps_set_simple(caps,
                "width", G_TYPE_INT, width,
                "height", G_TYPE_INT, height, NULL);
    }
    gst_caps_set_features(caps, 0, gst_caps_features_new("memory:NVMM", NULL));
    g_object_set(G_OBJECT(filter), "caps", caps, NULL);
    gst_caps_unref(caps);
    return filter;
}

GstElement *create_source_bin(const gchar *uri, gint index, const SourceTransform &transform,
        gint width, gint height) {
    GstElement *bin = NULL, *source = NULL, *depay = NULL, *decoder = NULL;
    gchar bin_name[16];
    snprintf(bin_name, 15, "source-bin-%1d", index);

    bin = gst_bin_new(bin_name);
    if (bin == NULL) {
        return NULL;
    }

    source = add_element(bin, "rtspsrc", "source");
    depay = add_element(bin, "rtph264depay", "depay");
    decoder = add_element(bin, "nvv4l2decoder", "decoder");

    if (!source || !depay || !decoder) {
        gst_object_unref(bin);
        return NULL;
    }

    g_object_set(G_OBJECT(source), "location", uri, NULL);
    g_object_set(G_OBJECT(source), "rtp-blocksize", 1440, NULL);
    g_object_set(G_OBJECT(source), "latency", 200, NULL);

    g_object_set(G_OBJECT(decoder), "disable-dpb", TRUE, NULL);
    g_object_set(G_OBJECT(decoder), "discard-corrupted-frames", TRUE, NULL);
    g_object_set(G_OBJECT(decoder), "low-latency-mode", TRUE, NULL);

    g_signal_connect(source, "pad-added", G_CALLBACK(cb_newpad), depay);
    g_signal_connect(source, "pad-removed", G_CALLBACK(cb_removepad), NULL);

    // Цепочка после декодера собирается из того, что задано в конфиге камеры:
    // поворот (nvvideoconvert flip-method) и/или выпрямление (nvdewarper).
    // Если ничего не задано, декодер подключается к nvstreammux напрямую,
    // масштабирование до размера пачки делает сам nvstreammux.
    std::vector<GstElement *> chain = { depay, decoder };
    bool has_flip = transform.flip_method >= 0;
    bool has_dewarp = !transform.dewarper_config.empty();

    if (has_flip || has_dewarp) {
        GstElement *converter = add_element(bin, "nvvideoconvert", "transform-converter");
        if (converter == NULL) {
            gst_object_unref(bin);
            return NULL;
        }
        if (has_flip) {
            g_object_set(G_OBJECT(converter), "flip-method", transform.flip_method, NULL);
        }
        chain.push_back(converter);
    }

    if (has_dewarp) {
        // nvdewarper принимает RGBA, после него возвращаемся в NV12
        GstElement *dewarp_filter = add_nvmm_capsfilter(bin, "dewarp-filter", "RGBA", width, height);
        GstElement *dewarper = add_element(bin, "nvdewarper", "dewarper");
        GstElement *out_converter = add_element(bin, "nvvideoconvert", "out-converter");
        GstElement *out_filter = add_nvmm_capsfilter(bin, "out-filter", "NV12", width, height);

        if (!dewarp_filter || !dewarper || !out_converter || !out_filter) {
            gst_object_unref(bin);
            return NULL;
        }

        g_object_set(G_OBJECT(dewarper),
                "config-file", transform.dewarper_config.c_str(),
                "num-batch-buffers", 1,
                "num-output-buffers", 1,
                NULL);

        chain.insert(chain.end(), { dewarp_filter, dewarper, out_converter, out_filter });
    } else if (has_flip) {
        GstElement *out_filter = add_nvmm_capsfilter(bin, "out-filter", "NV12", 0, 0);
        if (out_filter == NULL) {
            gst_object_unref(bin);
            return NULL;
        }
        chain.push_back(out_filter);
    }

    for (size_t i = 1; i < chain.size(); i++) {
        if (!gst_element_link(chain[i - 1], chain[i])) {
            gchar *a = gst_element_get_name(chain[i - 1]);
            gchar *b = gst_element_get_name(chain[i]);
            LOG_ERROR("PIPELINE", "Cannot link %s and %s in %s.", a, b, bin_name);
            g_free(a);
            g_free(b);
            gst_object_unref(bin);
            return NULL;
        }
    }

    GstElement *last = chain.back();
    GstPad *bin_src = gst_element_get_static_pad(last, "src");
    gchar *elem_name;
    if (!bin_src) {
        elem_name = gst_element_get_name(last);
        LOG_ERROR("PIPELINE", "Failed to get pad of %s", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    GstPad *ghost_pad = gst_ghost_pad_new("src", bin_src);
    gst_object_unref(bin_src);

    if (!ghost_pad) {
        elem_name = gst_element_get_name(bin);
        LOG_ERROR("PIPELINE", "Failed to create ghost pad for %s", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    if (!gst_element_add_pad(bin, ghost_pad)) {
        elem_name = gst_element_get_name(bin);
        LOG_ERROR("PIPELINE", "Failed to add ghost pad in %s", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    return bin;
}

GstElement *create_sink_bin(const gchar *uri, gint index) {
    GstElement *bin = NULL, *encoder = NULL, *parser = NULL,
               *sink = NULL;
    gchar buffer[20];
    snprintf(buffer, 20, "sink-bin-%1d", index);

    bin = gst_bin_new(buffer);
    encoder = gst_element_factory_make("nvv4l2h264enc", "encoder");
    parser = gst_element_factory_make("h264parse", "parser");
    sink = gst_element_factory_make("rtspclientsink", "sink");

    if (!bin || !encoder || !parser || !sink) {
        LOG_ERROR("PIPELINE", "Cannot create rtsp sink bin for uri = %s", uri);
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
        LOG_ERROR("PIPELINE", "Failed to create ghost pad for %s", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    if (!gst_element_add_pad(bin, ghost_pad)) {
        elem_name = gst_element_get_name(bin);
        LOG_ERROR("PIPELINE", "Failed to add ghost pad in %s", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    if (!gst_element_link_many(encoder, parser, NULL)){
        LOG_ERROR("PIPELINE", "Cannot link elements in rtsp sink for uri = %s.", uri);
        gst_object_unref(bin);
        return NULL;
    }

    GstPad *parser_src = gst_element_get_static_pad(parser, "src");
    if (!parser_src) {
        elem_name = gst_element_get_name(parser);
        LOG_ERROR("PIPELINE", "Failed to get pad of %s", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    snprintf(buffer, 17, "sink_%1d", index);
    GstPad *sink_pad = gst_element_request_pad_simple(sink, buffer);
    if (!sink_pad) {
        elem_name = gst_element_get_name(sink);
        LOG_ERROR("PIPELINE", "Failed to request sink pad of %s", elem_name);

        g_free(elem_name);
        gst_object_unref(bin);
        return NULL;
    }

    if (gst_pad_link(parser_src, sink_pad) != GST_PAD_LINK_OK) {
        LOG_ERROR("PIPELINE", "Cannot link %s and %s in the %s", GST_ELEMENT_NAME(parser),
                GST_ELEMENT_NAME(sink), GST_ELEMENT_NAME(bin));

        gst_object_unref(bin);
        return NULL;
    }
    gst_object_unref(parser_src);
    gst_object_unref(sink_pad);

    return bin;
}

