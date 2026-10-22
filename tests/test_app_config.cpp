#include <glib.h>
#include <stdlib.h>
#include <unistd.h>
#include <fstream>
#include <string>
#include <vector>

#include "test.hpp"
#include "../app_config.hpp"

#ifndef TEST_SRC_DIR
#define TEST_SRC_DIR "."
#endif

namespace {

// Временная папка для файлов теста; удаляется вместе с содержимым
class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/sheremetevo-test-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir() {
        for (const std::string &f : files) {
            unlink(f.c_str());
        }
        rmdir(path.c_str());
    }
    std::string write(const std::string &name, const std::string &content) {
        std::string full = path + "/" + name;
        std::ofstream(full) << content;
        files.push_back(full);
        return full;
    }

    std::string path;

private:
    std::vector<std::string> files;
};

// Минимальный рабочий конфиг: только обязательные ключи
const std::string kMinimal =
    "[app]\n"
    "nvinfer-config=infer.txt\n"
    "[output]\n"
    "enabled=false\n"
    "[udp]\n"
    "host=127.0.0.1\n"
    "port=6767\n"
    "[camera0]\n"
    "uri=rtsp://cam/0\n"
    "calibration=calibration/camera0.csv\n";

struct Loaded {
    bool ok;
    AppConfig config;
    std::string error;
    std::string path;
};

Loaded load(const std::string &ini) {
    TempDir dir;
    Loaded r;
    r.path = dir.write("app.ini", ini);
    r.ok = load_app_config(r.path, r.config, r.error);
    return r;
}

// Загрузка конфига, который должен не пройти проверку; возвращает текст ошибки
std::string load_error(const std::string &ini) {
    Loaded r = load(ini);
    if (r.ok) {
        return "<loaded without error>";
    }
    return r.error;
}

