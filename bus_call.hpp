#ifndef BUS_CALL_H
#define BUS_CALL_H

#include "gst/gstelement.h"
#include <gst/gst.h>

#include "output_manager.hpp"
#include "source_manager.hpp"

typedef struct {
    GMainLoop *loop;
    GstElement *pipeline;
    // ошибки камер передаются сюда для переподключения
    SourceManager *sources;
    // ошибки видеовыхода передаются сюда для перезапуска его ветки
    OutputManager *outputs;
} BusCallData;

gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data);

#endif // BUS_CALL_H
