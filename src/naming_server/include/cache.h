#ifndef CACHE_H
#define CACHE_H

#include "errors.h"
#include "directory.h"
#include <stddef.h>

ErrorCode cache_init(size_t capacity);
void cache_cleanup();

ErrorCode cache_get(const char *path, DirectoryEntry **dir_entry);
ErrorCode cache_put(const char *path, DirectoryEntry *dir_entry);
ErrorCode cache_invalidate(const char *path);

#endif // CACHE_H