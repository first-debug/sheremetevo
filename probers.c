#include "gstnvdsmeta.h"
#include "nvdsmeta.h"

#include "probers.h"

GstPadProbeReturn meta_prober(GstPad * pad,
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

  /* Итерируемся по всем кадрам в батче */
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
    // g_print ("[PROBE]   source_frame_num: %u\n", frame_meta->source_frame_num);

    /* Считаем и выводим объекты в ЭТОМ конкретном кадре */
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

      /* Проверяем, не является ли это объект от другого источника */
      if (obj_meta->unique_component_id != 1) {
        g_print ("[PROBE]      WARNING: suspicious unique_component_id=%d (expected 1 for primary detector)\n",
                 obj_meta->unique_component_id);
      }

      obj_count++;
      obj_meta_list = obj_meta_list->next;
    }

    g_print ("[PROBE]   Total objects in this frame: %u\n", obj_count);

    /* Также проверяем display_meta (текст, линии и т.д.) */
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

void set_prober(GstElement* element, gchar *pad_name) {
    GstElement *probed_element = element;
    GstPad *pad = gst_element_get_static_pad(element, pad_name);
    if (!pad) {
      g_printerr ("[PROBE] Error: Could not get pad '%s' from element '%s'\n",
                  pad_name, GST_ELEMENT_NAME (element));
      return;
    }

    guint probe_id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER,
                                         meta_prober, NULL, NULL);
    if (probe_id == 0) {
      g_printerr ("[PROBE] Error: Failed to add probe on pad '%s'\n", pad_name);
    } else {
      g_print ("[PROBE] Probe installed successfully on %s:%s (probe_id=%u)\n",
               GST_ELEMENT_NAME (element), pad_name, probe_id);
    }

    gst_object_unref (pad);
}

