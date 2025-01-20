#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <pthread.h>
#include "errors.h"
#include "protocol.h"
// #include "directory.h"

// Maximum block size
#define BLOCK_SIZE 4096

// Structure representing a file block
typedef struct FileBlock {
    uint64_t block_id;
    uint8_t data[BLOCK_SIZE];
    size_t size; // Actual data size in the block
    pthread_mutex_t lock;
    struct FileBlock *next;
} FileBlock;

// Structure representing a file in storage
typedef struct StorageFile {
    char *filepath;
    FileMetadata metadata;
    FileBlock *blocks;
    pthread_mutex_t lock;
    struct StorageFile *next;
} StorageFile;

// Initialize the storage system
ErrorCode storage_init();

// Clean up the storage system
void storage_cleanup();

// Read data from a file at a given offset
ErrorCode storage_read(const char *filepath, uint64_t offset, uint8_t *buffer, size_t length, size_t *bytes_read);

// Write data to a file at a given offset
ErrorCode storage_write(const char *filepath, uint64_t offset, const uint8_t *buffer, size_t length);

// Stream a file for audio playback
ErrorCode storage_stream(const char *filepath, void (*callback)(const uint8_t *data, size_t length, void *user_data), void *user_data);

// Register a new file in storage
ErrorCode storage_register_file(const char *filepath, FileMetadata *metadata);

// Delete a file from storage
ErrorCode storage_delete_file(const char *filepath);

// Load tracking functions
void increment_load();
void decrement_load();
int storage_get_load();

#endif // STORAGE_H