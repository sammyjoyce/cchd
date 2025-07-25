/*
 * HTTP communication for CCHD.
 * 
 * Provides a high-level HTTP client specifically designed for hook server communication.
 * We use libcurl for its robust handling of edge cases, proxy support, and SSL/TLS
 * capabilities that would be error-prone to implement from scratch. The interface
 * is kept minimal to reduce coupling with the underlying HTTP library.
 */

#pragma once

#include <stdint.h>

#include "../core/error.h"
#include "../core/types.h"

// Forward declaration avoids circular dependency with config.h.
// This allows the HTTP module to accept config without exposing config internals.
typedef struct cchd_config cchd_config_t;

// Initialize HTTP subsystem including libcurl global state.
// Must be called once before any HTTP operations to ensure thread safety
// and proper SSL initialization. Failure to initialize will cause crashes.
CCHD_NODISCARD cchd_error cchd_http_init(void);

// Cleanup HTTP subsystem to release global resources.
// Critical for preventing memory leaks in long-running processes or when
// CCHD is used as a library. Safe to call even if init failed.
void cchd_http_cleanup(void);

// Send request to server with automatic retry on transient failures.
// Implements exponential backoff for server errors and immediate retry for
// network errors. Returns the HTTP status code or negative error code.
// The retry logic helps ensure reliability in unstable network conditions.
CCHD_NODISCARD int32_t cchd_send_request_to_server(
    const cchd_config_t *config, const char *json_payload,
    cchd_response_buffer_t *server_response, const char *program_name);