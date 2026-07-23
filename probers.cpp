#include "glib.h"
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>

#include "gstnvdsmeta.h"
#include "nvdsmeta.h"

#include "probers.hpp"
#include "message.hpp"
#include "serializer.hpp"
#include "udp_connection.hpp"

void draw_object_meta(NvOSD_RectParams* bounding_box, NvOSD_TextParams* text_params,
                      const std::string& class_name, int track_id, float confidence);
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
    GstElement *probed_element = element;
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

GstPadProbeReturn print_nvmeta_probe(GstPad * pad,
        GstPadProbeInfo * info,
        gpointer u_data) {
    GstBuffer *buf = (GstBuffer *) info->data;
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);

    if (!batch_meta) {
        g_print ("[PROBE] Warning: No batch meta found!\n");
        return GST_PAD_PROBE_OK;
    }

    g_print ("\n[PROBE] ========== Batch Meta Analysis ==========\n");
    g_print ("[PROBE] Batch size: %u\n", batch_meta->num_frames_in_batch);

    NvDsFrameMetaList *frame_meta_list = batch_meta->frame_meta_list;
    guint frame_idx = 0;

    while (frame_meta_list != NULL) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) frame_meta_list->data;

        g_print ("\n[PROBE] --- Frame #%u ---\n", frame_idx);
        g_print ("[PROBE]   source_id: %u\n", frame_meta->source_id);
        g_print ("[PROBE]   batch_id: %u\n", frame_meta->batch_id);
        g_print ("[PROBE]   pad_index (mux sink pad): %u\n", frame_meta->pad_index);
        g_print ("[PROBE]   frame_num: %d\n", frame_meta->frame_num);
        g_print ("[PROBE]   buf_pts: %lu\n", frame_meta->buf_pts);

        NvDsObjectMetaList *obj_meta_list = frame_meta->obj_meta_list;
        guint obj_count = 0;

        while (obj_meta_list != NULL) {
            NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) obj_meta_list->data;

            g_print ("[PROBE]   -> Object #%u:\n", obj_count);
            g_print ("[PROBE]      class_id: %d\n", obj_meta->class_id);
            g_print ("[PROBE]      unique_component_id: %d\n", obj_meta->unique_component_id);
            g_print ("[PROBE]      confidence: %.4f\n", obj_meta->confidence);
            g_print ("[PROBE]      bbox: left=%.2f, top=%.2f, width=%.2f, height=%.2f\n",
                     obj_meta->rect_params.left,
                     obj_meta->rect_params.top,
                     obj_meta->rect_params.width,
                     obj_meta->rect_params.height);
            g_print ("[PROBE]      tracker_id: %ld\n", obj_meta->object_id);

            if (obj_meta->unique_component_id != 1) {
                g_print ("[PROBE]      WARNING: suspicious unique_component_id=%d (expected 1 for primary detector)\n",
                         obj_meta->unique_component_id);
            }

            obj_count++;
            obj_meta_list = obj_meta_list->next;
        }

        g_print ("[PROBE]   Total objects in this frame: %u\n", obj_count);

        NvDisplayMetaList *display_meta_list = frame_meta->display_meta_list;
        guint display_count = 0;
        while (display_meta_list != NULL) {
            display_count++;
            display_meta_list = display_meta_list->next;
        }
        if (display_count > 0) {
            g_print ("[PROBE]   Display meta entries: %u\n", display_count);
        }

        frame_idx++;
        frame_meta_list = frame_meta_list->next;
    }

    g_print ("[PROBE] ========== End of Batch ==========\n\n");

    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn pgie_src_pad_buffer_probe(GstPad * pad,
        GstPadProbeInfo * info,
        gpointer u_data) {
    GstBuffer *buf = (GstBuffer *) info->data;
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);

    if (!batch_meta) {
        return GST_PAD_PROBE_OK;
    }

    assert(u_data != NULL);

    pgie_probe_data *prob_data = static_cast<pgie_probe_data *>(u_data);

    assert(prob_data->udp_conn != NULL);

    GArray *new_futures_array = g_array_new(FALSE, FALSE, sizeof(future_t));

    NvDsFrameMetaList *frame_meta_list = batch_meta->frame_meta_list;

    for (guint frame_idx = 0; frame_meta_list != NULL; frame_idx++) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)frame_meta_list->data;
        guint stream_id = frame_meta->pad_index;

        NvDsDisplayMeta* current_display_meta = nullptr;

        NvDsObjectMetaList *obj_meta_list = frame_meta->obj_meta_list;
        guint obj_count = 0;

        for (; obj_meta_list != NULL; obj_count++) {
            NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) obj_meta_list->data;

            // проверка размера bbox'а
            if (obj_meta->rect_params.height != 0 &&
                    obj_meta->rect_params.width / obj_meta->rect_params.height > 1.2) {

                gint x = (gint) (obj_meta->rect_params.left +
                        obj_meta->rect_params.width / 2);
                gint y = (gint) (obj_meta->rect_params.top +
                        obj_meta->rect_params.height);

                std::pair geo_coords = prob_data->transformers[frame_meta->pad_index].pixel_to_geo(x, y);

                future_t future = {
                    .lat = geo_coords.first,
                    .lng = geo_coords.second,
                    .object_id = static_cast<int>(obj_meta->object_id),
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

            obj_meta_list = obj_meta_list->next;
        }

        NvDisplayMetaList *display_meta_list = frame_meta->display_meta_list;

        frame_meta_list = frame_meta_list->next;
    }

    struct timespec current_time;
    clock_gettime(CLOCK_REALTIME, &current_time);
    gdouble time_sec = current_time.tv_sec + current_time.tv_nsec;

    points_message_t msg = {
        .camera_id = 0,
        .timestamp = time_sec,
        .future = (future_t *)new_futures_array->data,
        .future_size = new_futures_array->len
    };

    uint8_t *data = NULL;
    size_t len_data;

    if (serialize_message(&msg, &data, &len_data) != 0) {
        g_print("Cannot serialize message.\n");
    } else {
        ssize_t sent = prob_data->udp_conn->send(data, len_data);

        if (sent < 0)
            g_print("Failed to send message with length = %ld to server = %s:%d\n",
                    len_data,
                    prob_data->udp_conn->server_ip.data(),
                    prob_data->udp_conn->server_port
                    );
    }

    g_array_free(new_futures_array, TRUE);

    return GST_PAD_PROBE_OK;
}

