/*
 * Command-line argument parsing for CCHD.
 * 
 * Handles parsing of all command-line options and arguments with proper validation.
 * Supports both short and long option formats for user convenience. The parser
 * integrates with the config system to apply command-line overrides as the highest
 * priority configuration source.
 */

#pragma once

#include "../core/error.h"
#include "../core/types.h"

// Forward declaration avoids circular dependency with config.h.
// The parser updates config based on parsed arguments.
typedef struct cchd_config cchd_config_t;

// Parse command line arguments and update configuration accordingly.
// Returns CCHD_SUCCESS on success, or appropriate error code for invalid arguments.
// Special handling: exits with code 0 for --help/--version (not an error).
// This allows scripts to check cchd capabilities without error handling.
CCHD_NODISCARD cchd_error cchd_parse_args(int argc, char *argv[],
                                          cchd_config_t *config);