#ifndef CUSTOM_BINS_H
#define CUSTOM_BINS_H

#include "gst/gstelement.h"

#include "contexts.hpp"

typedef struct { AppCtx *app; guint index; } RetryArg;

GstElement *create_source_bin(gchar *uri, gint index);
GstElement *create_sink_bin(gchar *uri, gint index);
gboolean attach_source(AppCtx *app, guint index);
gboolean retry_attach_cb(gpointer data);
void do_reconnect_source(AppCtx *app, guint ssrc);

#endif // CUSTOM_BINS_H