// kMinimal с заменой строки from на to (from должна встречаться)
std::string replace(std::string s, const std::string &from, const std::string &to) {
    size_t pos = s.find(from);
    if (pos != std::string::npos) {
        s.replace(pos, from.size(), to);
    }
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// app.ini
// ---------------------------------------------------------------------------

TEST("app_config: minimal config gets defaults for optional sections") {
    Loaded r = load(kMinimal);
    REQUIRE(r.ok);
    const AppConfig &c = r.config;

    CHECK_EQ(c.class_id, 4);
    CHECK_EQ(c.frame_width, 1728);
    CHECK_EQ(c.frame_height, 2752);
    CHECK_EQ(c.batched_push_timeout_us, 40000);
    CHECK_EQ(c.output_enabled, false);
    CHECK_EQ(c.udp_host, std::string("127.0.0.1"));
    CHECK_EQ(c.udp_port, 6767);
    CHECK_EQ(c.udp_batch_interval_ms, 1000);
    CHECK_EQ(c.udp_max_datagram_bytes, 1400);
    // [rtsp], [health], [geo], [tracker] не заданы
    CHECK_EQ(c.stall_timeout_sec, 10);
    CHECK_EQ(c.reconnect_interval_sec, 5);
    CHECK_EQ(c.heartbeat_file, std::string("/tmp/sheremetevo-health"));
    CHECK_EQ(c.hang_timeout_sec, 60);
    CHECK_EQ(c.no_frames_restart_sec, 300);
    CHECK_NEAR(c.ransac_threshold_m, 10.0, 1e-12);
    CHECK_NEAR(c.min_aspect_ratio, 1.2, 1e-12);
    CHECK(c.zone_file.empty());
    CHECK_EQ(c.tracker_enabled, false);

    REQUIRE_EQ(c.cameras.size(), size_t(1));
    CHECK_EQ(c.cameras[0].uri, std::string("rtsp://cam/0"));
    CHECK_EQ(c.cameras[0].flip_method, -1);
    CHECK(c.cameras[0].dewarper_config.empty());
}

TEST("app_config: relative paths are resolved from the config folder") {
    std::string ini = kMinimal +
        "[health]\n"
        "heartbeat-file=run/health\n"
        "[tracker]\n"
        "enabled=true\n"
        "ll-lib-file=/opt/lib/tracker.so\n"
        "ll-config-file=tracker.yml\n";
    ini = replace(ini, "calibration=calibration/camera0.csv",
                  "calibration=calibration/camera0.csv\ndewarper-config=warper.toml");
    Loaded r = load(ini);
    REQUIRE(r.ok);

    std::string dir = r.path.substr(0, r.path.rfind('/'));
    CHECK_EQ(r.config.nvinfer_config, dir + "/infer.txt");
    CHECK_EQ(r.config.heartbeat_file, dir + "/run/health");
    CHECK_EQ(r.config.tracker_lib, std::string("/opt/lib/tracker.so"));
    CHECK_EQ(r.config.tracker_config, dir + "/tracker.yml");
    CHECK_EQ(r.config.cameras[0].calibration_file, dir + "/calibration/camera0.csv");
    CHECK_EQ(r.config.cameras[0].dewarper_config, dir + "/warper.toml");
}

TEST("app_config: [geo] zone is resolved from the config folder") {
    Loaded r = load(kMinimal + "[geo]\nzone=zones/apron.csv\n");
    REQUIRE(r.ok);
    std::string dir = r.path.substr(0, r.path.rfind('/'));
    CHECK_EQ(r.config.zone_file, dir + "/zones/apron.csv");

    Loaded abs = load(kMinimal + "[geo]\nzone=/etc/sheremetevo/zone.csv\n");
    REQUIRE(abs.ok);
    CHECK_EQ(abs.config.zone_file, std::string("/etc/sheremetevo/zone.csv"));
}

TEST("app_config: camera zone overrides [geo] zone, empty value disables it") {
    std::string ini = kMinimal +
        "[geo]\nzone=common.csv\n"
        "[camera1]\nuri=rtsp://cam/1\ncalibration=c1.csv\nzone=zones/cam1.csv\n"
        "[camera2]\nuri=rtsp://cam/2\ncalibration=c2.csv\nzone=\n";
    Loaded r = load(ini);
    REQUIRE(r.ok);
    REQUIRE_EQ(r.config.cameras.size(), size_t(3));
    std::string dir = r.path.substr(0, r.path.rfind('/'));
    CHECK_EQ(r.config.cameras[0].zone_file, dir + "/common.csv");
    CHECK_EQ(r.config.cameras[1].zone_file, dir + "/zones/cam1.csv");
    CHECK(r.config.cameras[2].zone_file.empty());

    // без [geo] zone у камеры без своего ключа фильтра нет
    Loaded none = load(kMinimal);
    REQUIRE(none.ok);
    CHECK(none.config.cameras[0].zone_file.empty());
}

TEST("app_config: empty heartbeat-file disables heartbeat") {
    Loaded r = load(kMinimal + "[health]\nheartbeat-file=\n");
    REQUIRE(r.ok);
    CHECK(r.config.heartbeat_file.empty());
}

TEST("app_config: cameras are ordered by group number") {
    std::string ini = kMinimal +
        "[camera2]\nuri=rtsp://cam/2\ncalibration=c2.csv\nflip-method=1\n"
        "[camera1]\nuri=rtsp://cam/1\ncalibration=c1.csv\n";
    Loaded r = load(ini);
    REQUIRE(r.ok);
    REQUIRE_EQ(r.config.cameras.size(), size_t(3));
    CHECK_EQ(r.config.cameras[1].uri, std::string("rtsp://cam/1"));
    CHECK_EQ(r.config.cameras[2].uri, std::string("rtsp://cam/2"));
    CHECK_EQ(r.config.cameras[2].flip_method, 1);
}

TEST("app_config: tracker section with enabled=false needs nothing else") {
    Loaded r = load(kMinimal + "[tracker]\nenabled=false\n");
    REQUIRE(r.ok);
    CHECK_EQ(r.config.tracker_enabled, false);
}

TEST("app_config: tracker settings are read when enabled") {
    Loaded r = load(kMinimal +
        "[tracker]\nenabled=true\nll-lib-file=/t.so\nll-config-file=/t.yml\nwidth=640\nheight=384\n");
    REQUIRE(r.ok);
    CHECK_EQ(r.config.tracker_enabled, true);
    CHECK_EQ(r.config.tracker_width, 640);
    CHECK_EQ(r.config.tracker_height, 384);
}

TEST("app_config: configs/app.ini.example is a valid config") {
    AppConfig config;
    std::string error;
    bool ok = load_app_config(TEST_SRC_DIR "/configs/app.ini.example", config, error);
    CHECK_EQ(error, std::string(""));
    REQUIRE(ok);
    CHECK_EQ(config.cameras.size(), size_t(4));
    CHECK_EQ(config.tracker_enabled, true);
    CHECK_EQ(config.udp_max_datagram_bytes, 1400);
    CHECK_EQ(config.log_level, std::string("info"));
    CHECK_EQ(config.metrics_port, 9464);
}

// Шаблон — справочник по настройкам: в нём должна быть каждая группа,
// которую читает load_app_config, даже необязательная
TEST("app_config: configs/app.ini.example has every config group") {
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;
    bool loaded = g_key_file_load_from_file(kf, TEST_SRC_DIR "/configs/app.ini.example",
                                            G_KEY_FILE_NONE, &err);
    g_clear_error(&err);
    REQUIRE(loaded);
    const char *groups[] = {"app", "rtsp", "health", "logging", "metrics", "output",
                            "udp", "geo", "tracker", "camera0"};
    for (const char *group : groups) {
        if (!g_key_file_has_group(kf, group)) {
            CHECK_EQ(std::string("missing [") + group + "]", std::string(""));
        }
    }
    g_key_file_free(kf);
}

TEST("app_config: missing file") {
    AppConfig config;
    std::string error;
    CHECK(!load_app_config("/nonexistent/app.ini", config, error));
    CHECK_CONTAINS(error, "/nonexistent/app.ini");
}

TEST("app_config: error message names the file") {
    Loaded r = load(replace(kMinimal, "port=6767", "port=0"));
    REQUIRE(!r.ok);
    CHECK_CONTAINS(r.error, r.path + ": ");
}

TEST("app_config: missing required keys") {
    CHECK_CONTAINS(load_error(replace(kMinimal, "nvinfer-config=infer.txt", "")),
                   "[app] nvinfer-config");
    CHECK_CONTAINS(load_error(replace(kMinimal, "host=127.0.0.1", "")), "[udp] host");
    CHECK_CONTAINS(load_error(replace(kMinimal, "port=6767", "")), "[udp] port");
    CHECK_CONTAINS(load_error(replace(kMinimal, "uri=rtsp://cam/0", "")), "[camera0] uri");
    CHECK_CONTAINS(load_error(replace(kMinimal, "calibration=calibration/camera0.csv", "")),
                   "[camera0] calibration");
    // видеовыход включён (по умолчанию), а куда отправлять — не задано
    CHECK_CONTAINS(load_error(replace(kMinimal, "enabled=false", "")), "[output] uri-prefix");
}

TEST("app_config: values that do not parse") {
    CHECK_CONTAINS(load_error(replace(kMinimal, "port=6767", "port=abc")), "[udp] port");
    CHECK_CONTAINS(load_error(replace(kMinimal, "enabled=false", "enabled=maybe")),
                   "[output] enabled");
    CHECK_CONTAINS(load_error(kMinimal + "[geo]\nransac-threshold-m=ten\n"),
                   "[geo] ransac-threshold-m");
    CHECK_CONTAINS(load_error(kMinimal + "[rtsp]\nstall-timeout-sec=1.5\n"),
                   "[rtsp] stall-timeout-sec");
    CHECK_CONTAINS(load_error("this is not an ini file\n"), "app.ini");
}

TEST("app_config: values out of range") {
    CHECK_CONTAINS(load_error(replace(kMinimal, "port=6767", "port=70000")), "[udp] port");
    CHECK_CONTAINS(load_error(kMinimal + "[app]\nframe-width=0\n"), "[app] frame-width");
    CHECK_CONTAINS(load_error(replace(kMinimal, "port=6767", "port=6767\nbatch-interval-ms=-1")),
                   "[udp] batch-interval-ms");
    CHECK_CONTAINS(load_error(replace(kMinimal, "port=6767", "port=6767\nmax-datagram-bytes=100")),
                   "[udp] max-datagram-bytes");
    CHECK_CONTAINS(load_error(replace(kMinimal, "port=6767", "port=6767\nmax-datagram-bytes=65508")),
                   "[udp] max-datagram-bytes");
    CHECK_CONTAINS(load_error(kMinimal + "[rtsp]\nreconnect-interval-sec=0\n"), "[rtsp]");
    CHECK_CONTAINS(load_error(kMinimal + "[health]\nhang-timeout-sec=-1\n"), "[health]");
    CHECK_CONTAINS(load_error(kMinimal + "[geo]\nransac-threshold-m=0\n"),
                   "[geo] ransac-threshold-m");
    CHECK_CONTAINS(load_error(replace(kMinimal, "uri=rtsp://cam/0", "uri=rtsp://cam/0\nflip-method=8")),
                   "[camera0] flip-method");
    CHECK_CONTAINS(load_error(replace(kMinimal, "nvinfer-config=infer.txt", "nvinfer-config=infer.txt\nclass-id=-2")),
                   "[app] class-id");
}

TEST("app_config: boundary values are accepted") {
    Loaded r = load(replace(kMinimal, "port=6767", "port=65535\nmax-datagram-bytes=256\nbatch-interval-ms=0"));
    CHECK_EQ(r.error, std::string(""));
    REQUIRE(r.ok);
    CHECK_EQ(r.config.udp_port, 65535);
    CHECK_EQ(r.config.udp_max_datagram_bytes, 256);
    CHECK_EQ(r.config.udp_batch_interval_ms, 0);

    r = load(kMinimal + "[health]\nhang-timeout-sec=0\nno-frames-restart-sec=0\n");
    REQUIRE(r.ok);

    // -1 — объекты любого класса
    r = load(replace(kMinimal, "nvinfer-config=infer.txt", "nvinfer-config=infer.txt\nclass-id=-1"));
    REQUIRE(r.ok);
    CHECK_EQ(r.config.class_id, -1);
}

TEST("app_config: tracker errors") {
    CHECK_CONTAINS(load_error(kMinimal + "[tracker]\nenabled=true\nll-config-file=/t.yml\n"),
                   "[tracker] ll-lib-file");
    CHECK_CONTAINS(load_error(kMinimal + "[tracker]\nenabled=true\nll-lib-file=/t.so\n"),
                   "[tracker] ll-config-file");
    CHECK_CONTAINS(load_error(kMinimal +
                       "[tracker]\nenabled=true\nll-lib-file=/t.so\nll-config-file=/t.yml\nwidth=100\n"),
                   "multiples of 32");
}

TEST("app_config: camera group errors") {
    // ни одной камеры
    std::string no_cameras = kMinimal.substr(0, kMinimal.find("[camera0]"));
    CHECK_CONTAINS(load_error(no_cameras), "no [cameraN]");

    // пропуск в нумерации
    CHECK_CONTAINS(load_error(kMinimal + "[camera2]\nuri=rtsp://cam/2\ncalibration=c.csv\n"),
                   "consecutively");
    CHECK_CONTAINS(load_error(replace(kMinimal, "[camera0]", "[camera1]")), "consecutively");

    // имя группы не по шаблону
    CHECK_CONTAINS(load_error(kMinimal + "[camera_main]\nuri=x\n"), "bad camera group name [camera_main]");
    CHECK_CONTAINS(load_error(kMinimal + "[camera]\nuri=x\n"), "bad camera group name [camera]");
    CHECK_CONTAINS(load_error(kMinimal + "[camera-1]\nuri=x\n"), "bad camera group name");
}

// ---------------------------------------------------------------------------
// batch-size из конфига nvinfer
// ---------------------------------------------------------------------------

TEST("nvinfer config: batch-size is read from [property]") {
    TempDir dir;
    std::string path = dir.write("infer.txt",
            "[property]\n"
            "gpu-id=0\n"
            "batch-size=4\n"
            "filter-out-class-ids=0;1;2;3;5\n"
            "\n"
            "[class-attrs-all]\n"
            "pre-cluster-threshold=0.25\n");
    int batch = 0;
    std::string error;
    REQUIRE(read_nvinfer_batch_size(path, batch, error));
    CHECK_EQ(batch, 4);
}

TEST("nvinfer config: configs/config_infer_primary_yolo.txt.example has batch-size") {
    int batch = 0;
    std::string error;
    REQUIRE(read_nvinfer_batch_size(std::string(TEST_SRC_DIR) + "/configs/config_infer_primary_yolo.txt.example",
                                    batch, error));
    CHECK(batch > 0);
}

TEST("nvinfer config: no batch-size keeps the value, errors name the file") {
    TempDir dir;
    int batch = 7;
    std::string error;
    REQUIRE(read_nvinfer_batch_size(dir.write("infer.txt", "[property]\ngpu-id=0\n"), batch, error));
    CHECK_EQ(batch, 7);

    CHECK(!read_nvinfer_batch_size(dir.path + "/missing.txt", batch, error));
    CHECK_CONTAINS(error, "missing.txt");

    CHECK(!read_nvinfer_batch_size(dir.write("bad.txt", "[property]\nbatch-size=four\n"), batch, error));
    CHECK_CONTAINS(error, "batch-size");
    CHECK_CONTAINS(error, "bad.txt");
}

// ---------------------------------------------------------------------------
// CSV калибровки
// ---------------------------------------------------------------------------

namespace {

struct Points {
    bool ok;
    std::vector<cv::Point2d> pixels;
    std::vector<cv::Point2d> geo;
    std::string error;
};

Points load_csv(const std::string &content) {
    TempDir dir;
    Points p;
    std::string path = dir.write("camera.csv", content);
    p.ok = load_calibration_points(path, p.pixels, p.geo, p.error);
    return p;
}

} // namespace

TEST("calibration csv: header, comments, blank lines, CRLF and spaces") {
    Points p = load_csv(
        "# комментарий\r\n"
        "\r\n"
        "pixel_x,pixel_y,lng,lat\r\n"
        "100,200,37.41,55.979   # точка у ангара\r\n"
        "  900 , 210 , 37.412 , 55.9791\r\n"
        "\t\n"
        "880,700,37.4119,55.978\n"
        "120.5,690.25,37.4101,55.9779");  // без перевода строки в конце
    CHECK_EQ(p.error, std::string(""));
    REQUIRE(p.ok);
    REQUIRE_EQ(p.pixels.size(), size_t(4));
    REQUIRE_EQ(p.geo.size(), size_t(4));
    CHECK_NEAR(p.pixels[0].x, 100, 1e-12);
    CHECK_NEAR(p.pixels[0].y, 200, 1e-12);
    // в geo x — долгота, y — широта
    CHECK_NEAR(p.geo[0].x, 37.41, 1e-12);
    CHECK_NEAR(p.geo[0].y, 55.979, 1e-12);
    CHECK_NEAR(p.pixels[1].x, 900, 1e-12);
    CHECK_NEAR(p.geo[1].y, 55.9791, 1e-12);
    CHECK_NEAR(p.pixels[3].x, 120.5, 1e-12);
    CHECK_NEAR(p.pixels[3].y, 690.25, 1e-12);
}

TEST("calibration csv: full precision coordinates are kept") {
    Points p = load_csv(
        "2530,437,37.41477683540954,55.98059550914879\n"
        "2580,411,37.41465827198790,55.98057708028017\n"
        "1,2,3,4\n"
        "5,6,7,8\n");
    REQUIRE(p.ok);
    CHECK_NEAR(p.geo[0].x, 37.41477683540954, 1e-14);
    CHECK_NEAR(p.geo[1].y, 55.98057708028017, 1e-14);
}

TEST("calibration csv: configs/calibration/camera.csv.example loads") {
    std::vector<cv::Point2d> pixels, geo;
    std::string error;
    bool ok = load_calibration_points(TEST_SRC_DIR "/configs/calibration/camera.csv.example",
                                      pixels, geo, error);
    CHECK_EQ(error, std::string(""));
    REQUIRE(ok);
    CHECK_EQ(pixels.size(), size_t(4));
}

TEST("calibration csv: missing file") {
    std::vector<cv::Point2d> pixels, geo;
    std::string error;
    CHECK(!load_calibration_points("/nonexistent/camera.csv", pixels, geo, error));
    CHECK_CONTAINS(error, "/nonexistent/camera.csv: cannot open file");
}

TEST("calibration csv: fewer than 4 points") {
    Points p = load_csv("pixel_x,pixel_y,lng,lat\n1,2,3,4\n5,6,7,8\n9,10,11,12\n");
    CHECK(!p.ok);
    CHECK_CONTAINS(p.error, "need at least 4 points, got 3");

    p = load_csv("# пусто\n");
    CHECK(!p.ok);
    CHECK_CONTAINS(p.error, "got 0");
}

TEST("calibration csv: bad lines report their line number") {
    const std::string good = "1,2,3,4\n5,6,7,8\n9,10,11,12\n13,14,15,16\n";

    Points p = load_csv(good + "17,18,19\n");
    CHECK(!p.ok);
    CHECK_CONTAINS(p.error, "camera.csv:5: expected pixel_x,pixel_y,lng,lat");

    p = load_csv("# заголовок\n1,2,3,4,5\n" + good);
    CHECK(!p.ok);
    CHECK_CONTAINS(p.error, ":2:");

    p = load_csv(good + "17,18,abc,20\n");
    CHECK(!p.ok);
    CHECK_CONTAINS(p.error, ":5:");

    // ';' вместо ',' (CSV из Excel с русской локалью)
    p = load_csv("1;2;3;4\n" + good);
    CHECK(!p.ok);
    CHECK_CONTAINS(p.error, ":1:");
}

TEST("calibration csv: header is allowed only before the first point") {
    Points p = load_csv("1,2,3,4\npixel_x,pixel_y,lng,lat\n5,6,7,8\n9,10,11,12\n13,14,15,16\n");
    CHECK(!p.ok);
    CHECK_CONTAINS(p.error, ":2:");
}

TEST("calibration csv: previous contents of the output vectors are replaced") {
    TempDir dir;
    std::string path = dir.write("camera.csv", "1,2,3,4\n5,6,7,8\n9,10,11,12\n13,14,15,16\n");
    std::vector<cv::Point2d> pixels = {{-1, -1}}, geo = {{-1, -1}};
    std::string error;
    REQUIRE(load_calibration_points(path, pixels, geo, error));
    CHECK_EQ(pixels.size(), size_t(4));
    CHECK_NEAR(pixels[0].x, 1, 1e-12);
}
