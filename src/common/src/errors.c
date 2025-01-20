#include "errors.h"

const char *error_string(ErrorCode code) {
    switch (code) {
        case ERR_SUCCESS:
            return "Success";
        case ERR_UNKNOWN:
            return "Unknown error";
        case ERR_INVALID_ARGUMENT:
            return "Invalid argument";
        case ERR_NOT_FOUND:
            return "Not found";
        case ERR_ACCESS_DENIED:
            return "Access denied";
        case ERR_IO_ERROR:
            return "I/O error";
        case ERR_NETWORK_FAILURE:
            return "Network failure";
        case ERR_TIMEOUT:
            return "Timeout occurred";
        case ERR_PROTOCOL_ERROR:
            return "Protocol error";
        case ERR_INTERNAL_ERROR:
            return "Internal error";
        default:
            return "Unrecognized error code";
    }
}