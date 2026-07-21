#ifndef SERIALIZER_H
#define SERIALIZER_H

#include <stdint.h>

#include "message.h"

int serialize_message(const points_message_t *msg,
        uint8_t **out_data,
        size_t *out_len);

#endif // SERIALIZER_H
