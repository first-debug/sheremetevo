#ifndef PROBERS_H
#define PROBERS_H

#include "gst/gstpad.h"
#include <atomic>
#include <memory>
#include <vector>

#include "geo_zone.hpp"
#include "pixel_transformer.hpp"
#include "points_sender.hpp"

struct StreamCounters {
    // кадры и объекты на выходе анализа
    std::atomic<guint64> frames{0};
    std::atomic<guint64> objects{0};
    // задержка обработки: от прихода кадра в nvstreammux (ntp_timestamp)
    // до выхода из анализа; сумма в микросекундах и число замеров
    std::atomic<guint64> latency_sum_us{0};
    std::atomic<guint64> latency_count{0};
};

struct pgie_probe_data {
    PointsSender *sender = nullptr;
    // гомография для каждой камеры, индекс — номер входа nvstreammux (pad_index)
    std::vector<PixelGeoTransformer> transformers;
    // зона камеры, вне которой точки не отправляются, индекс как у transformers;
    // пустая — без фильтра
    std::vector<GeoZone> zones;
    // минимальное отношение ширины bbox к высоте
    double min_aspect_ratio = 1.2;

    // Счётчики с начала работы, по одному набору на камеру. Пишутся из потока
    // конвейера, читаются таймером статистики и метриками в главном цикле.
    size_t num_streams = 0;
    std::unique_ptr<StreamCounters[]> counters;

    // Только для главного цикла: значения счётчиков на прошлом выводе
    // статистики и fps за последний интервал.
    std::vector<guint64> last_frames;
    std::vector<guint64> last_objects;
    std::vector<double> fps;

    void init_stats(size_t streams) {
        num_streams = streams;
        counters.reset(new StreamCounters[streams]);
        last_frames.assign(streams, 0);
        last_objects.assign(streams, 0);
        fps.assign(streams, 0.0);
    }
};

// Печатает fps и число объектов по каждой камере за прошедший интервал.
// Предназначена для g_timeout_add_seconds, user_data — pgie_probe_data.
gboolean print_stream_stats(gpointer user_data);

void set_probe(GstElement* element, const gchar *pad_name, GstPadProbeReturn (*prober) (GstPad * pad,
        GstPadProbeInfo * info,
        gpointer u_data),
        gpointer u_data);

GstPadProbeReturn pgie_src_pad_buffer_probe(GstPad * pad,
        GstPadProbeInfo * info,
        gpointer u_data);

#endif // PROBERS_H
