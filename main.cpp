#include "glib.h"
#include "gst/gst.h"
#include "gst/gstelement.h"
#include <glib-unix.h>
#include <csignal>
#include <unistd.h>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "bus_call.hpp"
#include "custom_bins.hpp"
#include "app_config.hpp"
#include "health.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "points_sender.hpp"
#include "probers.hpp"
#include "source_manager.hpp"
#include "udp_connection.hpp"

typedef struct {
    GMainLoop *loop;
    GstElement *pipeline;
    SourceManager *sources;
    gboolean eos_sent;
} SignalData;

// Сколько ждать прохождения EOS по конвейеру и перехода в NULL, секунд
static const guint EOS_TIMEOUT_SEC = 3;
static const guint SHUTDOWN_TIMEOUT_SEC = 10;

static gboolean on_eos_timeout(gpointer user_data) {
    SignalData *data = (SignalData *)user_data;
    LOG_WARN("APP", "EOS did not reach the sinks in %u s, stopping anyway", EOS_TIMEOUT_SEC);
    g_main_loop_quit(data->loop);
    return G_SOURCE_REMOVE;
}

// Первый SIGINT/SIGTERM — отправить EOS, чтобы корректно закрыть энкодеры и
// RTSP-сессии; если EOS не дойдёт за EOS_TIMEOUT_SEC (конвейер завис), выходим
// всё равно. Второй сигнал — выйти сразу.
static gboolean on_terminate_signal(gpointer user_data) {
    SignalData *data = (SignalData *)user_data;
    if (!data->eos_sent) {
        LOG_INFO("APP", "stopping pipeline (send the signal again to force quit)");
        data->eos_sent = TRUE;
        source_manager_shutdown(*data->sources);
        gst_element_send_event(data->pipeline, gst_event_new_eos());
        g_timeout_add_seconds(EOS_TIMEOUT_SEC, on_eos_timeout, data);
    } else {
        g_main_loop_quit(data->loop);
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char *argv[]) {
    GMainLoop *loop = NULL;
    GstElement *pipeline = NULL,
               *streammux = NULL,
               *pgie = NULL,
               *nvosd = NULL,
               *tracker = NULL,
               *demux = NULL;
    GstPad *src_pad = NULL, *sink_pad = NULL;
    GstBus *bus = NULL;
    guint bus_watch_id;

    gst_init(&argc, &argv);

    // Построчная буферизация stdout: иначе при выводе в файл или через tee
    // строки журнала и вывод DeepStream теряются при падении.
    setvbuf(stdout, NULL, _IOLBF, 0);
    // С этого момента журнал работает; g_print/g_printerr сторонних библиотек тоже идут в него
    log_init();

    if (argc != 2) {
        LOG_ERROR("APP", "usage: %s <path_to_app_config>", argv[0]);
        return -1;
    }

    AppConfig config;
    std::string error;
    if (!load_app_config(argv[1], config, error)) {
        LOG_ERROR("CONFIG", "cannot load config: %s", error.c_str());
        return -1;
    }

    LogConfig log_config;
    log_level_from_string(config.log_level, log_config.level);
    log_config.json = config.log_format == "json";
    log_config.file = config.log_file;
    log_config.max_size_mb = static_cast<guint>(config.log_max_size_mb);
    log_config.max_files = static_cast<guint>(config.log_max_files);
    if (!log_configure(log_config, error)) {
        LOG_ERROR("LOG", "cannot open log file %s, logging to stdout/stderr only", error.c_str());
    }

    const int num_sources = static_cast<int>(config.cameras.size());

    // Калибровку загружаем до сборки конвейера, чтобы ошибка в точках
    // обнаруживалась сразу, а не после подключения к камерам.
    UdpConnection udp_conn(config.udp_host, static_cast<uint16_t>(config.udp_port));
    if (!udp_conn.is_open()) {
        LOG_ERROR("UDP", "cannot open UDP connection to %s:%d", config.udp_host.c_str(), config.udp_port);
        return -1;
    }

    // Объявлен после udp_conn: разрушается раньше и успевает отправить остаток
    PointsSender sender(udp_conn, config.udp_batch_interval_ms,
            static_cast<size_t>(config.udp_max_datagram_bytes));

    pgie_probe_data probe_data;
    probe_data.sender = &sender;
    probe_data.min_aspect_ratio = config.min_aspect_ratio;
    probe_data.transformers.reserve(num_sources);

    for (int cam = 0; cam < num_sources; cam++) {
        std::vector<cv::Point2d> pixels, geo;
        if (!load_calibration_points(config.cameras[cam].calibration_file, pixels, geo, error)) {
            LOG_CAM_ERROR("CALIB", cam, "calibration failed: %s", error.c_str());
            return -1;
        }

        try {
            probe_data.transformers.emplace_back(pixels, geo, config.ransac_threshold_m);
        } catch (const std::exception &e) {
            LOG_CAM_ERROR("CALIB", cam, "calibration failed: %s", e.what());
            return -1;
        }

        const auto &st = probe_data.transformers.back().reprojection_stats();
        LOG_CAM_INFO("CALIB", cam, "inliers %zu/%zu, reprojection error median %.1f m, p90 %.1f m, max %.1f m",
                st.inliers, st.points, st.median_m, st.p90_m, st.max_m);
    }

    probe_data.zones.resize(num_sources);
    for (int cam = 0; cam < num_sources; cam++) {
        const std::string &zone_file = config.cameras[cam].zone_file;
        if (zone_file.empty()) {
            continue;
        }
        if (!load_geo_zone(zone_file, probe_data.zones[cam], error)) {
            LOG_CAM_ERROR("ZONE", cam, "cannot load zone: %s", error.c_str());
            return -1;
        }
        LOG_CAM_INFO("ZONE", cam, "%zu vertices, area %.0f m2; points outside are not sent",
                probe_data.zones[cam].vertex_count(), probe_data.zones[cam].area_m2());
    }

    loop = g_main_loop_new(NULL, FALSE);

    // Elements initialization
    pipeline = gst_pipeline_new("sheremetevo");

    streammux = gst_element_factory_make("nvstreammux", "stream-muxer");
    pgie = gst_element_factory_make("nvinfer", "primary-infer");

    if (config.output_enabled) {
        demux = gst_element_factory_make("nvstreamdemux", "demux");
    } else {
        // Без видеовыхода разметка и кодирование не нужны: кадры уходят в fakesink.
        demux = gst_element_factory_make("fakesink", "sink");
        if (demux) {
            g_object_set(G_OBJECT(demux), "sync", FALSE, "async", FALSE, NULL);
        }
    }

    if (!pipeline || !streammux || !pgie || !demux) {
        LOG_ERROR("PIPELINE", "cannot create nvstreammux, nvinfer or output elements");
        return -1;
    }

    gst_bin_add_many(GST_BIN(pipeline), streammux, pgie, demux, NULL);

    if (config.tracker_enabled) {
        tracker = gst_element_factory_make("nvtracker", "tracker");
        if (!tracker) {
            LOG_ERROR("PIPELINE", "cannot create nvtracker");
            return -1;
        }
        g_object_set(G_OBJECT(tracker),
                "ll-lib-file", config.tracker_lib.c_str(),
                "ll-config-file", config.tracker_config.c_str(),
                "tracker-width", config.tracker_width,
                "tracker-height", config.tracker_height,
                NULL);
        gst_bin_add(GST_BIN(pipeline), tracker);
    }

    // Последний элемент анализа: после него метаданные объектов уже окончательные
    GstElement *analytics_tail = tracker ? tracker : pgie;

    // batched-push-timeout: nvstreammux отправляет пачку, не дожидаясь
    // отставшей или упавшей камеры, чтобы остальные камеры продолжали работать.
    g_object_set(G_OBJECT(streammux),
            "batch-size", num_sources,
            "width", config.frame_width,
            "height", config.frame_height,
            "batched-push-timeout", config.batched_push_timeout_us,
            NULL);

    g_object_set(G_OBJECT(pgie), "config-file-path", config.nvinfer_config.c_str(), NULL);

    SourceManager sources;
    sources.stall_timeout_sec = config.stall_timeout_sec;
    sources.reconnect_interval_sec = config.reconnect_interval_sec;

    gchar buffer[40];
    GstElement *new_bin = NULL;
    for (int index = 0; index < num_sources; index++) {
        const CameraConfig &camera = config.cameras[index];
        SourceTransform transform;
        transform.flip_method = camera.flip_method;
        transform.dewarper_config = camera.dewarper_config;

        new_bin = create_source_bin(camera.uri.c_str(), index, transform,
                config.frame_width, config.frame_height);

        // Номер входа связывает камеру с её калибровкой, поэтому пропускать
        // источник нельзя: все следующие камеры получат чужую гомографию.
        if (new_bin == NULL) {
            LOG_CAM_ERROR("PIPELINE", index, "cannot create rtsp source bin for uri = %s", camera.uri.c_str());
            return -1;
        }

        if (!source_manager_add(sources, pipeline, streammux, new_bin, index)) {
            return -1;
        }
    }

    if (!gst_element_link(streammux, pgie) ||
            (tracker && !gst_element_link(pgie, tracker)) ||
            !gst_element_link(analytics_tail, demux)) {
        LOG_ERROR("PIPELINE", "cannot link nvstreammux, nvinfer, nvtracker and output");
        return -1;
    }

    for (int index = 0; config.output_enabled && index < num_sources; index++) {
        std::string output_uri = config.output_uri_prefix + "_" + std::to_string(index);
        new_bin = create_sink_bin(output_uri.c_str(), index);
        snprintf(buffer, 39, "nvosd_%1d", index);
        nvosd = gst_element_factory_make("nvdsosd", buffer);

        // Видеовыход отвязан от анализа очередью, которая выбрасывает старые
        // кадры: если RTSP-сервер не принимает поток или тормозит, детекция и
        // отправка координат продолжают работать.
        snprintf(buffer, 39, "output_queue_%1d", index);
        GstElement *output_queue = gst_element_factory_make("queue", buffer);

        if (new_bin == NULL || nvosd == NULL || output_queue == NULL) {
            g_printerr("Cannot create rtsp sink bin, queue or nvdsosd for uri = %s\n", output_uri.c_str());
            return -1;
        }

        g_object_set(G_OBJECT(output_queue),
                "leaky", 2,  // downstream: выбрасывать самые старые кадры
                "max-size-buffers", 4,
                "max-size-bytes", 0,
                "max-size-time", (guint64)0,
                NULL);

        gst_bin_add_many(GST_BIN(pipeline), output_queue, nvosd, new_bin, NULL);

        if (!gst_element_link(output_queue, nvosd)) {
            g_printerr("Cannot link output queue and nvosd.\n");
            return -1;
        }

        snprintf(buffer, 17, "src_%1d", index);
        src_pad = gst_element_request_pad_simple(demux, buffer);
        sink_pad = gst_element_get_static_pad(output_queue, "sink");

        if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
            g_printerr("Cannot link demux and output queue.\n");
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

    // Проба стоит после трекера, чтобы в сообщения попадали его object_id
    set_probe(analytics_tail, "src", pgie_src_pad_buffer_probe, &probe_data);

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "sheremetevo");
    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    BusCallData data = { loop, pipeline, &sources };
    bus_watch_id = gst_bus_add_watch(bus, bus_call, &data);
    gst_object_unref(bus);

    SignalData signal_data = { loop, pipeline, &sources, FALSE };
    guint sigint_id = g_unix_signal_add(SIGINT, on_terminate_signal, &signal_data);
    guint sigterm_id = g_unix_signal_add(SIGTERM, on_terminate_signal, &signal_data);

    probe_data.init_stats(num_sources);
    g_timeout_add_seconds(5, print_stream_stats, &probe_data);
    g_timeout_add_seconds(1, source_manager_watchdog, &sources);

    HealthMonitor health;
    health.heartbeat_file = config.heartbeat_file;
    health.hang_timeout_sec = config.hang_timeout_sec;
    health.no_frames_restart_sec = config.no_frames_restart_sec;
    health.sources = &sources;
    health.on_fatal = [](gpointer user_data) { on_terminate_signal(user_data); };
    health.on_fatal_data = &signal_data;
    health_attach(health, analytics_tail);

    MetricsServer metrics;
    if (config.metrics_enabled) {
        metrics.bind_address = config.metrics_bind_address;
        metrics.port = config.metrics_port;
        metrics.probe = &probe_data;
        metrics.sources = &sources;
        metrics.sender = &sender;
        // Включённые, но недоступные метрики — ошибка конфигурации (порт занят):
        // лучше узнать о ней при старте, чем не получить данные мониторинга.
        if (!metrics_start(metrics, error)) {
            LOG_ERROR("METRICS", "cannot start metrics server: %s", error.c_str());
            return -1;
        }
    }

    LOG_INFO("APP", "starting pipeline");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    // После перехода в PLAYING: в нём nvinfer может минутами строить engine,
    // и сторож не должен принять это за зависание.
    health_start(health);
    g_main_loop_run(loop);
    health_stop(health);
    metrics_stop(metrics);

    // Обработчики сигналов GLib работают только внутри главного цикла, а он уже
    // остановлен. Возвращаем стандартное поведение, чтобы Ctrl+C убивал процесс,
    // если какой-то элемент завис при остановке, и страхуемся таймером.
    g_source_remove(sigint_id);
    g_source_remove(sigterm_id);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    alarm(SHUTDOWN_TIMEOUT_SEC);

    LOG_INFO("APP", "shutting down pipeline");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    alarm(0);

    gst_object_unref(GST_OBJECT(pipeline));
    g_source_remove(bus_watch_id);
    g_main_loop_unref(loop);

    log_shutdown();

    // Ненулевой код, если приложение остановил контроль работоспособности
    return health.failed ? 1 : 0;
}
