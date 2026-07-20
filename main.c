#include "gst/gstelement.h"
#include "gst/gstelementfactory.h"
#include "gst/gstobject.h"
#include "gst/gstpad.h"
#include "gst/gstpipeline.h"
#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>

#include "gst/gstutils.h"

#include "structs.h"

gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data);
void cb_newpad(GstElement *bin, GstPad *pad, gpointer data);
gboolean cb_removepad(GstElement *bin, GstPad *pad, gpointer data);
GstElement *create_source_bin(gchar *uri, gint index);
GstElement *create_sink_bin(gchar *uri, gint index);

int main(int argc, char *argv[]) {
    GMainLoop *loop = NULL;
    GstElement *pipeline = NULL, *streammux = NULL, *pgie = NULL,
               *nvosd = NULL, *tiler = NULL, *sink = NULL;
    GstPad *src_pad = NULL, *sink_pad = NULL;
    GstBus *bus = NULL;
    guint bus_watch_id;

    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    if (argc < 3) {
        g_printerr("Usage: %s <path_to_config_file_nvinfer> <video_output_path> <video_path1> <video_path2> ...\n", argv[0]);
        return -1;
    }

    // Elements initialization
    pipeline = gst_pipeline_new("sheremetevo");

    streammux = gst_element_factory_make("nvstreammux", "stream-muxer");
    pgie = gst_element_factory_make("nvinfer", "primary-infer");
    tiler = gst_element_factory_make("nvmultistreamtiler", "tiler");

    nvosd = gst_element_factory_make("nvdsosd", "nvosd");
    sink = create_sink_bin(argv[2], 0);

    if (!pipeline || !streammux || !pgie || !tiler || !nvosd || !sink) {
        g_printerr("Cannot create some modules.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(pipeline), streammux, pgie,
            tiler, nvosd, sink, NULL);

    g_object_set(G_OBJECT(streammux),
            "batch-size", argc - 3,
            "width", 1728,
            "height", 2752,
            NULL);
            // "width", 854,
            // "height", 480, NULL);

    g_object_set(G_OBJECT(pgie), "config-file-path", argv[1], NULL);
    // 1728 2752
    // 3656 5504
    g_object_set(G_OBJECT(tiler),
            "width", 2720,
            "height", 4096, NULL);

    // Dynamic linking
    gchar buffer[20];
    GstElement *src_bin = NULL;
    for (int i = 3, index; i < argc; i++) {
        index = i - 3;
        src_bin = create_source_bin(argv[i], index);

        if (src_bin == NULL) {
            g_printerr("Cannot create rtsp source bin for uri = %s\n", argv[i]);
            continue;
        }

        gst_bin_add_many(GST_BIN(pipeline), src_bin, NULL);

        snprintf(buffer, 17, "sink_%1d", index);
        src_pad = gst_element_get_static_pad(src_bin, "src");
        sink_pad = gst_element_request_pad_simple(streammux, buffer);

        if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
            g_printerr("Cannot link bin_src and streammux.\n");
            gst_object_unref(src_pad);
            gst_object_unref(sink_pad);
            return -1;
        }

        gst_object_unref(src_pad);
        gst_object_unref(sink_pad);
    }

    if (streammux->numsinkpads == 0) {
        g_printerr("Couldn't create at least one source.\n");
        return -1;
    }

    if (!gst_element_link_many(streammux, pgie, tiler, nvosd, NULL)) {
        g_printerr("Cannot link elements.\n");
        return -1;
    }

    }
        return -1;
    }

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "sheremetevo");
    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    BusCallData data = { loop, pipeline };
    bus_watch_id = gst_bus_add_watch(bus, bus_call, &data);
    gst_object_unref(bus);

    g_print("\nStarting pipline...\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline));
    g_source_remove(bus_watch_id);
    g_main_loop_unref(loop);

    return 0;
}
