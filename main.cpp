#include "glib.h"
#include "gst/gst.h"
#include "gst/gstelement.h"
#include <cstdio>

#include "bus_call.hpp"
#include "custom_bins.hpp"
#include "probers.hpp"
#include "splited_points.hpp"
#include "udp_connection.hpp"

int main(int argc, char *argv[]) {
    GMainLoop *loop = NULL;
    GstElement *pipeline = NULL,
               *streammux = NULL,
               *pgie = NULL,
               *nvosd = NULL,
               *demux = NULL;
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

    demux = gst_element_factory_make("nvstreamdemux", "demux");

    if (!pipeline || !streammux || !pgie || !demux) {
        g_printerr("Cannot create some modules.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(pipeline), streammux, pgie, demux, NULL);

    g_object_set(G_OBJECT(streammux),
            "batch-size", argc - 3,
            "width", 1728,
            "height", 2752,
            NULL);
            // "width", 854,
            // "height", 480, NULL);

    g_object_set(G_OBJECT(pgie), "config-file-path", argv[1], NULL);

    gchar buffer[40];
    GstElement *new_bin = NULL;
    for (int i = 3, index; i < argc; i++) {
        index = i - 3;
        new_bin = create_source_bin(argv[i], index);

        if (new_bin == NULL) {
            g_printerr("Cannot create rtsp source bin for uri = %s\n", argv[i]);
            continue;
        }

        gst_bin_add_many(GST_BIN(pipeline), new_bin, NULL);

        snprintf(buffer, 17, "sink_%1d", index);
        src_pad = gst_element_get_static_pad(new_bin, "src");
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

    if (!gst_element_link_many(streammux, pgie, demux, NULL)) {
        g_printerr("Cannot link elements.\n");
        return -1;
    }

    for (int i = 3, index; i < argc; i++) {
        index = i - 3;
        snprintf(buffer, 39, "%s_%1d", argv[2], index);
        new_bin = create_sink_bin(buffer, index);
        snprintf(buffer, 39, "nvosd_%1d", index);
        nvosd = gst_element_factory_make("nvdsosd", buffer);

        if (new_bin == NULL || nvosd == NULL) {
            g_printerr("Cannot create rtsp sink bin for uri = %s\n", argv[i]);
            continue;
        }

        gst_bin_add_many(GST_BIN(pipeline), nvosd, new_bin, NULL);

        snprintf(buffer, 17, "src_%1d", index);
        src_pad = gst_element_request_pad_simple(demux, buffer);
        sink_pad = gst_element_get_static_pad(nvosd, "sink");

        if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
            g_printerr("Cannot link demux and nvosd.\n");
            gst_object_unref(src_pad);
            gst_object_unref(sink_pad);
            gst_bin_remove_many(GST_BIN(pipeline), nvosd, new_bin, NULL);
            return -1;
        }

        if (!gst_element_link(nvosd, new_bin)) {
            g_printerr("Cannot link nvosd and new sink bin.\n");
            gst_object_unref(src_pad);
            gst_object_unref(sink_pad);
            gst_bin_remove_many(GST_BIN(pipeline), nvosd, new_bin, NULL);
            return -1;
        }

        gst_object_unref(src_pad);
        gst_object_unref(sink_pad);
    }

    if (demux->numsrcpads == 0) {
        g_printerr("Couldn't create at least one sink.\n");
        return -1;
    }

    UdpConnection udp_conn("192.168.10.185", 6767);

    points_struct points;
    init_points(points);

    pgie_probe_data probe_data = {
        &udp_conn,
        {
            PixelGeoTransformer(points.pixels_cam1, points.geo_cam1),
            PixelGeoTransformer(points.pixels_cam2, points.geo_cam2),
            PixelGeoTransformer(points.pixels_cam3, points.geo_cam3),
            PixelGeoTransformer(points.pixels_cam4, points.geo_cam4),
        }
    };

    set_probe(pgie, "src", pgie_src_pad_buffer_probe, &probe_data);

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
