#ifndef PROBERS_H
#define PROBERS_H

#include "gst/gstpad.h"

#include "gstnvdsmeta.h"
#include "nvdsmeta.h"

void set_prober(GstElement* element, gchar *pad_name);

GstPadProbeReturn meta_prober(GstPad * pad,
        GstPadProbeInfo * info,
        gpointer u_data);

#endif // PROBERS_H
