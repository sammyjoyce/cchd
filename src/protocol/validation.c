/*
 * Input validation implementation.
 */

#include "validation.h"

#include <stdio.h>
#include <string.h>

#include "../core/config.h"
#include "../utils/colors.h"
#include "../utils/logging.h"

bool cchd_validate_server_url(const char *url, const cchd_config_t *config) {
  if (url == NULL || strlen(url) == 0) {
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      fprintf(stderr, "Error: Server URL cannot be empty\n");
    }
    return false;
  }

  // Must start with http:// or https://
  if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      const char *red = cchd_use_colors(config) ? COLOR_RED : "";
      const char *reset = cchd_use_colors(config) ? COLOR_RESET : "";
      fprintf(stderr, "%sError: Invalid URL format: %s%s\n", red, url, reset);
      fprintf(stderr, "URLs must start with 'http://' or 'https://'\n");
    }
    return false;
  }

  // Check for HTTPS enforcement
  if (!cchd_config_is_insecure(config) && strncmp(url, "http://", 7) == 0) {
    const char *host_start = url + 7;
    if (strncmp(host_start, "localhost", 9) != 0 &&
        strncmp(host_start, "127.0.0.1", 9) != 0 &&
        strncmp(host_start, "[::1]", 5) != 0) {
      if (!cchd_config_is_quiet(config) &&
          !cchd_config_is_json_output(config)) {
        const char *yellow = cchd_use_colors(config) ? COLOR_YELLOW : "";
        const char *reset = cchd_use_colors(config) ? COLOR_RESET : "";
        fprintf(stderr, "%sWarning: Using insecure HTTP connection to %s%s\n",
                yellow, url, reset);
        fprintf(stderr, "HTTPS is strongly recommended for production use.\n");
        fprintf(stderr, "To suppress this warning:\n");
        fprintf(stderr, "  • Use HTTPS instead: https://...\n");
        fprintf(stderr, "  • Or add --insecure flag (not recommended)\n\n");
      }
      LOG_WARNING("Insecure HTTP connection detected for non-localhost URL: %s",
                  url);
    }
  }

  // Validate URL components
  const char *scheme_end = strstr(url, "://");
  if (scheme_end == NULL) {
    return false;
  }

  const char *host_start = scheme_end + 3;
  if (*host_start == '\0') {
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      fprintf(stderr, "Error: URL missing host: %s\n", url);
    }
    return false;
  }

  if (*host_start == ':' || *host_start == '/') {
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      fprintf(stderr, "Error: URL missing hostname: %s\n", url);
    }
    return false;
  }

  // Validate URL length
  if (strlen(url) > 2048) {
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      fprintf(stderr, "Error: URL too long (max 2048 characters)\n");
    }
    return false;
  }

  // Check for spaces
  if (strstr(url, " ") != NULL) {
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      fprintf(stderr, "Error: URL contains spaces: %s\n", url);
    }
    return false;
  }

  return true;
}

bool cchd_validate_hook_event_fields(yyjson_val *input_root,
                                     const cchd_config_t *config) {
  if (!yyjson_is_obj(input_root)) {
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      fprintf(stderr, "Error: Input must be a JSON object\n");
    }
    return false;
  }

  // Required fields
  const char *required_fields[] = {"hook_event_name", "session_id"};

  for (size_t i = 0; i < sizeof(required_fields) / sizeof(required_fields[0]);
       i++) {
    yyjson_val *field = yyjson_obj_get(input_root, required_fields[i]);
    if (field == nullptr) {
      if (!cchd_config_is_quiet(config) &&
          !cchd_config_is_json_output(config)) {
        fprintf(stderr, "Error: Missing required field '%s'\n",
                required_fields[i]);
      }
      return false;
    }
  }

  // Validate hook_event_name
  yyjson_val *hook_name = yyjson_obj_get(input_root, "hook_event_name");
  if (!yyjson_is_str(hook_name)) {
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      fprintf(stderr, "Error: 'hook_event_name' must be a string\n");
    }
    return false;
  }

  const char *hook_name_str = yyjson_get_str(hook_name);
  const char *valid_hooks[] = {
      "PreToolUse", "PostToolUse",  "Notification", "UserPromptSubmit",
      "Stop",       "SubagentStop", "PreCompact"};

  bool valid_hook = false;
  for (size_t i = 0; i < sizeof(valid_hooks) / sizeof(valid_hooks[0]); i++) {
    if (strcmp(hook_name_str, valid_hooks[i]) == 0) {
      valid_hook = true;
      break;
    }
  }

  if (!valid_hook && !cchd_config_is_quiet(config) &&
      !cchd_config_is_json_output(config)) {
    fprintf(stderr, "Warning: Unknown hook_event_name '%s'\n", hook_name_str);
    fprintf(stderr, "See documentation for a list of valid hook events.\n");
  }

  // Validate session_id
  yyjson_val *session_id = yyjson_obj_get(input_root, "session_id");
  if (!yyjson_is_str(session_id)) {
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      fprintf(stderr, "Error: 'session_id' must be a string\n");
    }
    return false;
  }

  // Hook-specific validation
  if (strcmp(hook_name_str, "PreToolUse") == 0 ||
      strcmp(hook_name_str, "PostToolUse") == 0) {
    yyjson_val *tool_name = yyjson_obj_get(input_root, "tool_name");
    if (tool_name == nullptr && !cchd_config_is_quiet(config) &&
        !cchd_config_is_json_output(config)) {
      fprintf(stderr, "Warning: %s hook should include 'tool_name' field\n",
              hook_name_str);
    }
  }

  return true;
}