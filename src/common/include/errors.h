#ifndef ERRORS_H
#define ERRORS_H

typedef enum {
    ERR_SUCCESS = 0,
    ERR_UNKNOWN = -1,
    ERR_INVALID_ARGUMENT = -2,
    ERR_NOT_FOUND = -3,
    ERR_ACCESS_DENIED = -4,
    ERR_IO_ERROR = -5,
    ERR_NETWORK_FAILURE = -6,
    ERR_TIMEOUT = -7,
    ERR_PROTOCOL_ERROR = -8,
    ERR_INTERNAL_ERROR = -9,
    ERR_FILE_NOT_FOUND = -10,
} ErrorCode;

const char *error_string(ErrorCode code);

#endif // ERRORS_H