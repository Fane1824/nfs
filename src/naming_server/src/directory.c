#define __USE_GNU
#define _GNU_SOURCE
#include "directory.h"
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h> //! debug

// Root of the directory tree
static DirectoryEntry *root = NULL;
static pthread_rwlock_t tree_lock = PTHREAD_RWLOCK_INITIALIZER;

// Initialize the directory manager
ErrorCode directory_init() {
    root = malloc(sizeof(DirectoryEntry));
    if (!root) return ERR_INTERNAL_ERROR;

    root->name = strdup("/");
    root->is_directory = 1;
    root->metadata = NULL;
    root->parent = NULL;
    root->children = NULL;
    root->child_count = 0;
    pthread_rwlock_init(&root->lock, NULL);

    return ERR_SUCCESS;
}

// Clean up a directory entry recursively
static void directory_free(DirectoryEntry *entry) {
    if (!entry) return;

    pthread_rwlock_wrlock(&entry->lock);
    for (size_t i = 0; i < entry->child_count; ++i) {
        directory_free(entry->children[i]);
    }
    if (entry->children) free(entry->children);
    free(entry->name);
    if (entry->metadata) {
        free(entry->metadata->storage_server_ip);
        free(entry->metadata);
    }
    pthread_rwlock_unlock(&entry->lock);
    pthread_rwlock_destroy(&entry->lock);
    free(entry);
}

// Clean up the directory manager
void directory_cleanup() {
    pthread_rwlock_wrlock(&tree_lock);
    directory_free(root);
    root = NULL;
    pthread_rwlock_unlock(&tree_lock);
    pthread_rwlock_destroy(&tree_lock);
}

// Helper function to tokenize path
static char **split_path(const char *path, size_t *count) {
    char *path_copy = strdup(path);
    if (!path_copy) return NULL;

    char **tokens = NULL;
    size_t tokens_count = 0;
    char *token = strtok(path_copy, "/");
    while (token) {
        char **new_tokens = realloc(tokens, sizeof(char *) * (tokens_count + 1));
        if (!new_tokens) {
            free(tokens);
            free(path_copy);
            return NULL;
        }
        tokens = new_tokens;
        tokens[tokens_count++] = strdup(token);
        token = strtok(NULL, "/");
    }
    free(path_copy);
    *count = tokens_count;
    return tokens;
}

// Helper function to free tokenized path
static void free_tokens(char **tokens, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        free(tokens[i]);
    }
    free(tokens);
}

// Internal function for path lookup
static ErrorCode directory_lookup_internal(const char *path, DirectoryEntry **result, int create, int is_directory) {
    if (!root || !path || !result) return ERR_INVALID_ARGUMENT;

    size_t tokens_count = 0;
    char **tokens = split_path(path, &tokens_count);
    if ((!tokens && tokens_count > 0) || (tokens_count == 0 && strcmp(path, "/") != 0)) {
        free_tokens(tokens, tokens_count);
        return ERR_INVALID_ARGUMENT;
    }

    DirectoryEntry *current = root;

    for (size_t i = 0; i < tokens_count; ++i) {
        pthread_rwlock_wrlock(&current->lock); // Lock current node for write
        DirectoryEntry *child = NULL;

        // Search for the child
        for (size_t j = 0; j < current->child_count; ++j) {
            if (strcmp(current->children[j]->name, tokens[i]) == 0) {
                child = current->children[j];
                break;
            }
        }

        if (!child) {
            if (create) {
                // Create new entry
                child = malloc(sizeof(DirectoryEntry));
                if (!child) {
                    pthread_rwlock_unlock(&current->lock);
                    free_tokens(tokens, tokens_count);
                    return ERR_INTERNAL_ERROR;
                }
                child->name = strdup(tokens[i]);
                child->is_directory = (i < tokens_count - 1) || is_directory;
                child->metadata = NULL;
                child->parent = current;
                child->children = NULL;
                child->child_count = 0;
                pthread_rwlock_init(&child->lock, NULL);

                // Add child to current
                DirectoryEntry **new_children = realloc(current->children, sizeof(DirectoryEntry *) * (current->child_count + 1));
                if (!new_children) {
                    free(child->name);
                    free(child);
                    pthread_rwlock_unlock(&current->lock);
                    free_tokens(tokens, tokens_count);
                    return ERR_INTERNAL_ERROR;
                }
                current->children = new_children;
                current->children[current->child_count++] = child;
            } else {
                pthread_rwlock_unlock(&current->lock);
                free_tokens(tokens, tokens_count);
                return ERR_NOT_FOUND;
            }
        } else {
            pthread_rwlock_rdlock(&child->lock); // Lock child node
        }

        pthread_rwlock_unlock(&current->lock); // Unlock current node
        current = child; // Move to child
    }

    pthread_rwlock_unlock(&current->lock); // Unlock the last node
    *result = current;
    free_tokens(tokens, tokens_count);
    return ERR_SUCCESS;
}

