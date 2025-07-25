/*
 * Output handling for CCHD.
 * 
 * Manages output formatting to support multiple output modes (JSON, plain text, quiet).
 * The module ensures proper formatting based on configuration while maintaining
 * compatibility with downstream tools expecting specific formats. Output goes to
 * stdout for pipeline integration, with errors going to stderr.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../core/types.h"

// Forward declaration prevents circular dependency with config.h.
// Allows output module to respect config settings without tight coupling.
typedef struct cchd_config cchd_config_t;

// Output the appropriate response based on configuration and exit code.
// Handles modified output from server, original input passthrough, or error responses.
// The suppress_output flag allows hooks to block all output for security reasons.
// Exit code determines whether to output success or error formatting.
void cchd_handle_output(bool suppress_output, const char *modified_output_json,
                        const char *input_json_string,
                        const cchd_config_t *config, int32_t exit_code);