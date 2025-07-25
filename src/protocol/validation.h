/*
 * Input validation for CCHD.
 * 
 * Provides strict validation of inputs to prevent security issues and ensure
 * reliable operation. All external inputs (URLs, JSON) pass through validation
 * before processing. This defense-in-depth approach catches errors early and
 * provides clear feedback about what needs correction.
 */

#pragma once

#include <stdbool.h>
#include <yyjson.h>

#include "../core/types.h"

// Forward declaration to access validation configuration options.
// Config determines strictness levels and allowed URL schemes.
typedef struct cchd_config cchd_config_t;

// Validate server URL format for safety and correctness.
// Checks URL scheme (http/https), format validity, and dangerous patterns.
// Returns true only for safe, well-formed URLs. This prevents SSRF attacks
// and ensures reliable network communication.
bool cchd_validate_server_url(const char *url, const cchd_config_t *config);

// Validate hook event has required fields per Claude Code specification.
// Ensures tool_name and event_type are present and properly formatted.
// This validation ensures compatibility with the hook protocol and helps
// catch integration errors before they reach the server.
bool cchd_validate_hook_event_fields(yyjson_val *input_root,
                                     const cchd_config_t *config);