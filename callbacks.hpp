#ifndef CALLBACKS_H
#define CALLBACKS_H

#include "gst/gstelement.h"

void cb_newpad(GstElement *bin, GstPad *pad, gpointer data);
gboolean cb_removepad(GstElement *bin, GstPad *pad, gpointer data);

#endif // CALLBACKS_H