void draw_object_meta(NvOSD_RectParams* bounding_box, NvOSD_TextParams* text_params,
                      const std::string& class_name, int track_id, float confidence) {

    // Настройка рамки
    bounding_box->border_width = 2;
    bounding_box->has_bg_color = 0;

    bounding_box->border_color.red   = 0.0;
    bounding_box->border_color.green = 1.0;
    bounding_box->border_color.blue  = 0.0;
    bounding_box->border_color.alpha = 1.0;

    // Форматирование текста
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%d %s %.2f", track_id, class_name.c_str(), confidence);

    // Выделение памяти под строку (в DeepStream ожидается char*)
    text_params->display_text = strdup(buffer);

    // Позиционирование текста
    text_params->x_offset = std::max(0, static_cast<int>(bounding_box->left));
    text_params->y_offset = std::max(0, static_cast<int>(bounding_box->top) - 30);

    // Настройка шрифта
    text_params->font_params.font_name = strdup("Serif");
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
    snprintf(buffer, sizeof(buffer), "lat: %.6f lng: %.6f", lat, lng);

    label->display_text = strdup(buffer);

    label->x_offset = std::max(0, x);
    label->y_offset = std::max(0, y - 15);

    label->font_params.font_name = strdup("Arial");
    label->font_params.font_size = 12;
    label->font_params.font_color = {1.0, 1.0, 0.0, 1.0};

    label->set_bg_clr = 1;
    label->text_bg_clr = {0.0, 0.0, 0.0, 0.6};

    display_meta->num_labels++;
}

