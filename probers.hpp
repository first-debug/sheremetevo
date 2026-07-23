#ifndef PROBERS_H
#define PROBERS_H

#include "gst/gstpad.h"
#include "pixel_transformer.hpp"
#include "udp_connection.hpp"

typedef struct {
    UdpConnection udp_conn;
    PixelGeoTransformer transformers[4];
} pgie_probe_data;

void set_probe(GstElement* element, const gchar *pad_name, GstPadProbeReturn (*prober) (GstPad * pad,
        GstPadProbeInfo * info,
        gpointer u_data),
        gpointer u_data);

GstPadProbeReturn print_nvmeta_probe(GstPad * pad,
        GstPadProbeInfo * info,
        gpointer u_data);

GstPadProbeReturn pgie_src_pad_buffer_probe(GstPad * pad,
        GstPadProbeInfo * info,
        gpointer u_data);

#endif // PROBERS_H
