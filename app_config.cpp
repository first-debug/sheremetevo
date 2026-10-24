#include <glib.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

#include "app_config.hpp"

namespace {

std::string resolve_path(const std::string &base_dir, const std::string &path) {
    if (path.empty() || g_path_is_absolute(path.c_str())) {
        return path;
    }
    gchar *joined = g_build_filename(base_dir.c_str(), path.c_str(), NULL);
    std::string result(joined);
    g_free(joined);
    return result;
}

// Обёртки над GKeyFile: значение по умолчанию, если ключа нет,
// и ошибка, если ключ есть, но значение не разбирается.
bool get_string(GKeyFile *kf, const char *group, const char *key, bool required,
                std::string &out, std::string &error) {
    GError *err = NULL;
    gchar *value = g_key_file_get_string(kf, group, key, &err);
    if (value == NULL) {
        bool missing = err != NULL && (err->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND ||
                                       err->code == G_KEY_FILE_ERROR_GROUP_NOT_FOUND);
        if (missing && !required) {
            g_clear_error(&err);
            return true;
        }
        error = std::string("[") + group + "] " + key + ": " + (err ? err->message : "invalid value");
        g_clear_error(&err);
        return false;
    }
    out = g_strstrip(value);
    g_free(value);
    return true;
}

bool get_int(GKeyFile *kf, const char *group, const char *key, int &out, std::string &error) {
    if (!g_key_file_has_key(kf, group, key, NULL)) {
        return true;
    }
    GError *err = NULL;
    gint value = g_key_file_get_integer(kf, group, key, &err);
    if (err != NULL) {
        error = std::string("[") + group + "] " + key + ": " + err->message;
        g_error_free(err);
        return false;
    }
    out = value;
    return true;
}

bool get_double(GKeyFile *kf, const char *group, const char *key, double &out, std::string &error) {
    if (!g_key_file_has_key(kf, group, key, NULL)) {
        return true;
    }
    GError *err = NULL;
    gdouble value = g_key_file_get_double(kf, group, key, &err);
    if (err != NULL) {
        error = std::string("[") + group + "] " + key + ": " + err->message;
        g_error_free(err);
        return false;
    }
    out = value;
    return true;
}

bool get_bool(GKeyFile *kf, const char *group, const char *key, bool &out, std::string &error) {
    if (!g_key_file_has_key(kf, group, key, NULL)) {
        return true;
    }
    GError *err = NULL;
    gboolean value = g_key_file_get_boolean(kf, group, key, &err);
    if (err != NULL) {
        error = std::string("[") + group + "] " + key + ": " + err->message;
        g_error_free(err);
        return false;
    }
    out = value;
    return true;
}

bool parse_config(GKeyFile *kf, const std::string &base_dir, AppConfig &config, std::string &error) {
    if (!get_string(kf, "app", "nvinfer-config", true, config.nvinfer_config, error) ||
            !get_int(kf, "app", "class-id", config.class_id, error) ||
            !get_int(kf, "app", "frame-width", config.frame_width, error) ||
            !get_int(kf, "app", "frame-height", config.frame_height, error) ||
            !get_int(kf, "app", "batched-push-timeout-us", config.batched_push_timeout_us, error) ||
            !get_bool(kf, "output", "enabled", config.output_enabled, error) ||
            !get_string(kf, "output", "uri-prefix", config.output_enabled, config.output_uri_prefix, error) ||
            !get_string(kf, "udp", "host", true, config.udp_host, error) ||
            !get_int(kf, "udp", "port", config.udp_port, error) ||
            !get_int(kf, "udp", "batch-interval-ms", config.udp_batch_interval_ms, error) ||
            !get_int(kf, "udp", "max-datagram-bytes", config.udp_max_datagram_bytes, error) ||
            !get_double(kf, "geo", "ransac-threshold-m", config.ransac_threshold_m, error) ||
            !get_double(kf, "geo", "min-aspect-ratio", config.min_aspect_ratio, error) ||
            !get_string(kf, "geo", "zone", false, config.zone_file, error) ||
            !get_int(kf, "rtsp", "stall-timeout-sec", config.stall_timeout_sec, error) ||
            !get_int(kf, "rtsp", "reconnect-interval-sec", config.reconnect_interval_sec, error) ||
            !get_string(kf, "health", "heartbeat-file", false, config.heartbeat_file, error) ||
            !get_int(kf, "health", "hang-timeout-sec", config.hang_timeout_sec, error) ||
            !get_int(kf, "health", "no-frames-restart-sec", config.no_frames_restart_sec, error) ||
            !get_string(kf, "logging", "level", false, config.log_level, error) ||
            !get_string(kf, "logging", "format", false, config.log_format, error) ||
            !get_string(kf, "logging", "file", false, config.log_file, error) ||
            !get_int(kf, "logging", "max-size-mb", config.log_max_size_mb, error) ||
            !get_int(kf, "logging", "max-files", config.log_max_files, error) ||
            !get_bool(kf, "metrics", "enabled", config.metrics_enabled, error) ||
            !get_string(kf, "metrics", "bind-address", false, config.metrics_bind_address, error) ||
            !get_int(kf, "metrics", "port", config.metrics_port, error)) {
        return false;
    }

    if (config.class_id < -1) {
        error = "[app] class-id must be -1 (any class) or a class index";
        return false;
    }
    if (config.frame_width <= 0 || config.frame_height <= 0) {
        error = "[app] frame-width and frame-height must be positive";
        return false;
    }
    if (config.udp_port <= 0 || config.udp_port > 65535) {
        error = "[udp] port must be in 1..65535";
        return false;
    }
    if (config.udp_batch_interval_ms < 0) {
        error = "[udp] batch-interval-ms must be non-negative";
        return false;
    }
    // 65507 — наибольшая полезная нагрузка UDP по IPv4
    if (config.udp_max_datagram_bytes < 256 || config.udp_max_datagram_bytes > 65507) {
        error = "[udp] max-datagram-bytes must be in 256..65507";
        return false;
    }
    if (config.stall_timeout_sec <= 0 || config.reconnect_interval_sec <= 0) {
        error = "[rtsp] stall-timeout-sec and reconnect-interval-sec must be positive";
        return false;
    }
    if (config.hang_timeout_sec < 0 || config.no_frames_restart_sec < 0) {
        error = "[health] hang-timeout-sec and no-frames-restart-sec must be non-negative";
        return false;
    }
    if (config.log_level != "debug" && config.log_level != "info" &&
            config.log_level != "warning" && config.log_level != "warn" && config.log_level != "error") {
        error = "[logging] level must be debug, info, warning or error";
        return false;
    }
    if (config.log_format != "text" && config.log_format != "json") {
        error = "[logging] format must be text or json";
        return false;
    }
    if (config.log_max_size_mb < 0 || config.log_max_files < 0) {
        error = "[logging] max-size-mb and max-files must be non-negative";
        return false;
    }
    if (config.metrics_enabled && (config.metrics_port <= 0 || config.metrics_port > 65535)) {
        error = "[metrics] port must be in 1..65535";
        return false;
    }
    if (config.ransac_threshold_m <= 0) {
        error = "[geo] ransac-threshold-m must be positive";
        return false;
    }

    config.zone_file = resolve_path(base_dir, config.zone_file);
    config.nvinfer_config = resolve_path(base_dir, config.nvinfer_config);
    config.heartbeat_file = resolve_path(base_dir, config.heartbeat_file);
    config.log_file = resolve_path(base_dir, config.log_file);

    if (!get_bool(kf, "tracker", "enabled", config.tracker_enabled, error)) {
        return false;
    }
    if (config.tracker_enabled) {
        if (!get_string(kf, "tracker", "ll-lib-file", true, config.tracker_lib, error) ||
                !get_string(kf, "tracker", "ll-config-file", true, config.tracker_config, error) ||
                !get_int(kf, "tracker", "width", config.tracker_width, error) ||
                !get_int(kf, "tracker", "height", config.tracker_height, error)) {
            return false;
        }
        if (config.tracker_width <= 0 || config.tracker_height <= 0 ||
                config.tracker_width % 32 != 0 || config.tracker_height % 32 != 0) {
            error = "[tracker] width and height must be positive multiples of 32";
            return false;
        }
        config.tracker_lib = resolve_path(base_dir, config.tracker_lib);
        config.tracker_config = resolve_path(base_dir, config.tracker_config);
    }

    // Группы [cameraN]: номера должны идти подряд с нуля, номер = camera_id
    std::vector<std::pair<long, std::string>> camera_groups;
    gchar **groups = g_key_file_get_groups(kf, NULL);
    for (gchar **g = groups; *g != NULL; g++) {
        if (!g_str_has_prefix(*g, "camera")) {
            continue;
        }
        const char *suffix = *g + strlen("camera");
        char *end = NULL;
        long index = strtol(suffix, &end, 10);
        if (*suffix == '\0' || *end != '\0' || index < 0) {
            error = std::string("bad camera group name [") + *g + "], expected [camera0], [camera1], ...";
            g_strfreev(groups);
            return false;
        }
        camera_groups.emplace_back(index, *g);
    }
    g_strfreev(groups);

    std::sort(camera_groups.begin(), camera_groups.end());
    if (camera_groups.empty()) {
        error = "no [cameraN] groups in config";
        return false;
    }

    for (size_t i = 0; i < camera_groups.size(); i++) {
        if (camera_groups[i].first != static_cast<long>(i)) {
            error = "camera groups must be numbered consecutively from [camera0]";
            return false;
        }

        const char *group = camera_groups[i].second.c_str();
        CameraConfig camera;
        if (!get_string(kf, group, "uri", true, camera.uri, error) ||
                !get_int(kf, group, "flip-method", camera.flip_method, error) ||
                !get_string(kf, group, "dewarper-config", false, camera.dewarper_config, error) ||
                !get_string(kf, group, "calibration", true, camera.calibration_file, error)) {
            return false;
        }
        if (camera.flip_method < -1 || camera.flip_method > 7) {
            error = std::string("[") + group + "] flip-method must be in 0..7";
            return false;
        }
        camera.dewarper_config = resolve_path(base_dir, camera.dewarper_config);
        camera.calibration_file = resolve_path(base_dir, camera.calibration_file);
        // zone= с пустым значением отключает общую зону для этой камеры
        if (g_key_file_has_key(kf, group, "zone", NULL)) {
            if (!get_string(kf, group, "zone", false, camera.zone_file, error)) {
                return false;
            }
            camera.zone_file = resolve_path(base_dir, camera.zone_file);
        } else {
            camera.zone_file = config.zone_file;
        }
        config.cameras.push_back(camera);
    }

    return true;
}

} // namespace

