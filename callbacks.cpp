#include "callbacks.hpp"
#include "log.hpp"

void cb_newpad(GstElement *bin, GstPad *pad, gpointer data) {
    GstElement *depay = (GstElement *)data;

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (caps == NULL) {
        LOG_DEBUG("RTSP", "skip pad %s of %s without caps", GST_PAD_NAME(pad), GST_ELEMENT_NAME(bin));
        return;
    }

    // structure и строки из неё принадлежат caps: освобождать caps только после них
    const GstStructure *structure = gst_caps_get_structure(caps, 0);
    const gchar *caps_name = gst_structure_get_name(structure);
    const gchar *media = gst_structure_get_string(structure, "media");

    if (!g_str_has_suffix(caps_name, "x-rtp") || g_strcmp0(media, "video") != 0) {
        LOG_DEBUG("RTSP", "skip pad %s of %s: %s, media %s", GST_PAD_NAME(pad), GST_ELEMENT_NAME(bin),
                caps_name, media ? media : "(none)");
    } else {
        GstPad *sinkpad = gst_element_get_static_pad(depay, "sink");
        if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
            LOG_ERROR("RTSP", "cannot link %s of %s to depayloader", GST_PAD_NAME(pad), GST_ELEMENT_NAME(bin));
        }
        gst_object_unref(sinkpad);
    }

    gst_caps_unref(caps);
}

gboolean cb_removepad(GstElement *bin, GstPad *pad, gpointer) {
    LOG_DEBUG("RTSP", "pad %s removed from %s", GST_PAD_NAME(pad), GST_ELEMENT_NAME(bin));
    return TRUE;
}
