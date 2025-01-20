#ifndef CACHE_H
#define CACHE_H

#include "errors.h"
#include <stdint.h>
#include <stddef.h>

ErrorCode cache_init(size_t capacity);
void cache_cleanup();

ErrorCode cache_get(const char *filepath, uint64_t offset, uint8_t *buffer, size_t length, size_t *bytes_read);
ErrorCode cache_put(const char *filepath, uint64_t offset, const uint8_t *data, size_t length);
ErrorCode cache_invalidate(const char *filepath, uint64_t offset);

#endif // CACHE_H