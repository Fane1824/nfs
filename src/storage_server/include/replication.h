#ifndef REPLICATION_H
#define REPLICATION_H

#include "errors.h"
#include "storage.h"
#include <stdint.h>

// Initialize the replication system
ErrorCode replication_init();

// Clean up the replication system
void replication_cleanup();

// Add a secondary storage server
ErrorCode replication_add_secondary(const char *host, const char *port);

// Remove a secondary storage server
ErrorCode replication_remove_secondary(const char *host, const char *port);

// Replicate write operation to secondaries
ErrorCode replication_replicate_write(const char *filepath, uint64_t offset, const uint8_t *buffer, size_t length);

// Replicate delete operation to secondaries
ErrorCode replication_replicate_delete(const char *filepath);

// Failure detection and recovery
void replication_check_health();

#endif // REPLICATION_H