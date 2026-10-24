#include "glib.h"
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>

#include "gstnvdsmeta.h"
#include "nvdsmeta.h"

#include "log.hpp"
#include "probers.hpp"
#include "message.hpp"
#include "points_sender.hpp"

void draw_object_meta(NvOSD_RectParams* bounding_box, NvOSD_TextParams* text_params,
                      const std::string& class_name, int64_t track_id, float confidence);
void draw_point_with_latlng(NvDsBatchMeta* batch_meta, NvDsFrameMeta* frame_meta,
                             NvDsDisplayMeta*& display_meta,
                             const NvOSD_RectParams& bbox, double lat, double lng);

void set_probe(GstElement* element,
        const gchar *pad_name,
        GstPadProbeReturn (*prober)
        (GstPad * pad,
        GstPadProbeInfo * info,
        gpointer u_data),
        gpointer u_data
        ) {
    GstPad *pad = gst_element_get_static_pad(element, pad_name);
    if (!pad) {
      g_printerr ("[PROBE] Error: Could not get pad '%s' from element '%s'\n",
                  pad_name, GST_ELEMENT_NAME (element));
      return;
    }

    guint probe_id = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                                         prober, u_data, NULL);
    if (probe_id == 0) {
      g_printerr ("[PROBE] Error: Failed to add probe on pad '%s'\n", pad_name);
    } else {
      g_print ("[PROBE] Probe installed successfully on %s:%s (probe_id=%u)\n",
               GST_ELEMENT_NAME(element), pad_name, probe_id);
    }

    gst_object_unref (pad);
}

// Время кадра в секундах Unix-эпохи. nvstreammux проставляет ntp_timestamp
// (по умолчанию — системное время прихода кадра, с attach-sys-ts=TRUE);
// если его нет, берём текущее время.
static gdouble frame_timestamp_sec(const NvDsFrameMeta *frame_meta) {
    if (frame_meta->ntp_timestamp != 0) {
        return frame_meta->ntp_timestamp / 1e9;
    }

    struct timespec current_time;
    clock_gettime(CLOCK_REALTIME, &current_time);
    return current_time.tv_sec + current_time.tv_nsec / 1e9;
}

static void send_frame_message(pgie_probe_data *prob_data, guint camera_id,
        gdouble timestamp, GArray *futures_array) {
    points_message_t msg = {
        .camera_id = static_cast<int>(camera_id),
        .timestamp = timestamp,
        .future = (future_t *)futures_array->data,
        .future_size = futures_array->len
    };

    prob_data->sender->add_frame(msg);
}

GstPadProbeReturn pgie_src_pad_buffer_probe(GstPad *,
        GstPadProbeInfo * info,
        gpointer u_data) {
    GstBuffer *buf = (GstBuffer *) info->data;
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);

    if (!batch_meta || u_data == NULL) {
        return GST_PAD_PROBE_OK;
    }

    pgie_probe_data *prob_data = static_cast<pgie_probe_data *>(u_data);

    if (prob_data->sender == NULL) {
        return GST_PAD_PROBE_OK;
    }

    GArray *new_futures_array = g_array_new(FALSE, FALSE, sizeof(future_t));
    const guint64 now_ns = static_cast<guint64>(g_get_real_time()) * 1000;

    NvDsFrameMetaList *frame_meta_list = batch_meta->frame_meta_list;

    for (; frame_meta_list != NULL; frame_meta_list = frame_meta_list->next) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)frame_meta_list->data;
        guint stream_id = frame_meta->pad_index;

        if (stream_id >= prob_data->transformers.size()) {
            LOG_CAM_ERROR("PROBE", static_cast<int>(stream_id), "no calibration, frame skipped");
            continue;
        }

        const PixelGeoTransformer &transformer = prob_data->transformers[stream_id];

        if (stream_id < prob_data->num_streams) {
            StreamCounters &c = prob_data->counters[stream_id];
            c.frames++;
            c.objects += frame_meta->num_obj_meta;
            if (frame_meta->ntp_timestamp != 0 && now_ns > frame_meta->ntp_timestamp) {
                c.latency_sum_us += (now_ns - frame_meta->ntp_timestamp) / 1000;
                c.latency_count++;
            }
        }

        g_array_set_size(new_futures_array, 0);
        NvDsDisplayMeta* current_display_meta = nullptr;

        NvDsObjectMetaList *obj_meta_list = frame_meta->obj_meta_list;

        for (; obj_meta_list != NULL; obj_meta_list = obj_meta_list->next) {
            NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) obj_meta_list->data;

            // проверка размера bbox'а
            if (obj_meta->rect_params.height == 0 ||
                    obj_meta->rect_params.width / obj_meta->rect_params.height <= prob_data->min_aspect_ratio) {
                continue;
            }

            gdouble x = obj_meta->rect_params.left + obj_meta->rect_params.width / 2;
            gdouble y = obj_meta->rect_params.top + obj_meta->rect_params.height;

            // get a geo point in format (lng, lat)
            auto geo_coords = transformer.pixel_to_geo(x, y);
            if (!geo_coords || !prob_data->zones[stream_id].contains(geo_coords->first, geo_coords->second)) {
                continue;
            }

            future_t future = {
                .lat = geo_coords->second,
                .lng = geo_coords->first,
                .object_id = obj_meta->object_id == UNTRACKED_OBJECT_ID
                    ? -1 : static_cast<int64_t>(obj_meta->object_id),
                .name = "самолёт",
                .confidence = obj_meta->confidence,
                .bbox = {
                    .x1 = static_cast<int>(obj_meta->rect_params.left),
                    .y1 = static_cast<int>(obj_meta->rect_params.top),
                    .x2 = static_cast<int>(obj_meta->rect_params.left + obj_meta->rect_params.width),
                    .y2 = static_cast<int>(obj_meta->rect_params.top + obj_meta->rect_params.height)
                }
            };
            g_array_append_vals(new_futures_array, &future, 1);

            draw_object_meta(&obj_meta->rect_params, &obj_meta->text_params,
                    future.name, future.object_id, future.confidence);

            draw_point_with_latlng(batch_meta, frame_meta, current_display_meta,
                    obj_meta->rect_params, future.lat, future.lng);
        }

        // одно сообщение на кадр, чтобы получатель знал, с какой камеры объекты
        send_frame_message(prob_data, stream_id, frame_timestamp_sec(frame_meta),
                new_futures_array);
    }

    g_array_free(new_futures_array, TRUE);

    return GST_PAD_PROBE_OK;
}

