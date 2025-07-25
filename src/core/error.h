/*
 * Error handling definitions for CCHD.
 * 
 * Centralizes all error codes to ensure consistent error reporting across the application.
 * By using numeric codes with human-readable descriptions, we enable both programmatic
 * error handling and meaningful user feedback. The error code ranges are designed to
 * help quickly identify the error category during debugging.
 */

#pragma once

#include "types.h"

// Error codes are grouped by category with reserved ranges to aid debugging.
// Each range represents a different layer of the application, making it easier
// to identify where failures occur without examining stack traces.
typedef enum {
  // Success and hook-specific codes (0-2): These special codes indicate either
  // successful execution or hook-specific control flow (blocked, ask user).
  // We reserve low numbers for these as they represent normal operation paths.
  CCHD_SUCCESS = 0,
  CCHD_ERROR_BLOCKED = 1,
  CCHD_ERROR_ASK_USER = 2,

  // Input/configuration errors (3-9): User-correctable errors that typically
  // occur during startup or argument parsing. These errors indicate the user
  // needs to fix their input or configuration rather than a system failure.
  CCHD_ERROR_INVALID_ARG = 3,
  CCHD_ERROR_INVALID_URL = 4,
  CCHD_ERROR_INVALID_JSON = 5,
  CCHD_ERROR_INVALID_HOOK = 6,
  CCHD_ERROR_CONFIG = 7,
  CCHD_ERROR_CONFIG_PARSE = 8,
  CCHD_ERROR_CONFIG_INVALID = 9,

  // Network/communication errors (10-19): Transient errors that may succeed
  // on retry. We group these together because they often share similar retry
  // strategies and user mitigation approaches (check connection, wait, retry).
  CCHD_ERROR_NETWORK = 10,
  CCHD_ERROR_CONNECTION = 11,
  CCHD_ERROR_TIMEOUT = 12,
  CCHD_ERROR_TLS = 13,
  CCHD_ERROR_DNS = 14,
  CCHD_ERROR_HTTP_CLIENT = 15,
  CCHD_ERROR_HTTP_SERVER = 16,
  CCHD_ERROR_RATE_LIMIT = 17,
  CCHD_ERROR_AUTH = 18,
  CCHD_ERROR_PROXY = 19,

  // System errors (20-29): Critical failures that typically cannot be recovered
  // from without administrator intervention. These indicate resource exhaustion,
  // permission issues, or internal bugs that require investigation.
  CCHD_ERROR_MEMORY = 20,
  CCHD_ERROR_IO = 21,
  CCHD_ERROR_PERMISSION = 22,
  CCHD_ERROR_INTERNAL = 23,
  CCHD_ERROR_THREADING = 24,
  CCHD_ERROR_RESOURCE = 25,
  CCHD_ERROR_SIGNAL = 26,

  // Server response errors (30-39): Errors in the server's response format or content.
  // These are separated from network errors because they indicate successful communication
  // but invalid data, requiring different debugging approaches than connection issues.
  CCHD_ERROR_SERVER_INVALID = 30,
  CCHD_ERROR_SERVER_MODIFY = 31,
  CCHD_ERROR_ALL_SERVERS_FAILED = 32,
  CCHD_ERROR_PROTOCOL = 33,
  CCHD_ERROR_JSON_MISSING_FIELD = 34,
  CCHD_ERROR_JSON_TYPE_MISMATCH = 35,
  CCHD_ERROR_SERVER_BUSY = 36,
  CCHD_ERROR_UNSUPPORTED = 37,
} cchd_error;

// Get human-readable error description for user-facing messages.
// This function ensures users receive meaningful feedback instead of cryptic error codes,
// improving the debugging experience and reducing support burden.
CCHD_NODISCARD const char *cchd_strerror(cchd_error error_code);

// Retry state enum distinguishes between network-level and server-level failures.
// RETRY_NETWORK (-1) indicates connection issues warranting immediate retry,
// while RETRY_SERVER (500) suggests server overload requiring exponential backoff.
// These distinctions enable intelligent retry strategies that avoid overwhelming servers.
typedef enum {
  CCHD_RETRY_NETWORK = -1,
  CCHD_RETRY_SERVER = 500
} cchd_retry_state;