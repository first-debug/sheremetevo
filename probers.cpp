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

    GArray *new_futures_array = g_array_new(FALSE, FALSE, sizeof(future_t));

    NvDsFrameMetaList *frame_meta_list = batch_meta->frame_meta_list;

    for (guint frame_idx = 0; frame_meta_list != NULL; frame_idx++) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)frame_meta_list->data;

        guint stream_id = frame_meta->pad_index;

        // получение ограничивающих полигонов

        // получение необходимого объекта для преобразования пиксельных координат в географичесикие

        NvDsObjectMetaList *obj_meta_list = frame_meta->obj_meta_list;
        guint obj_count = 0;

        for (; obj_meta_list != NULL; obj_count++) {
            NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) obj_meta_list->data;

            // проверка размера bbox'а
            if (obj_meta->rect_params.height != 0 &&
                    obj_meta->rect_params.width / obj_meta->rect_params.height > 1.2) {
                NvOSD_TextParams text_params = obj_meta->text_params;

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

                // добавление дополнительных данных на OSD

                // добавление отрисовка точки внизу bbox'а с географическими координатами
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
        ssize_t sent = prob_data->udp_conn.send(data, len_data);

        if (sent < 0) {
            g_print("Failed to send message with length = %ld to server = %s:%d\n",
                    len_data,
                    prob_data->udp_conn.server_ip.data(),
                    prob_data->udp_conn.server_port
                    );
        } else
            g_print("Sent %zd bytes to %s:%u: %.*s\n",
                   sent,
                   prob_data->udp_conn.server_ip.data(),
                   prob_data->udp_conn.server_port,
                   (int)len_data,
                   (const char *)data
                   );
    }

    g_array_free(new_futures_array, TRUE);

    return GST_PAD_PROBE_OK;
}

