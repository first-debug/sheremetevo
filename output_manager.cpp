#include <algorithm>
#include <cstdio>

#include "custom_bins.hpp"
#include "log.hpp"
#include "output_manager.hpp"

static const gint64 US_PER_SEC = 1000000;
// наибольшая пауза между попытками перезапуска ветки
static const guint MAX_RETRY_INTERVAL_SEC = 60;
// ветка, проработавшая без ошибок столько после перезапуска, считается восстановленной
static const guint STABLE_AFTER_SEC = 30;

// Проба на выходе nvstreamdemux: передаёт кадр в ветку сама и не возвращает
// результат в nvstreamdemux. Без неё ошибка в ветке (FLOW_ERROR от
// rtspclientsink, через очередь) дошла бы до nvstreamdemux и остановила
// анализ всех камер. Пока ветка остановлена, кадры для неё отбрасываются.
// События (caps, segment, EOS) идут обычным путём.
static GstPadProbeReturn isolate_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    OutputBranch *branch = static_cast<OutputBranch *>(user_data);
    if (branch->down) {
        return GST_PAD_PROBE_DROP;
    }

    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GstPad *peer = gst_pad_get_peer(pad);
    if (peer != NULL) {
        // gst_pad_chain забирает буфер, результат намеренно не используется
        gst_pad_chain(peer, buffer);
        gst_object_unref(peer);
    } else {
        gst_buffer_unref(buffer);
    }
    // HANDLED: кадр обработан пробой, буфер уже отдан, nvstreamdemux получает GST_FLOW_OK
    return GST_PAD_PROBE_HANDLED;
}

static guint retry_interval_sec(const OutputManager &manager, const OutputBranch &branch) {
    guint interval = manager.reconnect_interval_sec;
    for (guint i = 1; i < branch.failures && interval < MAX_RETRY_INTERVAL_SEC; i++) {
        interval *= 2;
    }
    return std::min(interval, MAX_RETRY_INTERVAL_SEC);
}

static void stop_branch(OutputManager &manager, OutputBranch &branch, const gchar *reason) {
    branch.failures++;
    branch.restarts_total++;
    const int camera = static_cast<int>(branch.index);
    const guint retry = retry_interval_sec(manager, branch);

    if (branch.failures == 1) {
        branch.lost_since_us = g_get_monotonic_time();
        LOG_CAM_ERROR("OUTPUT", camera, "video output failed: %s, restarting in %u s", reason, retry);
    } else {
        LOG_CAM_WARN("OUTPUT", camera, "video output failed again: %s, restarting in %u s (attempt %u)",
                reason, retry, branch.failures);
    }

    branch.down = true;
    // Сначала то, что ниже по потоку: rtspclientsink может держать поток очереди,
    // и очередь не остановится, пока он не отпустит.
    gst_element_set_state(branch.sink_bin, GST_STATE_NULL);
    gst_element_set_state(branch.osd, GST_STATE_NULL);
    gst_element_set_state(branch.queue, GST_STATE_NULL);
    branch.state = OutputBranch::State::Stopped;
    branch.state_since_us = g_get_monotonic_time();
}

static void start_branch(OutputBranch &branch) {
    branch.state = OutputBranch::State::Running;
    branch.state_since_us = g_get_monotonic_time();

    // Ниже по потоку — раньше, чтобы очередь не толкала кадры в неготовый энкодер
    if (!gst_element_sync_state_with_parent(branch.sink_bin) ||
            !gst_element_sync_state_with_parent(branch.osd) ||
            !gst_element_sync_state_with_parent(branch.queue)) {
        LOG_CAM_ERROR("OUTPUT", static_cast<int>(branch.index), "cannot start video output");
    }
    branch.down = false;
}

