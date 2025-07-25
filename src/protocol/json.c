/*
 * JSON processing implementation.
 */

#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core/config.h"
#include "../utils/colors.h"
#include "../utils/logging.h"
#include "../utils/memory.h"
#include "cloudevents.h"
#include "validation.h"

char *cchd_generate_rfc3339_timestamp(void) {
  if (TIMESTAMP_BUFFER_SIZE < 32) {
    LOG_ERROR("Timestamp buffer size too small");
    return nullptr;
  }

  time_t now = time(nullptr);
  if (now == (time_t)-1) {
    LOG_ERROR("Failed to get current time");
    errno = EINVAL;
    return nullptr;
  }

  struct tm tm_buf;
  struct tm *tm_info = gmtime_r(&now, &tm_buf);
  if (tm_info == nullptr) {
    LOG_ERROR("gmtime_r failed for timestamp %ld", (long)now);
    errno = EINVAL;
    return nullptr;
  }

  char *timestamp = cchd_secure_malloc(TIMESTAMP_BUFFER_SIZE);
  if (timestamp == nullptr) {
    return nullptr;
  }

  // Format timestamp according to RFC 3339
  if (strftime(timestamp, TIMESTAMP_BUFFER_SIZE, "%Y-%m-%dT%H:%M:%SZ",
               tm_info) == 0) {
    cchd_secure_free(timestamp, TIMESTAMP_BUFFER_SIZE);
    return NULL;
  }

  return timestamp;
}

char *cchd_process_input_to_protocol(const char *input_json_string,
                                     const cchd_config_t *config) {
  if (input_json_string == NULL || strlen(input_json_string) == 0) {
    LOG_ERROR("Invalid input JSON string");
    return NULL;
  }

  yyjson_read_err err;
  memset(&err, 0, sizeof(err));
  yyjson_doc *input_json_document = yyjson_read_opts(
      (char *)input_json_string, strlen(input_json_string), 0, NULL, &err);

  if (input_json_document == NULL) {
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      const char *red = cchd_use_colors(config) ? COLOR_RED : "";
      const char *yellow = cchd_use_colors(config) ? COLOR_YELLOW : "";
      const char *reset = cchd_use_colors(config) ? COLOR_RESET : "";

      fprintf(stderr,
              "\n%sFailed to parse input JSON at position %zu: %s%s\n\n", red,
              err.pos, err.msg, reset);
      fprintf(stderr, "Common JSON issues:\n");
      fprintf(stderr, "  • Missing quotes around strings\n");
      fprintf(stderr, "  • Trailing commas\n");
      fprintf(stderr, "  • Unescaped special characters\n\n");
      fprintf(stderr, "Example of valid input:\n");
      fprintf(stderr,
              "  %secho "
              "'{\"hook_event_name\":\"PreToolUse\",\"session_id\":\"abc123\"}'"
              " | cchd%s\n\n",
              yellow, reset);
      fprintf(stderr, "You can validate your JSON at https://jsonlint.com/\n");
    }
    return NULL;
  }

  // Validate required hook fields
  yyjson_val *input_root = yyjson_doc_get_root(input_json_document);
  if (!cchd_validate_hook_event_fields(input_root, config)) {
    yyjson_doc_free(input_json_document);
    return NULL;
  }

  // Transform to CloudEvents format
  yyjson_mut_doc *protocol_json_document =
      cchd_transform_to_cloudevents(input_json_document);
  yyjson_doc_free(input_json_document);

  if (protocol_json_document == NULL) {
    return NULL;
  }

  size_t json_len = 0;
  yyjson_write_err write_err;
  memset(&write_err, 0, sizeof(write_err));
  char *protocol_json_string = yyjson_mut_write_opts(
      protocol_json_document, YYJSON_WRITE_NOFLAG, NULL, &json_len, &write_err);
  yyjson_mut_doc_free(protocol_json_document);

  if (protocol_json_string == NULL) {
    return NULL;
  }

  // Copy to secure memory
  char *secure_json = cchd_secure_malloc(json_len + 1);
  if (secure_json == NULL) {
    free(protocol_json_string);
    return NULL;
  }
  memcpy(secure_json, protocol_json_string, json_len + 1);
  free(protocol_json_string);

  return secure_json;
}

