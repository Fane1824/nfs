// src/client/src/main.c

#include "client.h"
#include "protocol.h"
#include "network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>

#define MAX_ARGS 16
#define BUFFER_SIZE 4096
#define MAX_LINE 1024

static volatile int running = 1;
static Client *client = NULL;

static void handle_signal(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    running = 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <naming_server_host> <naming_server_port>\n", prog);
}

static void print_help() {
    printf(
        "Available commands:\n"
        "  create <path> <mode>           Create a new file\n"
        "  write <path> <offset> <data>   Write data to file\n"
        "  read <path> <offset> <length>  Read data from file\n"
        "  delete <path>                  Delete a file\n"
        "  stream <path>                  Stream audio file\n"
        "  info <path>                    Get file size and permissions\n"
        "  help                           Show this help\n"
        "  exit                           Exit the program\n");
}

// Remove the send_request_to_naming_server function
// ErrorCode send_request_to_naming_server(Client *client, MessageType type, char *path) {
//     // Function body removed because it's now handled in operations.c
// }

// Modify the command handlers to use client operations from operations.c

static void handle_read_command(Client *client, char **args, int argc) {
    if (argc != 4) {
        printf("Usage: read <path> <offset> <length>\n");
        return;
    }

    const char *path = args[1];
    uint64_t offset = strtoull(args[2], NULL, 10);
    size_t length = strtoull(args[3], NULL, 10);
    uint8_t buffer[BUFFER_SIZE];
    size_t bytes_read;

    // printf("path: %s\n", path);
    ErrorCode err = client_read(client, path, offset, buffer, length, &bytes_read);
    if (err == ERR_SUCCESS) {
        printf("Read %zu bytes: %.*s\n", bytes_read, (int)bytes_read, buffer);
    } else {
        printf("Failed to read from file: Error %d\n", err);
    }
}

static void handle_write_command(Client *client, char **args, int argc) {
    if (argc != 4) {
        printf("Usage: write <path> <offset> <data>\n");
        return;
    }

    const char *path = args[1];
    uint64_t offset = strtoull(args[2], NULL, 10);
    const uint8_t *data = (const uint8_t *)args[3];
    size_t length = strlen(args[3]);

    ErrorCode err = client_write(client, path, offset, data, length);
    if (err == ERR_SUCCESS) {
        printf("Write successful\n");
    } else {
        printf("Failed to write to file: Error %d\n", err);
    }
}

static void handle_create_command(Client *client, char **args, int argc) {
    if (argc != 3) {
        printf("Usage: create <path> <mode>\n");
        return;
    }

    const char *path = args[1];
    uint32_t mode = strtol(args[2], NULL, 8);

    ErrorCode err = client_create(client, path, mode);
    if (err == ERR_SUCCESS) {
        printf("File created successfully\n");
    } else {
        printf("Failed to create file: Error %d\n", err);
    }
}

static void handle_delete_command(Client *client, char **args, int argc) {
    if (argc != 2) {
        printf("Usage: delete <path>\n");
        return;
    }

    const char *path = args[1];

    ErrorCode err = client_delete(client, path);
    if (err == ERR_SUCCESS) {
        printf("File deleted successfully\n");
    } else {
        printf("Failed to delete file: Error %d\n", err);
    }
}

static void handle_stream_data(const uint8_t *data, size_t length, void *user_data) {
    printf("Received %zu bytes of streaming data\n", length);
    // Process streaming data as needed
}

static void audio_stream_callback(const uint8_t *data, size_t length, void *user_data) {
    // This callback will be called with chunks of audio data
    // In a real implementation, this would feed data to an audio player
    // For demonstration, we'll write to a file
    FILE *output = (FILE *)user_data;
    if (output) {
        fwrite(data, 1, length, output);
        fflush(output);
    }
}

// In main.c-1 - Update handle_stream_command
static void handle_stream_command(Client *client, char **args, int argc) {
    if (argc != 2) {
        printf("Usage: stream <path>\n");
        return;
    }

    const char *filepath = args[1];
    printf("Streaming audio from %s...\n", filepath);
    
    ErrorCode err = client_stream_audio_mpv(client, filepath);
    
    if (err != ERR_SUCCESS) {
        printf("Streaming failed with error: %d\n", err);
    }
}

static void handle_info_command(Client *client, char **args, int argc) {
    if (argc != 2) {
        printf("Usage: info <path>\n");
        return;
    }

    const char *path = args[1];
    uint64_t file_size;
    uint32_t permissions;

    ErrorCode err = client_get_file_info(client, path, &file_size, &permissions);
    if (err == ERR_SUCCESS) {
        printf("File: %s\nSize: %llu bytes\nPermissions: %o\n", path, (unsigned long long)file_size, permissions);
    } else {
        printf("Error getting file info: %d\n", err);
    }
}

static void parse_and_execute(Client *client, char *line) {
    if (!line) return;

    char *args[MAX_ARGS];
    int argc = 0;
    
    // Split line into arguments
    char *token = strtok(line, " \n");
    while (token && argc < MAX_ARGS) {
        args[argc++] = token;
        token = strtok(NULL, " \n");
    }

    if (argc == 0) return;

    if (strcmp(args[0], "help") == 0) {
        print_help();
    } else if (strcmp(args[0], "create") == 0) {
        handle_create_command(client, args, argc);
    } else if (strcmp(args[0], "write") == 0) {
        handle_write_command(client, args, argc);
    } else if (strcmp(args[0], "read") == 0) {
        handle_read_command(client, args, argc);
    } else if (strcmp(args[0], "delete") == 0) {
        handle_delete_command(client, args, argc);
    } else if (strcmp(args[0], "stream") == 0) {
        handle_stream_command(client, args, argc);
    } else if (strcmp(args[0], "info") == 0) {
        handle_info_command(client, args, argc);
    } else if (strcmp(args[0], "exit") == 0) {
        running = 0;
    } else {
        printf("Unknown command: %s\n", args[0]);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Set up signal handlers
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Initialize client
    if (client_init(&client, argv[1], argv[2]) != ERR_SUCCESS) {
        fprintf(stderr, "Failed to initialize client\n");
        return EXIT_FAILURE;
    }

    printf("Connected to naming server at %s:%s\n", argv[1], argv[2]);
    printf("Type 'help' for available commands\n");

    // Main command loop
    char line[MAX_LINE];
    while (running) {
        printf("nfs> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        parse_and_execute(client, line);
    }

    client_cleanup(client);
    printf("Client shut down cleanly\n");
    return EXIT_SUCCESS;
}