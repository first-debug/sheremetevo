#ifndef CUSTOM_BINS_H
#define CUSTOM_BINS_H

#include "gst/gstelement.h"

GstElement *create_source_bin(gchar *uri, gint index);
GstElement *create_rtsp_sink_bin(gchar *uri, gint index);

#endif // CUSTOM_BINS_H
