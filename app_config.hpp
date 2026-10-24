#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <string>
#include <vector>

#include "opencv2/core/types.hpp"

struct CameraConfig {
    std::string uri;
    // Преобразования входного потока; незаданные не добавляются в конвейер.
    // flip-method для nvvideoconvert (0..7), -1 — без поворота
    int flip_method = -1;
    // конфиг nvdewarper, пустая строка — без выпрямления
    std::string dewarper_config;
    // CSV с калибровочными точками: pixel_x,pixel_y,lng,lat
    std::string calibration_file;
    // CSV с вершинами зоны камеры (lng,lat); если ключа zone в группе камеры
    // нет — берётся общая [geo] zone, пустая строка — без фильтра по зоне
    std::string zone_file;
};

struct AppConfig {
    std::string nvinfer_config;
    // класс детектора, объекты которого отправляются (4 — airplane в COCO), -1 — любой
    int class_id = 4;

    // размер кадров в пачке nvstreammux (и выхода nvdewarper, если он включён)
    int frame_width = 1728;
    int frame_height = 2752;
    int batched_push_timeout_us = 40000;

    // переподключение камер: камера без кадров дольше stall_timeout_sec
    // перезапускается, между попытками — reconnect_interval_sec
    int stall_timeout_sec = 10;
    int reconnect_interval_sec = 5;

    // видеовыход с разметкой: <output_uri_prefix>_<номер камеры>
    bool output_enabled = true;
    std::string output_uri_prefix;

    std::string udp_host;
    int udp_port = 0;
    // пакетная отправка: точки копятся udp_batch_interval_ms и уходят пачкой,
    // 0 — каждый кадр отправляется сразу
    int udp_batch_interval_ms = 1000;
    // предел размера датаграммы; 1400 байт проходят без IP-фрагментации
    int udp_max_datagram_bytes = 1400;

    double ransac_threshold_m = 10.0;
    // минимальное отношение ширины bbox к высоте, чтобы объект считался самолётом
    double min_aspect_ratio = 1.2;
    // общая зона для камер без своего ключа zone (см. CameraConfig::zone_file)
    std::string zone_file;

    // порядок камер задаёт camera_id: [camera0], [camera1], ...
    std::vector<CameraConfig> cameras;

    // Трекер (nvtracker). Если секции [tracker] нет или enabled=false,
    // трекер в конвейер не добавляется и object_id в сообщениях равен -1.
    bool tracker_enabled = false;
    std::string tracker_lib;     // ll-lib-file
    std::string tracker_config;  // ll-config-file
    int tracker_width = 960;     // кратно 32
    int tracker_height = 544;    // кратно 32

    // Контроль работоспособности (см. health.hpp): heartbeat-файл для
    // Docker HEALTHCHECK, пустая строка — не писать
    std::string heartbeat_file = "/tmp/sheremetevo-health";
    // главный цикл не отвечает дольше — аварийный выход, 0 — не следить
    int hang_timeout_sec = 60;
    // нет обработанных кадров ни с одной камеры дольше — перезапуск, 0 — никогда
    int no_frames_restart_sec = 300;

    // Журнал (см. log.hpp): уровень debug/info/warning/error, формат text/json,
    // файл с ротацией по размеру; пустой log_file — только stdout/stderr
    std::string log_level = "info";
    std::string log_format = "text";
    std::string log_file;
    int log_max_size_mb = 50;
    int log_max_files = 5;

    // Prometheus-метрики (см. metrics.hpp). Если секции [metrics] нет или
    // enabled=false, порт не открывается.
    bool metrics_enabled = false;
    std::string metrics_bind_address = "0.0.0.0";
    int metrics_port = 9464;
};

/*
 * Загружает конфиг приложения (формат INI, как у nvinfer).
 * Относительные пути в конфиге считаются от папки, где лежит сам конфиг.
 * При ошибке возвращает false и пишет причину в error.
 */
bool load_app_config(const std::string &path, AppConfig &config, std::string &error);

/*
 * Читает CSV с калибровочными точками. Строки: pixel_x,pixel_y,lng,lat.
 * Пустые строки, строки с '#' и строка заголовка пропускаются.
 */
bool load_calibration_points(const std::string &path,
                             std::vector<cv::Point2d> &pixels,
                             std::vector<cv::Point2d> &geo,
                             std::string &error);

#endif // APP_CONFIG_H
