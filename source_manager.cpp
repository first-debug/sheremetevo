#include <algorithm>
#include <cstdio>

#include "log.hpp"
#include "source_manager.hpp"

static const gint64 US_PER_SEC = 1000000;

// Проба на выходе source-bin: отмечает время каждого кадра и не пускает EOS
// в nvstreammux. EOS приходит, когда сервер закрывает сессию; дойди он до
// nvstreammux, камера считалась бы законченной навсегда, а после последней
// камеры остановился бы весь конвейер. Вместо этого камеру перезапустит таймер.
static GstPadProbeReturn source_pad_probe(GstPad *, GstPadProbeInfo *info, gpointer user_data) {
    SourceSlot *slot = static_cast<SourceSlot *>(user_data);

    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        slot->last_buffer_us = g_get_monotonic_time();
        return GST_PAD_PROBE_OK;
    }

    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (event != NULL && GST_EVENT_TYPE(event) == GST_EVENT_EOS && !slot->manager->shutting_down) {
        LOG_CAM_INFO("RTSP", static_cast<int>(slot->index), "end of stream from server");
        return GST_PAD_PROBE_DROP;
    }
    return GST_PAD_PROBE_OK;
}

static void stop_source(SourceManager &manager, SourceSlot &slot, const gchar *reason) {
    slot.failures++;
    slot.restarts_total++;
    const int camera = static_cast<int>(slot.index);

    if (slot.failures == 1) {
        slot.lost_since_us = g_get_monotonic_time();
        LOG_CAM_ERROR("CAMERA", camera, "camera lost: %s, reconnecting in %u s",
                reason, manager.reconnect_interval_sec);
    } else {
        LOG_CAM_WARN("RTSP", camera, "%s, reconnecting in %u s (attempt %u, down for %" G_GINT64_FORMAT " s)",
                reason, manager.reconnect_interval_sec, slot.failures,
                (g_get_monotonic_time() - slot.lost_since_us) / US_PER_SEC);
    }

    gst_element_set_state(slot.bin, GST_STATE_NULL);
    slot.state = SourceSlot::State::Stopped;
    slot.state_since_us = g_get_monotonic_time();
}

static void start_source(SourceSlot &slot) {
    slot.state = SourceSlot::State::Running;
    slot.state_since_us = g_get_monotonic_time();

    // Ошибка подключения, если будет, придёт на шину и снова остановит камеру
    if (!gst_element_sync_state_with_parent(slot.bin)) {
        LOG_CAM_ERROR("RTSP", static_cast<int>(slot.index), "cannot start source");
    }
}

bool source_manager_add(SourceManager &manager, GstElement *pipeline, GstElement *streammux,
        GstElement *bin, guint index) {
    if (index != manager.slots.size()) {
        LOG_ERROR("RTSP", "camera %u added out of order", index);
        return false;
    }

    gst_bin_add(GST_BIN(pipeline), bin);

    gchar pad_name[16];
    snprintf(pad_name, sizeof(pad_name), "sink_%u", index);
    GstPad *src_pad = gst_element_get_static_pad(bin, "src");
    GstPad *sink_pad = gst_element_request_pad_simple(streammux, pad_name);

    bool linked = src_pad && sink_pad && gst_pad_link(src_pad, sink_pad) == GST_PAD_LINK_OK;
    if (sink_pad) {
        gst_object_unref(sink_pad);
    }
    if (!linked) {
        LOG_ERROR("PIPELINE", "cannot link source bin %u and streammux", index);
        if (src_pad) {
            gst_object_unref(src_pad);
        }
        return false;
    }

    auto slot = std::make_unique<SourceSlot>();
    slot->manager = &manager;
    slot->bin = bin;
    slot->index = index;
    slot->state_since_us = g_get_monotonic_time();

    gst_pad_add_probe(src_pad,
            (GstPadProbeType)(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
            source_pad_probe, slot.get(), NULL);
    gst_object_unref(src_pad);

    manager.slots.push_back(std::move(slot));
    return true;
}

void source_manager_on_error(SourceManager &manager, GstElement *bin, const gchar *reason) {
    for (auto &slot : manager.slots) {
        // Ошибки, пришедшие, пока камера уже остановлена, — хвост прошлой
        // попытки, повторно её не останавливаем.
        if (slot->bin == bin && slot->state == SourceSlot::State::Running && !manager.shutting_down) {
            stop_source(manager, *slot, reason);
            return;
        }
    }
}

void source_manager_shutdown(SourceManager &manager) {
    manager.shutting_down = true;
    const gint64 now = g_get_monotonic_time();

    for (auto &slot : manager.slots) {
        if (source_manager_is_receiving(manager, *slot, now)) {
            continue;
        }
        // Камера, от которой не идут кадры (остановлена или ещё подключается),
        // EOS не пришлёт, а nvstreammux ждёт его от всех входов. Если она всё же
        // пришлёт свой, второй EOS на том же входе просто отбрасывается.
        GstPad *src_pad = gst_element_get_static_pad(slot->bin, "src");
        GstPad *mux_pad = src_pad ? gst_pad_get_peer(src_pad) : NULL;
        if (mux_pad) {
            gst_pad_send_event(mux_pad, gst_event_new_eos());
            gst_object_unref(mux_pad);
        }
        if (src_pad) {
            gst_object_unref(src_pad);
        }
    }
}

bool source_manager_is_receiving(const SourceManager &manager, const SourceSlot &slot, gint64 now_us) {
    gint64 last = slot.last_buffer_us;
    return slot.state == SourceSlot::State::Running && last != 0 && last > slot.state_since_us &&
            now_us - last < (gint64)manager.stall_timeout_sec * US_PER_SEC;
}

gboolean source_manager_watchdog(gpointer user_data) {
    SourceManager *manager = static_cast<SourceManager *>(user_data);
    if (manager->shutting_down) {
        return G_SOURCE_REMOVE;
    }

    gint64 now = g_get_monotonic_time();

    for (auto &slot_ptr : manager->slots) {
        SourceSlot &slot = *slot_ptr;

        if (slot.state == SourceSlot::State::Stopped) {
            if (now - slot.state_since_us >= manager->reconnect_interval_sec * US_PER_SEC) {
                start_source(slot);
            }
            continue;
        }

        gint64 last_buffer = slot.last_buffer_us;
        if (last_buffer > slot.state_since_us && slot.failures > 0) {
            LOG_CAM_INFO("CAMERA", static_cast<int>(slot.index),
                    "camera restored after %" G_GINT64_FORMAT " s down, %u reconnect attempt(s)",
                    (now - slot.lost_since_us) / US_PER_SEC, slot.failures);
            slot.failures = 0;
        }

        // Пока кадров после запуска не было, отсчёт идёт от момента запуска
        gint64 silent_since = std::max(last_buffer, slot.state_since_us);
        if (now - silent_since > manager->stall_timeout_sec * US_PER_SEC) {
            gchar reason[64];
            snprintf(reason, sizeof(reason), "no frames for %u s", manager->stall_timeout_sec);
            stop_source(*manager, slot, reason);
        }
    }
    return G_SOURCE_CONTINUE;
}
