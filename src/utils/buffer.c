#include "utils/buffer.h"
#include <stdlib.h>
#include <string.h>

struct buffer* buffer_new(size_t size) {
    struct buffer *buf = malloc(sizeof(struct buffer));
    if (!buf) return NULL;

    buf->area = malloc(size);
    if (!buf->area) {
        free(buf);
        return NULL;
    }

    buf->size = size;
    buf->data = 0;
    buf->head = 0;

    return buf;
}

void buffer_free(struct buffer *buf) {
    if (buf) {
        free(buf->area);
        free(buf);
    }
}

int buffer_put(struct buffer *buf, const void *input, size_t len) {
    if (buf->data + len > buf->size)
        return -1;

    // if (buf-> data + len > buf->size)
    //    return -1;

    memcpy(buf->area + buf->data, input, len);
    buf->data += len;

    return len;
}

int buffer_get(struct buffer *buf, void *output, size_t len) {
    size_t available = buf->data - buf->head;
    if (len > available)
        len = available;

    memcpy(output, buf->area + buf->head, len);
    buf->head += len;

    return len;
}

void buffer_reset(struct buffer *buf) {
    buf->data = 0;
    buf->head = 0;
}
