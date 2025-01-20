#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// File metadata structure
typedef struct FileMetadata {
    char *storage_server_ip;
    uint16_t storage_server_port;
    uint64_t size;
    uint32_t permissions;
    // Additional metadata fields
} FileMetadata;

typedef enum {
    MSG_TYPE_READ = 1,
    MSG_TYPE_WRITE,
    MSG_TYPE_CREATE,
    MSG_TYPE_DELETE,
    MSG_TYPE_STREAM,
    MSG_TYPE_GET_LOCATION,
    MSG_TYPE_LOCATION,
    MSG_TYPE_ERROR,
    MSG_TYPE_HEARTBEAT,
    MSG_TYPE_REPLICATE_WRITE,
    MSG_TYPE_REPLICATE_DELETE,
    MSG_TYPE_SS_REGISTER,
    MSG_TYPE_SS_REGISTER_ACK,
    MSG_TYPE_GET_FILE_INFO = 20,           // Assign an unused message type number
    MSG_TYPE_GET_FILE_INFO_RESPONSE = 21,
    MSG_TYPE_STREAM_DATA = 22,
    MSG_TYPE_STREAM_CONTROL = 23,
    MSG_TYPE_STREAM_METADATA = 24,
    MSG_TYPE_STREAM_END = 25,
} MessageType;

typedef struct {
    char host[256];
    char port[32];
    int load;
} HeartbeatMessage;

typedef struct {
    uint32_t request_id;
    MessageType type;
    uint32_t payload_size;
} MessageHeader;

// Storage Server Registration Message
typedef struct {
    uint16_t port;
    uint32_t num_paths;
    // Paths follow
} SSRegisterMessage;

typedef struct {
    MessageHeader header;
    char filepath[256];
    uint64_t offset;
    uint32_t length;
} ReadRequest;

typedef struct {
    MessageHeader header;
    char filepath[256];
    uint64_t offset;
    uint32_t length;
    // Data follows
} WriteRequest;

typedef struct {
    MessageHeader header;
    char filepath[256];
    uint32_t mode;
} CreateRequest;

typedef struct {
    MessageHeader header;
    char filepath[256];
} DeleteRequest;



// Structure for get_file_info request
typedef struct {
    char filepath[256];
} __attribute__((packed)) GetFileInfoRequest;

// Structure for get_file_info response
typedef struct {
    uint64_t file_size;
    uint32_t permissions;
} __attribute__((packed)) GetFileInfoResponse;

// Audio format metadata
typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    char format[32];  // e.g., "MP3", "WAV", "FLAC"
    uint64_t duration_ms;
    uint64_t total_size;
} AudioMetadata;

// Enhanced stream request with seeking support
typedef struct {
    MessageHeader header;
    char filepath[256];
    uint64_t start_position;    // Seek position in bytes
    uint32_t chunk_size;        // Preferred chunk size for streaming
    uint8_t metadata_only;      // Flag to request only metadata
} StreamRequest;

// Stream control message
typedef struct {
    MessageHeader header;
    enum {
        STREAM_PAUSE,
        STREAM_RESUME,
        STREAM_SEEK,
        STREAM_STOP
    } action;
    uint64_t seek_position;     // Used for STREAM_SEEK
} StreamControl;

// Stream data chunk
typedef struct {
    MessageHeader header;
    uint64_t offset;           // Current position in stream
    uint32_t chunk_size;       // Size of this chunk
    uint8_t is_last_chunk;     // Flag for end of stream
    // Raw audio data follows
} StreamData;

// Stream metadata response
typedef struct {
    MessageHeader header;
    AudioMetadata metadata;
} StreamMetadataResponse;

#endif // PROTOCOL_H