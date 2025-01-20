#ifndef DIRECTORY_H
#define DIRECTORY_H

#define _GNU_SOURCE
#define __USE_GNU
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "errors.h"
#include "protocol.h"

// Directory entry structure
typedef struct DirectoryEntry {
    char *name;
    int is_directory;
    FileMetadata *metadata;
    struct DirectoryEntry *parent;
    struct DirectoryEntry **children;
    size_t child_count;
    pthread_rwlock_t lock;
} DirectoryEntry;

// Initialize the directory manager
ErrorCode directory_init();

// Clean up the directory manager
void directory_cleanup();

// Lookup a path and return the corresponding directory entry
ErrorCode directory_lookup(const char *path, DirectoryEntry **entry);

// Create a directory at the given path
ErrorCode directory_create(const char *path);

// Delete a directory or file at the given path
ErrorCode directory_delete(const char *path);

// Register a file with metadata at the given path
ErrorCode directory_register_file(const char *path, FileMetadata *metadata);

// Retrieve metadata for a file at the given path
ErrorCode directory_get_metadata(const char *path, FileMetadata **metadata);

#endif // DIRECTORY_H