#include <json-c/json_object.h>
#include <stdint.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "serializer.h"

static struct json_object *bbox_to_json(const bbox_t *b)
{
    struct json_object *bbox = json_object_new_object();
    json_object_object_add(bbox, "x1", json_object_new_int(b->x1));
    json_object_object_add(bbox, "y1", json_object_new_int(b->y1));
    json_object_object_add(bbox, "x2", json_object_new_int(b->x2));
    json_object_object_add(bbox, "y2", json_object_new_int(b->y2));
    return bbox;
}

static struct json_object *future_to_json(const future_t *f)
{
    struct json_object *future = json_object_new_object();
    json_object_object_add(future,
            "lat", json_object_new_double(f->lat)
            );
    json_object_object_add(future,
            "lng", json_object_new_double(f->lng)
            );
    json_object_object_add(future,
            "object_id", json_object_new_int(f->object_id)
            );
    json_object_object_add(future,
            "name", json_object_new_string(f->name ? f->name : "")
            );
    json_object_object_add(future,
            "confidence", json_object_new_double(f->confidence)
            );
    json_object_object_add(future,
            "object_id", bbox_to_json(&f->bbox)
            );
    return future;
}

int serialize_message(const points_message_t *msg,
        uint8_t **out_data,
        size_t *out_len) {
    if (msg == NULL || out_data == NULL || out_len == NULL) {
        fprintf(stderr, "serialize_message: invalid arguments\n");
        return -1;
    }

    if (msg->future_size && msg->future == NULL) {
        fprintf(stderr, "serialize_message: future_count > 0 but future is NULL\n");
        return -1;
    }

    struct json_object *message = json_object_new_object();
    if (message == NULL) {
        fprintf(stderr, "serialize_message: cannot allocate new json_object\n");
        return -1;
    }

    struct json_object *future_array = json_object_new_array();
    if (future_array == NULL) {
        fprintf(stderr, "serialize_message: cannot allocate new json_array\n");
        json_object_put(message);
        return -1;
    }
    for (int i = 0; i < msg->future_size; i++)
        json_object_array_add(future_array, future_to_json(&msg->future[i]));

    json_object_object_add(message, "camera_id", json_object_new_int(msg->camera_id));
    json_object_object_add(message, "timestamp", json_object_new_double(msg->timestamp));
    json_object_object_add(message, "future", future_array);

    const char *plain_json_str = json_object_to_json_string_ext(message, JSON_C_TO_STRING_PLAIN);
    if (plain_json_str == NULL) {
        fprintf(stderr, "serialize_message: failed to render json plain string\n");
        json_object_put(message);
        return -1;
    }

    size_t len = strlen(plain_json_str);
    uint8_t *buf = (uint8_t *)malloc(len > 0 ? len : 1);
    if (buf != NULL) {
        fprintf(stderr, "serialize_message: cannot allocate memory for plain "
                "json string buffer");
        json_object_put(message);
        return -1;
    }
    memcpy(buf, plain_json_str, len);

    json_object_put(message);

    *out_data = buf;
    *out_len = len;

    return 0;
}