static void parse_base_response(yyjson_val *response_root, bool *continue_out,
                                bool *suppress_output_out,
                                const char **stop_reason_out) {
  if (response_root == NULL || continue_out == NULL ||
      suppress_output_out == NULL || stop_reason_out == NULL ||
      !yyjson_is_obj(response_root)) {
    LOG_ERROR("Invalid parameters in parse_base_response");
    return;
  }

  yyjson_val *continue_value = yyjson_obj_get(response_root, "continue");
  if (yyjson_is_bool(continue_value)) {
    *continue_out = yyjson_get_bool(continue_value);
  } else {
    *continue_out = true;
  }

  yyjson_val *stop_reason_value = yyjson_obj_get(response_root, "stopReason");
  if (yyjson_is_str(stop_reason_value)) {
    *stop_reason_out = yyjson_get_str(stop_reason_value);
  } else {
    *stop_reason_out = NULL;
  }

  yyjson_val *suppress_output_value =
      yyjson_obj_get(response_root, "suppressOutput");
  if (yyjson_is_bool(suppress_output_value)) {
    *suppress_output_out = yyjson_get_bool(suppress_output_value);
  } else {
    *suppress_output_out = false;
  }
}

static const char *parse_decision(yyjson_val *response_root) {
  if (response_root == NULL || !yyjson_is_obj(response_root)) {
    LOG_ERROR("Invalid parameters in parse_decision");
    return NULL;
  }

  yyjson_val *decision_value = yyjson_obj_get(response_root, "decision");
  if (yyjson_is_str(decision_value)) {
    return yyjson_get_str(decision_value);
  }
  return NULL;
}

static void handle_decision(const char *decision, yyjson_val *response_root,
                            int32_t *exit_code_out) {
  if (decision == NULL || response_root == NULL || exit_code_out == NULL ||
      strlen(decision) == 0) {
    LOG_ERROR("Invalid parameters in handle_decision");
    if (exit_code_out != NULL) {
      *exit_code_out = 1;
    }
    return;
  }

  const char *reason = NULL;
  yyjson_val *reason_value = yyjson_obj_get(response_root, "reason");
  if (yyjson_is_str(reason_value)) {
    reason = yyjson_get_str(reason_value);
  }

  if (strcmp(decision, "block") == 0) {
    *exit_code_out = 1;
    if (reason) {
      fprintf(stderr, "✗ Blocked: %s\n", reason);
    }
  } else if (strcmp(decision, "approve") == 0 ||
             strcmp(decision, "allow") == 0) {
    *exit_code_out = 0;
    if (reason) {
      fprintf(stderr, "✓ Allowed: %s\n", reason);
    }
  }
}

static void handle_modify(yyjson_val *response_root,
                          char **modified_output_ptr) {
  if (response_root == NULL || modified_output_ptr == NULL ||
      !yyjson_is_obj(response_root)) {
    LOG_ERROR("Invalid parameters in handle_modify");
    return;
  }

  yyjson_val *modified_value = yyjson_obj_get(response_root, "modified_data");
  if (modified_value != NULL) {
    size_t json_len = 0;
    char *json_str = yyjson_val_write(modified_value, 0, &json_len);
    if (json_str != NULL) {
      char *secure_json = cchd_secure_malloc(json_len + 1);
      if (secure_json != NULL) {
        memcpy(secure_json, json_str, json_len + 1);
        *modified_output_ptr = secure_json;
      }
      free(json_str);
    }
  }
}

