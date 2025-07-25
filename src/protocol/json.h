/*
 * JSON processing for CCHD.
 * 
 * Central JSON handling using yyjson for performance and correctness.
 * We chose yyjson for its speed, small size, and strict JSON compliance.
 * All JSON operations go through this module to ensure consistent error
 * handling and proper memory management of JSON documents.
 */

#pragma once

#include <stdint.h>
#include <yyjson.h>

#include "../core/error.h"
#include "../core/types.h"

// Forward declaration to access configuration without circular dependency.
// Needed to check output format preferences during JSON processing.
typedef struct cchd_config cchd_config_t;

// Generate RFC3339 timestamp for CloudEvents time field.
// Returns heap-allocated string in format: 2024-03-14T12:34:56.789Z.
// RFC3339 is required by CloudEvents spec and ensures consistent timestamps
// across timezones. Caller must free the returned string.
CCHD_NODISCARD char *cchd_generate_rfc3339_timestamp(void);

// Process input JSON and transform to CloudEvents format for server transmission.
// Validates input JSON, adds CloudEvents envelope, and returns formatted string.
// Returns NULL on error. This transformation enables standardized event processing
// while preserving the original event data in the CloudEvents data field.
CCHD_NODISCARD char *cchd_process_input_to_protocol(
    const char *input_json_string, const cchd_config_t *config);

// Process server response and extract action directives.
// Parses response JSON and handles action fields (exit_code, output, suppress_output).
// Updates provided pointers with results. Returns error code if response is invalid.
// This careful parsing ensures we only act on valid server instructions.
CCHD_NODISCARD cchd_error cchd_process_server_response(
    const char *response_data, char **modified_output_ptr,
    const cchd_config_t *config, bool *suppress_output_ptr,
    int32_t server_http_status, int32_t *exit_code_out);