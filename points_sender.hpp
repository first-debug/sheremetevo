#ifndef POINTS_SENDER_H
#define POINTS_SENDER_H

#include <stddef.h>
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "points.pb.h"

#include "message.hpp"
#include "udp_connection.hpp"

/*
 * Пакетная отправка точек по UDP в формате protobuf (proto/points.proto).
 *
 * Проба конвейера кладёт кадры через add_frame() и сразу возвращается.
 * Отдельный поток раз в interval_ms забирает накопленное и отправляет
 * датаграммами не больше max_datagram_bytes; кадр, который не помещается
 * в одну датаграмму, делится по объектам. interval_ms = 0 — отправлять сразу.
 */
class PointsSender {
public:
    PointsSender(UdpConnection &udp_conn, int interval_ms, size_t max_datagram_bytes);
    ~PointsSender();

    // Потокобезопасно, вызывается из потока конвейера
    void add_frame(const points_message_t &msg);

    // Отправляет остаток и останавливает поток; повторный вызов ничего не делает
    void stop();

    // Счётчики с начала работы для метрик; читать можно из любого потока
    std::atomic<uint64_t> datagrams_sent{0};
    std::atomic<uint64_t> send_errors{0};
    std::atomic<uint64_t> frames_dropped{0};

    PointsSender(const PointsSender &) = delete;
    PointsSender &operator=(const PointsSender &) = delete;

private:
    void run();
    void flush(std::vector<sheremetevo::Frame> &frames);
    void send_batch(sheremetevo::Batch &batch);

    UdpConnection &udp_conn;
    const int interval_ms;
    const size_t max_datagram_bytes;

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<sheremetevo::Frame> pending;
    bool stopping = false;
    // кадры, отброшенные из-за переполнения очереди (поток отправки не успевает)
    size_t dropped_frames = 0;

    // используются только потоком отправки
    uint64_t sequence = 0;
    std::vector<uint8_t> buffer;
    // ошибки отправки пишутся в журнал не чаще раза в минуту: когда и сколько
    std::chrono::steady_clock::time_point last_error_log{};
    uint64_t errors_not_logged = 0;

    std::thread worker;
};

#endif // POINTS_SENDER_H