bool load_app_config(const std::string &path, AppConfig &config, std::string &error) {
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;

    if (!g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, &err)) {
        error = path + ": " + err->message;
        g_error_free(err);
        g_key_file_free(kf);
        return false;
    }

    gchar *dir = g_path_get_dirname(path.c_str());
    std::string base_dir(dir);
    g_free(dir);

    bool ok = parse_config(kf, base_dir, config, error);
    if (!ok) {
        error = path + ": " + error;
    }

    g_key_file_free(kf);
    return ok;
}

bool load_calibration_points(const std::string &path,
                             std::vector<cv::Point2d> &pixels,
                             std::vector<cv::Point2d> &geo,
                             std::string &error) {
    std::ifstream file(path);
    if (!file) {
        error = path + ": cannot open file";
        return false;
    }

    pixels.clear();
    geo.clear();

    std::string line;
    for (int line_num = 1; std::getline(file, line); line_num++) {
        size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }

        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream fields(line);
        double px, py, lng, lat;
        std::string extra;
        if (!(fields >> px >> py >> lng >> lat) || (fields >> extra)) {
            // заголовок допустим только первой значимой строкой
            if (pixels.empty() && line.find("pixel") != std::string::npos) {
                continue;
            }
            error = path + ":" + std::to_string(line_num) + ": expected pixel_x,pixel_y,lng,lat";
            return false;
        }

        pixels.emplace_back(px, py);
        geo.emplace_back(lng, lat);
    }

    if (pixels.size() < 4) {
        error = path + ": need at least 4 points, got " + std::to_string(pixels.size());
        return false;
    }
    return true;
}
