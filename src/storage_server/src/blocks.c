#include "storage.h"
#include "replication.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>

// Head of the storage files linked list
static StorageFile *storage_files = NULL;
static pthread_mutex_t storage_lock = PTHREAD_MUTEX_INITIALIZER;

// Global variable to track the number of active storage operations
static pthread_mutex_t load_mutex = PTHREAD_MUTEX_INITIALIZER;
static int current_load = 0;

// Function to increment load
void increment_load() {
    pthread_mutex_lock(&load_mutex);
    current_load++;
    pthread_mutex_unlock(&load_mutex);
}

// Function to decrement load
void decrement_load() {
    pthread_mutex_lock(&load_mutex);
    current_load--;
    pthread_mutex_unlock(&load_mutex);
}

// Function to get current load
int storage_get_load() {
    pthread_mutex_lock(&load_mutex);
    int load = current_load;
    pthread_mutex_unlock(&load_mutex);
    return load;
}

// Initialize the storage system
ErrorCode storage_init() {
    replication_init();
    // Initialization code if needed
    return ERR_SUCCESS;
}

// Clean up the storage system
void storage_cleanup() {
    pthread_mutex_lock(&storage_lock);
    StorageFile *current = storage_files;
    while (current) {
        StorageFile *next_file = current->next;

        // Clean up blocks
        FileBlock *block = current->blocks;
        while (block) {
            FileBlock *next_block = block->next;
            pthread_mutex_destroy(&block->lock);
            free(block);
            block = next_block;
        }

        pthread_mutex_destroy(&current->lock);
        free(current->filepath);
        free(current);
        current = next_file;
    }
    storage_files = NULL;
    pthread_mutex_unlock(&storage_lock);
    pthread_mutex_destroy(&storage_lock);

    replication_cleanup();
}

// Helper function to find or create a storage file
static ErrorCode get_storage_file(const char *filepath, StorageFile **file, int create) {
    pthread_mutex_lock(&storage_lock);
    StorageFile *current = storage_files;
    StorageFile *prev = NULL;
    while (current) {
        if (strcmp(current->filepath, filepath) == 0) {
            printf("[DEBUG] Found existing file: %s\n", filepath); // Debug log
            *file = current;
            pthread_mutex_unlock(&storage_lock);
            return ERR_SUCCESS;
        }
        prev = current;
        current = current->next;
    }
    if (create) {
        // Create new storage file
        printf("[DEBUG] Creating new storage file: %s\n", filepath); // Debug log
        StorageFile *new_file = malloc(sizeof(StorageFile));
        if (!new_file) {
            printf("[ERROR] Memory allocation failed for file: %s\n", filepath);
            pthread_mutex_unlock(&storage_lock);
            return ERR_INTERNAL_ERROR;
        }
        new_file->filepath = strdup(filepath);
        if (!new_file->filepath) {
            printf("[ERROR] Memory allocation failed for filepath: %s\n", filepath);
            free(new_file);
            pthread_mutex_unlock(&storage_lock);
            return ERR_INTERNAL_ERROR;
        }
        memset(&new_file->metadata, 0, sizeof(FileMetadata));
        new_file->blocks = NULL;
        pthread_mutex_init(&new_file->lock, NULL);
        new_file->next = NULL;

        // Add to storage files list
        if (prev) {
            prev->next = new_file;
        } else {
            storage_files = new_file;
        }
        *file = new_file;
        printf("[DEBUG] New storage file created: %s\n", filepath); // Debug log
        pthread_mutex_unlock(&storage_lock);
        return ERR_SUCCESS;
    } else {
        printf("[DEBUG] File not found and `create` flag is false: %s\n", filepath); // Debug log
        pthread_mutex_unlock(&storage_lock);
        return ERR_NOT_FOUND;
    }
}

