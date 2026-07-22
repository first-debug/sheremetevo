#ifndef PROBERS_H
#define PROBERS_H

#include "gst/gstpad.h"

#include "gstnvdsmeta.h"
#include "nvdsmeta.h"

void set_probe(GstElement* element, gchar *pad_name, GstPadProbeReturn (*prober) (GstPad * pad,
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
