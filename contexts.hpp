#ifndef CONTEXTS_H
#define CONTEXTS_H

#include "gst/gstelement.h"
#include <mutex>


#define RECONNECT_RETRY_DELAY_SEC  3000   // пауза перед повторной попыткой, если пересоздание не удалось

typedef struct _SourceCtx {
    GstElement *bin;            // текущий source-bin (создаётся через create_source_bin)
    GstPad     *mux_sinkpad;    // запрошенный sink_N pad у streammux
    gchar      *uri;
    guint       index;
    glong       last_buffer_time; // g_get_monotonic_time(), под мьютексом
    gboolean    reconnecting;     // под мьютексом
    std::mutex  mutex;
} SourceCtx;

typedef struct _AppCtx {
    GMainLoop  *loop;
    GstElement *pipeline;
    GstElement *streammux;
    SourceCtx sources[4];
    guint num_sources;
    gboolean shutting_down;
} AppCtx;

#endif // CONTEXTS_H