// Register a new file in storage
ErrorCode storage_register_file(const char *filepath, FileMetadata *metadata) {
    // Just verify the file exists or create it
    FILE *file = fopen(filepath, "a+b");
    if (!file) {
        perror("fopen");
        return ERR_IO_ERROR;
    }
    fclose(file);
    
    printf("[DEBUG] storage_register_file: Successfully registered file: %s\n", filepath);
    return ERR_SUCCESS;
}

// Delete a file from storage
ErrorCode storage_delete_file(const char *filepath) {
    if (remove(filepath) != 0) {
        perror("remove");
        return ERR_IO_ERROR;
    }

    // Replicate the delete to secondary servers
    replication_replicate_delete(filepath);

    printf("[DEBUG] storage_delete_file: Successfully deleted file: %s\n", filepath);
    return ERR_SUCCESS;
}

// Read data from a file at a given offset
ErrorCode storage_read(const char *filepath, uint64_t offset, uint8_t *buffer, size_t length, size_t *bytes_read) {
    printf("[DEBUG] Attempting to read from file: %s at offset %lu for length %zu\n", filepath, offset, length);
    
    increment_load();

    FILE *file = fopen(filepath, "rb");
    if (!file) {
        perror("fopen");
        decrement_load();
        return ERR_NOT_FOUND;
    }

    if (fseek(file, offset, SEEK_SET) != 0) {
        perror("fseek");
        fclose(file);
        decrement_load();
        return ERR_IO_ERROR;
    }

    size_t read = fread(buffer, 1, length, file);
    if (read < length && ferror(file)) {
        perror("fread");
        fclose(file);
        decrement_load();
        return ERR_IO_ERROR;
    }

    fclose(file);
    *bytes_read = read;
    printf("[DEBUG] storage_read: Successfully read %zu bytes from file: %s\n", *bytes_read, filepath);

    decrement_load();
    return ERR_SUCCESS;
}

// Write data to a file at a given offset
ErrorCode storage_write(const char *filepath, uint64_t offset, const uint8_t *buffer, size_t length) {
    increment_load();

    FILE *file = fopen(filepath, "r+b");
    if (!file) {
        // If file doesn't exist, create it
        file = fopen(filepath, "w+b");
        if (!file) {
            perror("fopen");
            decrement_load();
            return ERR_IO_ERROR;
        }
    }

    if (fseek(file, offset, SEEK_SET) != 0) {
        perror("fseek");
        fclose(file);
        decrement_load();
        return ERR_IO_ERROR;
    }

    size_t written = fwrite(buffer, 1, length, file);
    if (written < length) {
        perror("fwrite");
        fclose(file);
        decrement_load();
        return ERR_IO_ERROR;
    }

    fflush(file);
    fclose(file);

    // Replicate the write to secondary servers
    replication_replicate_write(filepath, offset, buffer, length);

    decrement_load();
    printf("[DEBUG] storage_write: Successfully wrote %zu bytes to file: %s\n", written, filepath);
    return ERR_SUCCESS;
}

// Stream a file for audio playback
ErrorCode storage_stream(const char *filepath, void (*callback)(const uint8_t *data, size_t length, void *user_data), void *user_data) {
    if (!callback) return ERR_INVALID_ARGUMENT;

    increment_load();

    FILE *file = fopen(filepath, "rb");
    if (!file) {
        decrement_load();
        return ERR_NOT_FOUND;
    }

    // Use a larger buffer size for audio streaming
    uint8_t buffer[8192];  // 8KB chunks
    size_t bytes_read;
    ErrorCode result = ERR_SUCCESS;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        callback(buffer, bytes_read, user_data);
    }

    if (ferror(file)) {
        result = ERR_IO_ERROR;
    }

    fclose(file);
    decrement_load();
    return result;
}

ErrorCode storage_get_file_info(const char *filepath, uint64_t *file_size, uint32_t *permissions) {
    struct stat st;
    if (stat(filepath, &st) != 0) {
        perror("stat");
        return ERR_NOT_FOUND;
    }

    *file_size = st.st_size;
    *permissions = st.st_mode & 0777; // Extract permission bits
    return ERR_SUCCESS;
}
