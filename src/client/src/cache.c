#include "client_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef struct CacheEntry {
    char *filepath;
    uint64_t offset;
    uint8_t *data;
    size_t length;
    struct CacheEntry *prev;
    struct CacheEntry *next;
} CacheEntry;

typedef struct {
    CacheEntry *head;
    CacheEntry *tail;
    size_t capacity;
    size_t size;
    pthread_mutex_t lock;
} Cache;

static Cache *cache = NULL;

ErrorCode cache_init(size_t capacity) {
    cache = malloc(sizeof(Cache));
    if (!cache) return ERR_INTERNAL_ERROR;
    cache->head = NULL;
    cache->tail = NULL;
    cache->capacity = capacity;
    cache->size = 0;
    pthread_mutex_init(&cache->lock, NULL);
    return ERR_SUCCESS;
}

void cache_cleanup() {
    if (!cache) return;

    pthread_mutex_lock(&cache->lock);
    CacheEntry *entry = cache->head;
    while (entry) {
        CacheEntry *next = entry->next;
        free(entry->filepath);
        free(entry->data);
        free(entry);
        entry = next;
    }
    pthread_mutex_unlock(&cache->lock);
    pthread_mutex_destroy(&cache->lock);
    free(cache);
    cache = NULL;
}

static void move_to_head(CacheEntry *entry) {
    if (entry == cache->head) return;

    if (entry->prev) entry->prev->next = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
    if (entry == cache->tail) cache->tail = entry->prev;

    entry->prev = NULL;
    entry->next = cache->head;
    if (cache->head) cache->head->prev = entry;
    cache->head = entry;
    if (!cache->tail) cache->tail = entry;
}

static void remove_tail() {
    if (!cache->tail) return;

    CacheEntry *tail = cache->tail;
    if (tail->prev) tail->prev->next = NULL;
    cache->tail = tail->prev;

    free(tail->filepath);
    free(tail->data);
    free(tail);
    cache->size--;
}

ErrorCode cache_get(const char *filepath, uint64_t offset, uint8_t *buffer, size_t length, size_t *bytes_read) {
    pthread_mutex_lock(&cache->lock);
    CacheEntry *entry = cache->head;
    while (entry) {
        if (strcmp(entry->filepath, filepath) == 0 && entry->offset == offset && entry->length >= length) {
            memcpy(buffer, entry->data, length);
            *bytes_read = length;
            move_to_head(entry);
            pthread_mutex_unlock(&cache->lock);
            return ERR_SUCCESS;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&cache->lock);
    *bytes_read = 0;
    return ERR_NOT_FOUND;
}

ErrorCode cache_put(const char *filepath, uint64_t offset, const uint8_t *data, size_t length) {
    pthread_mutex_lock(&cache->lock);

    CacheEntry *entry = cache->head;
    while (entry) {
        if (strcmp(entry->filepath, filepath) == 0 && entry->offset == offset) {
            free(entry->data);
            entry->data = malloc(length);
            if (!entry->data) {
                pthread_mutex_unlock(&cache->lock);
                return ERR_INTERNAL_ERROR;
            }
            memcpy(entry->data, data, length);
            entry->length = length;
            move_to_head(entry);
            pthread_mutex_unlock(&cache->lock);
            return ERR_SUCCESS;
        }
        entry = entry->next;
    }

    if (cache->size >= cache->capacity) {
        remove_tail();
    }

    entry = malloc(sizeof(CacheEntry));
    if (!entry) {
        pthread_mutex_unlock(&cache->lock);
        return ERR_INTERNAL_ERROR;
    }
    entry->filepath = strdup(filepath);
    entry->offset = offset;
    entry->data = malloc(length);
    if (!entry->filepath || !entry->data) {
        free(entry->filepath);
        free(entry->data);
        free(entry);
        pthread_mutex_unlock(&cache->lock);
        return ERR_INTERNAL_ERROR;
    }
    memcpy(entry->data, data, length);
    entry->length = length;
    entry->prev = NULL;
    entry->next = cache->head;

    if (cache->head) cache->head->prev = entry;
    cache->head = entry;
    if (!cache->tail) cache->tail = entry;

    cache->size++;
    pthread_mutex_unlock(&cache->lock);
    return ERR_SUCCESS;
}

ErrorCode cache_invalidate(const char *filepath, uint64_t offset) {
    pthread_mutex_lock(&cache->lock);
    CacheEntry *entry = cache->head;
    while (entry) {
        if (strcmp(entry->filepath, filepath) == 0 && entry->offset == offset) {
            if (entry->prev) entry->prev->next = entry->next;
            if (entry->next) entry->next->prev = entry->prev;
            if (entry == cache->head) cache->head = entry->next;
            if (entry == cache->tail) cache->tail = entry->prev;

            free(entry->filepath);
            free(entry->data);
            free(entry);
            cache->size--;
            break;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&cache->lock);
    return ERR_SUCCESS;
}