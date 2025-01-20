#include "cache.h"
#include "directory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef struct CacheEntry {
    char *path;
    DirectoryEntry *dir_entry;
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
        free(entry->path);
        // DirectoryEntry is managed elsewhere
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

    free(tail->path);
    // DirectoryEntry is managed elsewhere
    free(tail);
    cache->size--;
}

ErrorCode cache_get(const char *path, DirectoryEntry **dir_entry) {
    pthread_mutex_lock(&cache->lock);
    CacheEntry *entry = cache->head;
    while (entry) {
        if (strcmp(entry->path, path) == 0) {
            *dir_entry = entry->dir_entry;
            move_to_head(entry);
            pthread_mutex_unlock(&cache->lock);
            return ERR_SUCCESS;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&cache->lock);
    return ERR_NOT_FOUND;
}

ErrorCode cache_put(const char *path, DirectoryEntry *dir_entry) {
    pthread_mutex_lock(&cache->lock);

    CacheEntry *entry = cache->head;
    while (entry) {
        if (strcmp(entry->path, path) == 0) {
            entry->dir_entry = dir_entry;
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
    entry->path = strdup(path);
    entry->dir_entry = dir_entry;
    entry->prev = NULL;
    entry->next = cache->head;

    if (cache->head) cache->head->prev = entry;
    cache->head = entry;
    if (!cache->tail) cache->tail = entry;

    cache->size++;
    pthread_mutex_unlock(&cache->lock);
    return ERR_SUCCESS;
}

ErrorCode cache_invalidate(const char *path) {
    pthread_mutex_lock(&cache->lock);
    CacheEntry *entry = cache->head;
    while (entry) {
        if (strcmp(entry->path, path) == 0) {
            if (entry->prev) entry->prev->next = entry->next;
            if (entry->next) entry->next->prev = entry->prev;
            if (entry == cache->head) cache->head = entry->next;
            if (entry == cache->tail) cache->tail = entry->prev;

            free(entry->path);
            // DirectoryEntry is managed elsewhere
            free(entry);
            cache->size--;
            break;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&cache->lock);
    return ERR_SUCCESS;
}