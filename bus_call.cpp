#include "custom_bins.hpp"
#include "glib.h"
#include "gst/gstbus.h"
#include "gst/gstutils.h"

#include "bus_call.hpp"
#include "contexts.hpp"

static gboolean is_inside_source_bin(GstObject *obj, SourceCtx *src)
{
    return src->bin && gst_object_has_as_ancestor(obj, GST_OBJECT(src->bin));
}

gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    AppCtx *ctx = (AppCtx *)data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            if (ctx->shutting_down) {
                g_print("End of stream.\n");
                g_main_loop_quit(ctx->loop);
            } else {
                g_printerr("EOS from pipeline — ignoring\n");
            }
            break;

        case GST_MESSAGE_ERROR: {
            gchar *debug = NULL;
            GError *error = NULL;
            gst_message_parse_error(msg, &error, &debug);

            gboolean handled = FALSE;
            for (guint i = 0; i < ctx->num_sources; i++) {
                if (is_inside_source_bin(GST_MESSAGE_SRC(msg), &ctx->sources[i])) {
                    g_printerr("[source %u] error: %s (%s)\n", i, error->message, debug ? debug : "");
                    do_reconnect_source(ctx, i);
                    handled = TRUE;
                    break;
                }
            }
            if (!handled) {
                g_printerr("Error: %s\n%s\n",
                           error->message, debug ? debug : "");
                g_main_loop_quit(ctx->loop);
            }
            g_error_free(error);
            g_free(debug);
            break;
        }

        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC (msg) != GST_OBJECT (ctx->pipeline))
                return TRUE;

            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

            g_print("%-21s state has been changed from %s to %s, pending state %s\n",
                    msg->src->name, gst_element_state_get_name(old_state), gst_element_state_get_name(new_state),
                    gst_element_state_get_name(pending_state));
            break;
        }

        case GST_MESSAGE_ELEMENT: {
            const GstStructure *structure = gst_message_get_structure(msg);
            if (!structure) return TRUE;
            const gchar *name = gst_structure_get_name(structure);
            g_print("%s\n", name);

            // FIXME:найти способ правильно получать stream_id при обрыве
            if (g_strcmp0(name, "stream-eos") == 0) {
                guint stream_id = 0;
                if (gst_structure_get_uint(structure, "stream-id", &stream_id) &&
                    stream_id < ctx->num_sources) {
                    g_print("[source %u] streammux: stream-eos — reconnecting\n", stream_id);
                    do_reconnect_source(ctx, stream_id);
                }
            } else if (g_strcmp0(name, "GstRTSPSrcTimeout") == 0) {
                gint stream_id = 0;
                if (gst_structure_get_int(structure, "stream-number", &stream_id) &&
                    stream_id < ctx->num_sources) {
                    g_print("[source %u] rtcp timeout: reconnecting\n", stream_id);
                    do_reconnect_source(ctx, stream_id);
                }
            } else {
                gchar *src_name = gst_object_get_name(GST_MESSAGE_SRC(msg));
                gchar *struct_str = gst_structure_to_string(structure);
                g_print("MESSAGE_ELEMENT from %s: %s\n", src_name, struct_str);
                g_free(src_name);
                g_free(struct_str);
            }
            break;
        }

        default: {
        }
    }
    return TRUE;
}

