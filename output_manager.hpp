#ifndef OUTPUT_MANAGER_H
#define OUTPUT_MANAGER_H

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <gst/gst.h>

/*
 * Видеовыход с разметкой: для каждой камеры ветка
 *   nvstreamdemux src_N -> queue (leaky) -> nvdsosd -> sink-bin-N (H.264 -> rtspclientsink)
 *
 * Видеовыход вспомогательный, поэтому его сбой не должен мешать анализу
 * и отправке координат:
 *  - результат проталкивания кадра в ветку не возвращается в nvstreamdemux,
 *    иначе ошибка rtspclientsink (RTSP-сервер недоступен) остановила бы весь конвейер;
 *  - ERROR изнутри ветки не завершает приложение: ветка останавливается
 *    и перезапускается, пауза между попытками растёт от reconnect_interval_sec до 60 с.
 *
 * В журнал (компонент OUTPUT) пишутся: error — первый сбой ветки, warn — каждая
 * следующая попытка, info — ветка проработала без ошибок после перезапуска.
 */

struct OutputManager;

struct OutputBranch {
    enum class State { Running, Stopped };

    OutputManager *manager = NULL;
    guint index = 0;
    GstElement *queue = NULL;
    GstElement *osd = NULL;
    GstElement *sink_bin = NULL;

    State state = State::Running;
    // когда ветка перешла в текущее состояние, по g_get_monotonic_time
    gint64 state_since_us = 0;
    // сколько раз подряд ветка падала; обнуляется, когда она проработала без ошибок
    guint failures = 0;
    gint64 lost_since_us = 0;
    guint64 restarts_total = 0;

    // ветка остановлена: кадры для неё отбрасываются; читается из потока конвейера
    std::atomic<bool> down{false};
};

struct OutputManager {
    guint reconnect_interval_sec = 5;
    std::atomic<bool> shutting_down{false};
    std::vector<std::unique_ptr<OutputBranch>> branches;
};

// Создаёт ветку видеовыхода камеры index, публикующую поток в uri, и
// подключает её к выходу src_<index> nvstreamdemux. Индексы — подряд с нуля.
bool output_manager_add(OutputManager &manager, GstElement *pipeline, GstElement *demux,
        guint index, const std::string &uri);

// Вызывается из обработчика шины при ERROR. Возвращает true, если ошибка
// пришла из ветки видеовыхода (тогда она обработана и приложение работает дальше).
bool output_manager_on_error(OutputManager &manager, GstObject *source, const gchar *reason);

// При остановке приложения: остановленные ветки больше не перезапускаются.
void output_manager_shutdown(OutputManager &manager);

// Таймер главного цикла (раз в секунду), user_data — OutputManager:
// перезапускает остановленные ветки.
gboolean output_manager_watchdog(gpointer user_data);

#endif // OUTPUT_MANAGER_H
