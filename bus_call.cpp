#include "glib.h"
#include "gst/gstbus.h"
#include "gst/gstutils.h"

#include "bus_call.hpp"

gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    BusCallData *data_struct = (BusCallData *)data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("End of stream.\n");
            g_main_loop_quit(data_struct->loop);
            break;
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            g_printerr("Error: %s\n%s\n", error->message, debug);
            g_error_free(error);
            g_free(debug);
            g_main_loop_quit(data_struct->loop);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC (msg) != GST_OBJECT (data_struct->pipeline))
                return TRUE;

            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

            g_print("%-21s state has been changed from %s to %s, pending state %s\n",
                    msg->src->name, gst_element_state_get_name(old_state), gst_element_state_get_name(new_state),
                    gst_element_state_get_name(pending_state));
            break;
        }
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *structer = gst_message_get_structure(msg);
            if (structer == NULL)
                return TRUE;
            const gchar *elem_name = gst_structure_get_name(structer);
            GstObject *src_object = GST_MESSAGE_SRC(msg);

            gchar *src_name = gst_object_get_name(src_object);
            gchar *struct_str = gst_structure_to_string(structer);

            g_print("Catched MESSAGE_ELEMENT from = %s\n", src_name);
            g_print("%s\n", struct_str);
            g_free(src_name);
            g_free(struct_str);
            break;
        }
        default: {
        }
    }
    return TRUE;
}

