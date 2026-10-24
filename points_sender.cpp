#include <time.h>
#include <cerrno>
#include <chrono>
#include <utility>

#include <google/protobuf/io/coded_stream.h>

#include "log.hpp"
#include "points_sender.hpp"

using google::protobuf::io::CodedOutputStream;

namespace {

// Повторяющиеся ошибки отправки (получатель недоступен) — не чаще раза в минуту
const std::chrono::seconds kErrorLogInterval(60);

// Предел очереди: при интервале 1 с и 4 камерах по 25 fps в ней ~100 кадров.
// Больше — значит, поток отправки завис; новые кадры отбрасываются.
const size_t kMaxPendingFrames = 10000;

// Batch без кадров: sequence (тег + varint до 10 байт) и sent_at (тег + 8 байт)
const size_t kBatchHeaderMax = 1 + 10 + 1 + 8;

// Размер вложенного сообщения вместе с тегом поля (номер поля < 16) и длиной
size_t field_size(size_t message_size) {
    return 1 + CodedOutputStream::VarintSize32(static_cast<uint32_t>(message_size)) + message_size;
}

double realtime_sec() {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

} // namespace

PointsSender::PointsSender(UdpConnection &udp_conn, int interval_ms, size_t max_datagram_bytes)
    : udp_conn(udp_conn), interval_ms(interval_ms), max_datagram_bytes(max_datagram_bytes) {
    buffer.resize(max_datagram_bytes);
    worker = std::thread(&PointsSender::run, this);
}

PointsSender::~PointsSender() {
    stop();
}

void PointsSender::add_frame(const points_message_t &msg) {
    sheremetevo::Frame frame;
    frame.set_camera_id(static_cast<uint32_t>(msg.camera_id));
    frame.set_timestamp(msg.timestamp);
    frame.mutable_detections()->Reserve(static_cast<int>(msg.future_size));

    for (size_t i = 0; i < msg.future_size; i++) {
        const future_t &f = msg.future[i];
        sheremetevo::Detection *det = frame.add_detections();
        det->set_lat(f.lat);
        det->set_lng(f.lng);
        det->set_object_id(f.object_id);
        det->set_name(f.name ? f.name : "");
        det->set_confidence(static_cast<float>(f.confidence));
        sheremetevo::BBox *bbox = det->mutable_bbox();
        bbox->set_x1(f.bbox.x1);
        bbox->set_y1(f.bbox.y1);
        bbox->set_x2(f.bbox.x2);
        bbox->set_y2(f.bbox.y2);
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) {
            return;
        }
        if (pending.size() >= kMaxPendingFrames) {
            dropped_frames++;
            frames_dropped++;
            return;
        }
        pending.push_back(std::move(frame));
    }

    if (interval_ms == 0) {
        cv.notify_one();
    }
}

void PointsSender::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    cv.notify_one();
    if (worker.joinable()) {
        worker.join();
    }
}

void PointsSender::run() {
    using clock = std::chrono::steady_clock;
    // Сроки отправки отсчитываются от старта, а не от предыдущей отправки,
    // чтобы интервал не уплывал на время сериализации
    clock::time_point deadline = clock::now();

    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
        if (interval_ms > 0) {
            deadline += std::chrono::milliseconds(interval_ms);
            cv.wait_until(lock, deadline, [this] { return stopping; });
            // после долгой паузы не пытаемся наверстать пропущенные интервалы
            if (deadline < clock::now()) {
                deadline = clock::now();
            }
        } else {
            cv.wait(lock, [this] { return stopping || !pending.empty(); });
        }

        std::vector<sheremetevo::Frame> frames;
        frames.swap(pending);
        size_t dropped = std::exchange(dropped_frames, 0);
        bool stop_now = stopping;
        lock.unlock();

        if (dropped > 0) {
            LOG_WARN("UDP", "queue overflow, %zu frames dropped", dropped);
        }
        // при пакетной отправке пустой Batch раз в интервал тоже уходит:
        // получатель видит, что приложение работает, даже если камеры отключены
        if (!frames.empty() || interval_ms > 0) {
            flush(frames);
        }

        if (stop_now) {
            return;
        }
        lock.lock();
    }
}

void PointsSender::flush(std::vector<sheremetevo::Frame> &frames) {
    sheremetevo::Batch batch;
    size_t batch_size = kBatchHeaderMax;

    for (sheremetevo::Frame &frame : frames) {
        size_t entry = field_size(frame.ByteSizeLong());
        if (batch_size + entry <= max_datagram_bytes) {
            *batch.add_frames() = std::move(frame);
            batch_size += entry;
            continue;
        }
        if (batch.frames_size() > 0) {
            send_batch(batch);
            batch_size = kBatchHeaderMax;
        }
        if (batch_size + entry <= max_datagram_bytes) {
            *batch.add_frames() = std::move(frame);
            batch_size += entry;
            continue;
        }

        // Кадр не помещается даже в пустую датаграмму: делим его объекты
        // на несколько Frame с теми же camera_id и timestamp.
        sheremetevo::Frame part;
        part.set_camera_id(frame.camera_id());
        part.set_timestamp(frame.timestamp());
        const size_t part_base = part.ByteSizeLong();
        size_t part_size = part_base;

        for (sheremetevo::Detection &det : *frame.mutable_detections()) {
            size_t det_entry = field_size(det.ByteSizeLong());
            if (kBatchHeaderMax + field_size(part_size + det_entry) > max_datagram_bytes &&
                    part.detections_size() > 0) {
                *batch.add_frames() = part;
                send_batch(batch);
                part.clear_detections();
                part_size = part_base;
            }
            if (kBatchHeaderMax + field_size(part_size + det_entry) > max_datagram_bytes) {
                LOG_WARN("UDP", "detection of %zu bytes does not fit into a datagram, dropped", det_entry);
                continue;
            }
            *part.add_detections() = std::move(det);
            part_size += det_entry;
        }

        if (part.detections_size() > 0) {
            batch_size = kBatchHeaderMax + field_size(part_size);
            *batch.add_frames() = std::move(part);
        }
    }

    if (batch.frames_size() > 0 || frames.empty()) {
        send_batch(batch);
    }
}

void PointsSender::send_batch(sheremetevo::Batch &batch) {
    batch.set_sequence(sequence++);
    batch.set_sent_at(realtime_sec());

    size_t size = batch.ByteSizeLong();
    if (size > buffer.size()) {
        // не должно случаться: размеры считаются при сборке пачки
        LOG_WARN("UDP", "batch of %zu bytes exceeds max datagram size %zu, sent anyway",
                size, buffer.size());
        buffer.resize(size);
    }

    const char *failure = NULL;
    if (!batch.SerializeToArray(buffer.data(), static_cast<int>(size))) {
        failure = "cannot serialize batch";
    } else if (udp_conn.send(buffer.data(), size) < 0) {
        failure = g_strerror(errno);
    } else {
        datagrams_sent++;
    }

    if (failure != NULL) {
        send_errors++;
        auto now = std::chrono::steady_clock::now();
        if (last_error_log == std::chrono::steady_clock::time_point{} ||
                now - last_error_log >= kErrorLogInterval) {
            LOG_ERROR("UDP", "failed to send %zu bytes to %s:%u: %s (%" G_GUINT64_FORMAT
                    " more failures since the previous message)",
                    size, udp_conn.server_ip.c_str(), static_cast<unsigned>(udp_conn.server_port),
                    failure, (guint64)errors_not_logged);
            last_error_log = now;
            errors_not_logged = 0;
        } else {
            errors_not_logged++;
        }
    }

    batch.Clear();
}
