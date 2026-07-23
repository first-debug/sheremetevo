#ifndef MESSAGE_H
#define MESSAGE_H

#include <stddef.h>

typedef struct {
    int x1;
    int y1;
    int x2;
    int y2;
} bbox_t;

typedef struct {
    double      lat;
    double      lng;
    int         object_id;
    const char *name;
    double      confidence;
    bbox_t      bbox;
} future_t;

typedef struct {
    int             camera_id;
    double          timestamp;
    const future_t  *future;
    size_t          future_size;
} points_message_t;

#endif // MESSAGE_H
