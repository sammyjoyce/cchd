/*
 * Configuration management for CCHD.
 * 
 * Implements a layered configuration system where values can come from files,
 * environment variables, and command-line arguments, with later sources overriding
 * earlier ones. This approach allows users to set defaults in config files while
 * overriding specific values for individual runs via command line.
 */

#pragma once

#include <stdbool.h>

#include "error.h"
#include "types.h"

// Opaque configuration structure hides implementation details from callers.
// This allows us to change the internal representation without breaking API
// compatibility, and prevents direct manipulation that could leave config in
// an inconsistent state.
typedef struct cchd_config cchd_config_t;

// Create and destroy configuration objects with proper lifecycle management.
// The create function allocates and initializes with defaults, while destroy
// ensures all allocated resources (URLs, keys) are properly freed to prevent leaks.
CCHD_NODISCARD cchd_error cchd_config_create(cchd_config_t **config);
void cchd_config_destroy(cchd_config_t *config);

// Load configuration from various sources with cumulative override semantics.
// Each load function merges new values with existing configuration, allowing
// users to build up configuration in layers: file -> environment -> command line.
// This precedence order ensures command-line args always win for maximum flexibility.
CCHD_NODISCARD cchd_error cchd_config_load_file(cchd_config_t *config,
                                                const char *path);
CCHD_NODISCARD cchd_error cchd_config_load_env(cchd_config_t *config);
CCHD_NODISCARD cchd_error cchd_config_load_args(cchd_config_t *config, int argc,
                                                char *argv[]);

// Configuration getters provide read-only access to ensure thread safety.
// By returning const pointers and values, we prevent accidental modification
// of shared configuration state and enable safe concurrent access from multiple threads.
const char *cchd_config_get_server_url(const cchd_config_t *config,
                                       size_t index);
size_t cchd_config_get_server_count(const cchd_config_t *config);
const char *cchd_config_get_api_key(const cchd_config_t *config);
int64_t cchd_config_get_timeout_ms(const cchd_config_t *config);
bool cchd_config_is_fail_open(const cchd_config_t *config);
bool cchd_config_is_quiet(const cchd_config_t *config);
bool cchd_config_is_debug(const cchd_config_t *config);
bool cchd_config_is_json_output(const cchd_config_t *config);
bool cchd_config_is_plain_output(const cchd_config_t *config);
bool cchd_config_is_no_color(const cchd_config_t *config);
bool cchd_config_is_no_input(const cchd_config_t *config);
bool cchd_config_is_insecure(const cchd_config_t *config);

// Configuration setters for programmatic use during initialization.
// These are primarily used by the load functions and testing code.
// Application code should prefer using the load functions to ensure
// proper validation and consistent behavior.
void cchd_config_set_debug(cchd_config_t *config, bool debug);
void cchd_config_add_server_url(cchd_config_t *config, const char *url);