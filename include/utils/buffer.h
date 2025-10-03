#ifndef UTILS_BUFFER_H
#define UTILS_BUFFER_H

#include <stddef.h>
#include <stdint.h>
#include "core/common.h"

struct buffer* buffer_new(size_t size);
void buffer_free(struct buffer *buf);
int buffer_put(struct buffer *buf, const void *data, size_t len);
int buffer_get(struct buffer *buf, void *data, size_t len);
void buffer_reset(struct buffer *buf);

#endif