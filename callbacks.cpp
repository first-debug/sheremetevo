#include "callbacks.hpp"

void cb_newpad(GstElement *bin, GstPad *pad, gpointer data) {
    GstElement *depay = (GstElement *)data;

    GstCaps *new_pad_caps = gst_pad_get_current_caps(pad);
    GstStructure *structure = gst_caps_get_structure(new_pad_caps, 0);
    gst_caps_unref(new_pad_caps);

    const gchar *caps_name = gst_structure_get_name(structure);
    if (!g_str_has_suffix(caps_name, "x-rtp")) {
        g_print("Skip pad with capability = %s.\n", caps_name);
        return;
    }

    const gchar *media = gst_structure_get_string(structure, "media");
    if (g_strcmp0(media, "video") != 0) {
        g_print("Skip pad with media property = %s.\n", media);
        return;
    }

    GstPad *sinkpad = gst_element_get_static_pad(depay, "sink");
    if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
        g_printerr("Cannot link source and depay.\n");
    }
    gst_object_unref(sinkpad);
}

gboolean cb_removepad(GstElement *bin, GstPad *pad, gpointer data) {
    gchar *bin_name = gst_object_get_name(GST_OBJECT(bin));
    gchar *pad_name = gst_pad_get_name(pad);

    g_print("Remove pad %s from %s\n", pad_name, bin_name);
    g_free(bin_name);
    g_free(pad_name);
    return TRUE;
}

