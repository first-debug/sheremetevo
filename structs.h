#include "gst/gstelement.h"
#include <gst/gst.h>

typedef struct {
    GMainLoop *loop;
    GstElement *pipeline;
} BusCallData;