static void handle_hook_specific(yyjson_val *response_root,
                                 int32_t *exit_code_out) {
  if (response_root == NULL || exit_code_out == NULL ||
      !yyjson_is_obj(response_root)) {
    LOG_ERROR("Invalid parameters in handle_hook_specific");
    if (exit_code_out != NULL) {
      *exit_code_out = 1;
    }
    return;
  }

  yyjson_val *hook_specific =
      yyjson_obj_get(response_root, "hookSpecificOutput");
  if (hook_specific && yyjson_is_obj(hook_specific)) {
    yyjson_val *hook_name = yyjson_obj_get(hook_specific, "hookEventName");
    if (yyjson_is_str(hook_name) &&
        strcmp(yyjson_get_str(hook_name), "PreToolUse") == 0) {
      yyjson_val *perm_decision =
          yyjson_obj_get(hook_specific, "permissionDecision");
      if (yyjson_is_str(perm_decision)) {
        const char *perm = yyjson_get_str(perm_decision);
        yyjson_val *perm_reason =
            yyjson_obj_get(hook_specific, "permissionDecisionReason");
        const char *reason =
            yyjson_is_str(perm_reason) ? yyjson_get_str(perm_reason) : NULL;

        if (strcmp(perm, "deny") == 0) {
          *exit_code_out = 1;
          if (reason) {
            fprintf(stderr, "✗ Denied: %s\n", reason);
          }
        } else if (strcmp(perm, "allow") == 0) {
          *exit_code_out = 0;
          if (reason) {
            fprintf(stderr, "✓ Allowed: %s\n", reason);
          }
        } else if (strcmp(perm, "ask") == 0) {
          *exit_code_out = 2;
          if (reason) {
            fprintf(stderr, "⚠ User approval required: %s\n", reason);
          }
        }
      }
    }
  }
}

cchd_error cchd_process_server_response(const char *response_data,
                                        char **modified_output_ptr,
                                        const cchd_config_t *config,
                                        bool *suppress_output_ptr,
                                        int32_t server_http_status,
                                        int32_t *exit_code_out) {
  if (response_data == NULL || modified_output_ptr == NULL || config == NULL ||
      suppress_output_ptr == NULL || exit_code_out == NULL ||
      strlen(response_data) == 0) {
    LOG_ERROR("Invalid parameters in process_server_response");
    *exit_code_out = cchd_config_is_fail_open(config) ? 0 : 1;
    return CCHD_ERROR_INVALID_ARG;
  }

  if (server_http_status >= 400 && server_http_status < 500) {
    LOG_ERROR("Client error from server: HTTP %d", server_http_status);
    *exit_code_out = 1;
    return CCHD_ERROR_HTTP_CLIENT;
  }

  if (server_http_status >= 500) {
    LOG_ERROR("Server error: HTTP %d", server_http_status);
    *exit_code_out = cchd_config_is_fail_open(config) ? 0 : 1;
    return CCHD_ERROR_HTTP_SERVER;
  }

  *modified_output_ptr = NULL;
  *suppress_output_ptr = false;

  yyjson_doc *response_doc =
      yyjson_read(response_data, strlen(response_data), 0);
  if (response_doc == NULL) {
    *exit_code_out = cchd_config_is_fail_open(config) ? 0 : 1;
    return CCHD_ERROR_INVALID_JSON;
  }

  yyjson_val *response_root = yyjson_doc_get_root(response_doc);
  if (response_root == NULL || !yyjson_is_obj(response_root)) {
    yyjson_doc_free(response_doc);
    *exit_code_out = cchd_config_is_fail_open(config) ? 0 : 1;
    return CCHD_ERROR_SERVER_INVALID;
  }

  bool should_continue = true;
  bool suppress_output = false;
  const char *stop_reason = NULL;
  parse_base_response(response_root, &should_continue, &suppress_output,
                      &stop_reason);

  if (!should_continue) {
    if (stop_reason) {
      fprintf(stderr, "Stopped: %s\n", stop_reason);
    }
    yyjson_doc_free(response_doc);
    *exit_code_out = 1;
    return CCHD_SUCCESS;
  }

  if (suppress_output) {
    *suppress_output_ptr = true;
  }

  *exit_code_out = 0;

  const char *decision = parse_decision(response_root);
  if (decision != NULL) {
    if (strcmp(decision, "modify") == 0) {
      handle_modify(response_root, modified_output_ptr);
    } else {
      handle_decision(decision, response_root, exit_code_out);
    }
  }

  handle_hook_specific(response_root, exit_code_out);

  yyjson_doc_free(response_doc);
  return CCHD_SUCCESS;
}