gboolean print_stream_stats(gpointer user_data) {
    pgie_probe_data *prob_data = static_cast<pgie_probe_data *>(user_data);

    static gint64 last_time = g_get_monotonic_time();
    gint64 now = g_get_monotonic_time();
    double seconds = (now - last_time) / 1e6;
    last_time = now;

    if (seconds <= 0) {
        return G_SOURCE_CONTINUE;
    }

    for (size_t i = 0; i < prob_data->num_streams; i++) {
        guint64 total_frames = prob_data->counters[i].frames;
        guint64 total_objects = prob_data->counters[i].objects;
        guint64 frames = total_frames - prob_data->last_frames[i];
        guint64 objects = total_objects - prob_data->last_objects[i];
        prob_data->last_frames[i] = total_frames;
        prob_data->last_objects[i] = total_objects;
        prob_data->fps[i] = frames / seconds;

        if (frames) {
            log_write(LogLevel::Info, "STATS", static_cast<int>(i), "%.1f fps, %.1f objects/frame",
                    prob_data->fps[i], (double)objects / frames);
        } else {
            log_write(LogLevel::Warning, "STATS", static_cast<int>(i),
                    "0.0 fps (no frames reach inference)");
        }
    }
    return G_SOURCE_CONTINUE;
}

void draw_object_meta(NvOSD_RectParams* bounding_box, NvOSD_TextParams* text_params,
                      const std::string& class_name, int64_t track_id, float confidence) {

    // Настройка рамки
    bounding_box->border_width = 2;
    bounding_box->has_bg_color = 0;

    bounding_box->border_color.red   = 0.0;
    bounding_box->border_color.green = 1.0;
    bounding_box->border_color.blue  = 0.0;
    bounding_box->border_color.alpha = 1.0;

    // Форматирование текста
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%lld %s %.2f", static_cast<long long>(track_id),
            class_name.c_str(), confidence);

    // Выделение памяти под строку (DeepStream освобождает её через g_free).
    // Старый текст, если его уже проставил nvinfer, освобождаем, чтобы не текла память.
    g_free(text_params->display_text);
    text_params->display_text = g_strdup(buffer);

    // Позиционирование текста
    text_params->x_offset = std::max(0, static_cast<int>(bounding_box->left));
    text_params->y_offset = std::max(0, static_cast<int>(bounding_box->top) - 30);

    // Настройка шрифта
    // Имя шрифта DeepStream не освобождает, поэтому строковый литерал, а не копия
    text_params->font_params.font_name = (gchar *)"Serif";
    text_params->font_params.font_size = 16;

    text_params->font_params.font_color.red   = 1.0;
    text_params->font_params.font_color.green = 1.0;
    text_params->font_params.font_color.blue  = 1.0;
    text_params->font_params.font_color.alpha = 1.0;
}

void draw_point_with_latlng(NvDsBatchMeta* batch_meta, NvDsFrameMeta* frame_meta,
                             NvDsDisplayMeta*& display_meta,
                             const NvOSD_RectParams& bbox, double lat, double lng) {
    if (!display_meta ||
        display_meta->num_circles >= MAX_ELEMENTS_IN_DISPLAY_META ||
        display_meta->num_labels  >= MAX_ELEMENTS_IN_DISPLAY_META) {

        display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
        if (!display_meta) {
            return;
        }
        nvds_add_display_meta_to_frame(frame_meta, display_meta);
    }

    int x = static_cast<int>(bbox.left + bbox.width / 2.0);
    int y = static_cast<int>(bbox.top + bbox.height);

    NvOSD_CircleParams* circle = &display_meta->circle_params[display_meta->num_circles];
    circle->xc = x;
    circle->yc = y;
    circle->radius = 5;
    circle->circle_color = {1.0, 0.0, 0.0, 1.0};
    circle->has_bg_color = 1;
    circle->bg_color = {1.0, 0.0, 0.0, 0.5};
    display_meta->num_circles++;

    NvOSD_TextParams* label = &display_meta->text_params[display_meta->num_labels];
    char buffer[128];
    // 6 знаков после запятой — около 0.1 м; при 4 знаках (~10 м) подпись
    // почти не меняется, пока самолёт движется по рулёжке
    snprintf(buffer, sizeof(buffer), "lat: %.6f lng: %.6f", lat, lng);

    label->display_text = g_strdup(buffer);

    label->x_offset = std::max(0, x);
    label->y_offset = std::max(0, y - 15);

    label->font_params.font_name = (gchar *)"Arial";
    label->font_params.font_size = 12;
    label->font_params.font_color = {1.0, 1.0, 0.0, 1.0};

    label->set_bg_clr = 1;
    label->text_bg_clr = {0.0, 0.0, 0.0, 0.6};

    display_meta->num_labels++;
}

