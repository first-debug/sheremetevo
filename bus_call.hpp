#ifndef BUS_CALL_H
#define BUS_CALL_H

#include "gst/gstelement.h"
#include <gst/gst.h>

typedef struct {
    GMainLoop *loop;
    GstElement *pipeline;
} BusCallData;

gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data);

#endif // BUS_CALL_H