// Public API functions

ErrorCode directory_lookup(const char *path, DirectoryEntry **entry) {
    return directory_lookup_internal(path, entry, 0, 0);
}

ErrorCode directory_create(const char *path) {
    printf("Creating directory: %s\n", path); //! debug
    DirectoryEntry *entry;
    return directory_lookup_internal(path, &entry, 1, 1);
}

ErrorCode directory_delete(const char *path) {
    DirectoryEntry *entry;
    ErrorCode err = directory_lookup(path, &entry);
    if (err != ERR_SUCCESS) return err;

    pthread_rwlock_wrlock(&entry->lock);
    if (entry->child_count > 0) {
        pthread_rwlock_unlock(&entry->lock);
        return ERR_INVALID_ARGUMENT;
    }

    DirectoryEntry *parent = entry->parent;
    if (parent) {
        // Lock parent before modifying
        pthread_rwlock_wrlock(&parent->lock);

        // Find and remove entry from parent's children
        for (size_t i = 0; i < parent->child_count; ++i) {
            if (parent->children[i] == entry) {
                // Shift the remaining children
                for (size_t j = i; j < parent->child_count - 1; ++j) {
                    parent->children[j] = parent->children[j + 1];
                }
                parent->child_count--;
                parent->children = realloc(parent->children, sizeof(DirectoryEntry *) * parent->child_count);
                break;
            }
        }

        pthread_rwlock_unlock(&parent->lock);
    }

    pthread_rwlock_unlock(&entry->lock);
    directory_free(entry);
    return ERR_SUCCESS;
}

ErrorCode directory_register_file(const char *path, FileMetadata *metadata) {
    DirectoryEntry *entry;
    ErrorCode err = directory_lookup_internal(path, &entry, 1, 0);
    if (err != ERR_SUCCESS) return err;
    printf("directory lookup successful\n"); //! debug

    // pthread_rwlock_wrlock(&entry->lock);
    if (entry->metadata) free(entry->metadata);
    entry->metadata = malloc(sizeof(FileMetadata));
    if (!entry->metadata) {
        pthread_rwlock_unlock(&entry->lock);
        return ERR_INTERNAL_ERROR;
    }
    memcpy(entry->metadata, metadata, sizeof(FileMetadata));
    pthread_rwlock_unlock(&entry->lock);

    return ERR_SUCCESS;
}

ErrorCode directory_get_metadata(const char *path, FileMetadata **metadata) {
    DirectoryEntry *entry;
    ErrorCode err = directory_lookup(path, &entry);
    if (err != ERR_SUCCESS) return err;

    pthread_rwlock_rdlock(&entry->lock);
    if (!entry->metadata) {
        pthread_rwlock_unlock(&entry->lock);
        return ERR_NOT_FOUND;
    }
    *metadata = malloc(sizeof(FileMetadata));
    if (!*metadata) {
        pthread_rwlock_unlock(&entry->lock);
        return ERR_INTERNAL_ERROR;
    }
    memcpy(*metadata, entry->metadata, sizeof(FileMetadata));
    pthread_rwlock_unlock(&entry->lock);

    return ERR_SUCCESS;
}