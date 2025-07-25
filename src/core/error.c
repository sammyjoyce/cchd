/*
 * Error handling implementation for CCHD.
 * 
 * Provides human-readable error messages for all error codes. We use a simple
 * switch statement rather than a lookup table to ensure compile-time verification
 * that all error codes have corresponding messages, preventing oversight when
 * adding new error codes.
 */

#include "error.h"

const char *cchd_strerror(cchd_error error_code) {
  switch (error_code) {
  // Success and hook-specific codes: These messages are shown during normal
  // operation and should be clear about the action required from the user.
  case CCHD_SUCCESS:
    return "Success";
  case CCHD_ERROR_BLOCKED:
    return "Operation blocked";
  case CCHD_ERROR_ASK_USER:
    return "User approval required";

  // Input/configuration errors: Messages guide users to fix their input.
  // We provide specific details about what went wrong to reduce debugging time.
  case CCHD_ERROR_INVALID_ARG:
    return "Invalid argument";
  case CCHD_ERROR_INVALID_URL:
    return "Invalid URL format";
  case CCHD_ERROR_INVALID_JSON:
    return "JSON parse error";
  case CCHD_ERROR_INVALID_HOOK:
    return "Invalid hook event or missing required fields";
  case CCHD_ERROR_CONFIG:
    return "Configuration file error";
  case CCHD_ERROR_CONFIG_PARSE:
    return "Configuration file JSON parse error";
  case CCHD_ERROR_CONFIG_INVALID:
    return "Configuration file has invalid values";

  // Network/communication errors: Messages help users diagnose connectivity issues.
  // We distinguish between different failure modes to guide appropriate remediation.
  case CCHD_ERROR_NETWORK:
    return "Network error";
  case CCHD_ERROR_CONNECTION:
    return "Connection refused or failed";
  case CCHD_ERROR_TIMEOUT:
    return "Request timeout";
  case CCHD_ERROR_TLS:
    return "TLS/SSL error";
  case CCHD_ERROR_DNS:
    return "DNS resolution failure";
  case CCHD_ERROR_HTTP_CLIENT:
    return "HTTP client error (4xx)";
  case CCHD_ERROR_HTTP_SERVER:
    return "HTTP server error (5xx)";
  case CCHD_ERROR_RATE_LIMIT:
    return "Rate limit exceeded";
  case CCHD_ERROR_AUTH:
    return "Authentication/authorization error";
  case CCHD_ERROR_PROXY:
    return "Proxy connection error";

  // System errors: Messages indicate serious problems requiring system-level fixes.
  // These are kept concise as they often appear in logs that administrators review.
  case CCHD_ERROR_MEMORY:
    return "Memory allocation error";
  case CCHD_ERROR_IO:
    return "I/O error";
  case CCHD_ERROR_PERMISSION:
    return "Permission denied";
  case CCHD_ERROR_INTERNAL:
    return "Internal error";
  case CCHD_ERROR_THREADING:
    return "Thread/mutex error";
  case CCHD_ERROR_RESOURCE:
    return "Resource exhaustion";
  case CCHD_ERROR_SIGNAL:
    return "Signal handling error";

  // Server response errors: Messages help debug protocol mismatches and server issues.
  // We provide enough detail to distinguish between client bugs and server problems.
  case CCHD_ERROR_SERVER_INVALID:
    return "Invalid server response format";
  case CCHD_ERROR_SERVER_MODIFY:
    return "Server returned modified data";
  case CCHD_ERROR_ALL_SERVERS_FAILED:
    return "All servers failed";
  case CCHD_ERROR_PROTOCOL:
    return "Protocol violation";
  case CCHD_ERROR_JSON_MISSING_FIELD:
    return "Required JSON field missing";
  case CCHD_ERROR_JSON_TYPE_MISMATCH:
    return "JSON field has wrong type";
  case CCHD_ERROR_SERVER_BUSY:
    return "Server temporarily unavailable";
  case CCHD_ERROR_UNSUPPORTED:
    return "Unsupported operation/feature";

  default:
    return "Unknown error";
  }
}