bool output_manager_add(OutputManager &manager, GstElement *pipeline, GstElement *demux,
        guint index, const std::string &uri) {
    if (index != manager.branches.size()) {
        LOG_ERROR("OUTPUT", "video output %u added out of order", index);
        return false;
    }

    gchar name[32];
    snprintf(name, sizeof(name), "output_queue_%u", index);
    GstElement *queue = gst_element_factory_make("queue", name);
    snprintf(name, sizeof(name), "nvosd_%u", index);
    GstElement *osd = gst_element_factory_make("nvdsosd", name);
    GstElement *sink_bin = create_sink_bin(uri.c_str(), static_cast<gint>(index));

    if (queue == NULL || osd == NULL || sink_bin == NULL) {
        LOG_ERROR("OUTPUT", "cannot create queue, nvdsosd or rtsp sink bin for uri = %s", uri.c_str());
        for (GstElement *element : { queue, osd, sink_bin }) {
            if (element != NULL) {
                gst_object_unref(element);
            }
        }
        return false;
    }

    // Очередь отвязывает видеовыход от анализа: если RTSP-сервер тормозит,
    // старые кадры выбрасываются, а детекция и отправка координат не ждут.
    g_object_set(G_OBJECT(queue),
            "leaky", 2,  // downstream: выбрасывать самые старые кадры
            "max-size-buffers", 4,
            "max-size-bytes", 0,
            "max-size-time", (guint64)0,
            NULL);

    gst_bin_add_many(GST_BIN(pipeline), queue, osd, sink_bin, NULL);
    if (!gst_element_link_many(queue, osd, sink_bin, NULL)) {
        LOG_ERROR("OUTPUT", "cannot link output queue, nvdsosd and sink bin %u", index);
        return false;
    }

    snprintf(name, sizeof(name), "src_%u", index);
    GstPad *demux_pad = gst_element_request_pad_simple(demux, name);
    GstPad *queue_pad = gst_element_get_static_pad(queue, "sink");
    bool linked = demux_pad && queue_pad && gst_pad_link(demux_pad, queue_pad) == GST_PAD_LINK_OK;
    if (queue_pad) {
        gst_object_unref(queue_pad);
    }
    if (!linked) {
        LOG_ERROR("OUTPUT", "cannot link demux and output queue %u", index);
        if (demux_pad) {
            gst_object_unref(demux_pad);
        }
        return false;
    }

    auto branch = std::make_unique<OutputBranch>();
    branch->manager = &manager;
    branch->index = index;
    branch->queue = queue;
    branch->osd = osd;
    branch->sink_bin = sink_bin;
    branch->state_since_us = g_get_monotonic_time();

    gst_pad_add_probe(demux_pad, GST_PAD_PROBE_TYPE_BUFFER, isolate_probe, branch.get(), NULL);
    gst_object_unref(demux_pad);

    manager.branches.push_back(std::move(branch));
    return true;
}

static bool branch_contains(const OutputBranch &branch, GstObject *object) {
    GstObject *current = object ? GST_OBJECT(gst_object_ref(object)) : NULL;
    while (current != NULL) {
        if (current == GST_OBJECT(branch.queue) || current == GST_OBJECT(branch.osd) ||
                current == GST_OBJECT(branch.sink_bin)) {
            gst_object_unref(current);
            return true;
        }
        GstObject *parent = gst_object_get_parent(current);
        gst_object_unref(current);
        current = parent;
    }
    return false;
}

bool output_manager_on_error(OutputManager &manager, GstObject *source, const gchar *reason) {
    for (auto &branch : manager.branches) {
        if (!branch_contains(*branch, source)) {
            continue;
        }
        // Ошибки, пришедшие после остановки ветки, — хвост прошлой попытки
        // (например, очередь сообщает об ошибке rtspclientsink повторно)
        if (branch->state == OutputBranch::State::Running && !manager.shutting_down) {
            stop_branch(manager, *branch, reason);
        }
        return true;
    }
    return false;
}

void output_manager_shutdown(OutputManager &manager) {
    manager.shutting_down = true;
}

gboolean output_manager_watchdog(gpointer user_data) {
    OutputManager *manager = static_cast<OutputManager *>(user_data);
    if (manager->shutting_down) {
        return G_SOURCE_REMOVE;
    }

    gint64 now = g_get_monotonic_time();
    for (auto &branch_ptr : manager->branches) {
        OutputBranch &branch = *branch_ptr;

        if (branch.state == OutputBranch::State::Stopped) {
            if (now - branch.state_since_us >= (gint64)retry_interval_sec(*manager, branch) * US_PER_SEC) {
                start_branch(branch);
            }
        } else if (branch.failures > 0 && now - branch.state_since_us >= (gint64)STABLE_AFTER_SEC * US_PER_SEC) {
            LOG_CAM_INFO("OUTPUT", static_cast<int>(branch.index),
                    "video output restored after %" G_GINT64_FORMAT " s down, %u restart(s)",
                    (branch.state_since_us - branch.lost_since_us) / US_PER_SEC, branch.failures);
            branch.failures = 0;
        }
    }
    return G_SOURCE_CONTINUE;
}
