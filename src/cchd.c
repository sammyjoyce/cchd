/*
 * Claude Code Hooks Dispatcher (cchd)
 *
 * Bridges Claude Code hooks with HTTP servers for custom event handling.
 */

#include <assert.h>
#include <curl/curl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

#define DEFAULT_SERVER_URL "http://localhost:8080/hook"
#define DEFAULT_TIMEOUT_MS 5000
#define INPUT_BUFFER_INITIAL_SIZE 65536
#define INPUT_BUFFER_READ_CHUNK_SIZE 1024
#define INPUT_MAX_SIZE (1024 * 1024)

typedef struct {
  char *data;
  size_t size;
} ResponseBuffer;

typedef struct {
  const char *server_url;
  long timeout_ms;
  bool fail_open;
} Configuration;

static size_t write_callback(void *contents, size_t size, size_t nmemb,
                             void *userp) {
  ResponseBuffer *response_buffer = (ResponseBuffer *)userp;

  // Prevent overflow in size calculation.
  if (nmemb > SIZE_MAX / size) {
    return 0;
  }
  size_t real_size = size * nmemb;

  // Prevent overflow in new size calculation.
  if (response_buffer->size > SIZE_MAX - real_size - 1) {
    return 0;
  }
  size_t new_size = response_buffer->size + real_size + 1;

  char *new_data = realloc(response_buffer->data, new_size);
  assert(new_data != NULL);

  response_buffer->data = new_data;
  memcpy(&(response_buffer->data[response_buffer->size]), contents, real_size);
  response_buffer->size += real_size;
  response_buffer->data[response_buffer->size] = 0;

  return real_size;
}

static char *read_input_from_stdin(void) {
  // Read all input from stdin into a dynamically growing buffer, but enforce an
  // upper bound on the total size to prevent denial-of-service from unbounded
  // input. We use exponential growth for the buffer to amortize reallocations
  // while keeping a tight limit.
  size_t capacity = INPUT_BUFFER_INITIAL_SIZE;
  char *buffer = malloc(capacity);
  assert(buffer != NULL);

  size_t total_size = 0;
  while (1) {
    size_t remaining_capacity = capacity - total_size;
    if (remaining_capacity < INPUT_BUFFER_READ_CHUNK_SIZE) {
      if (capacity >= INPUT_MAX_SIZE) {
        free(buffer);
        return NULL;
      }
      size_t new_capacity = capacity * 2;
      if (new_capacity > INPUT_MAX_SIZE) {
        new_capacity = INPUT_MAX_SIZE;
      }
      char *new_buffer = realloc(buffer, new_capacity);
      assert(new_buffer != NULL);
      buffer = new_buffer;
      capacity = new_capacity;
      remaining_capacity = capacity - total_size;
    }

    size_t bytes_to_read = remaining_capacity < INPUT_BUFFER_READ_CHUNK_SIZE
                               ? remaining_capacity
                               : INPUT_BUFFER_READ_CHUNK_SIZE;

    size_t bytes_read = fread(buffer + total_size, 1, bytes_to_read, stdin);
    total_size += bytes_read;
    assert(total_size <= capacity);

    if (bytes_read < bytes_to_read) {
      // We have reached EOF or an error. Distinguish between them because
      // errors must be handled explicitly, while EOF is expected.
      if (ferror(stdin)) {
        free(buffer);
        return NULL;
      }
      break;
    }
  }

  buffer[total_size] = '\0';
  return buffer;
}

typedef enum {
  EVENT_UNKNOWN,
  EVENT_PRE_TOOL_USE,
  EVENT_POST_TOOL_USE,
  EVENT_NOTIFICATION,
  EVENT_STOP,
  EVENT_SUBAGENT_STOP,
  EVENT_USER_PROMPT_SUBMIT,
  EVENT_PRE_COMPACT,
} event_type;

static event_type get_event_type(const char *event_name) {
  assert(event_name != NULL);

  if (strcmp(event_name, "PreToolUse") == 0)
    return EVENT_PRE_TOOL_USE;
  if (strcmp(event_name, "PostToolUse") == 0)
    return EVENT_POST_TOOL_USE;
  if (strcmp(event_name, "Notification") == 0)
    return EVENT_NOTIFICATION;
  if (strcmp(event_name, "Stop") == 0)
    return EVENT_STOP;
  if (strcmp(event_name, "SubagentStop") == 0)
    return EVENT_SUBAGENT_STOP;
  if (strcmp(event_name, "UserPromptSubmit") == 0)
    return EVENT_USER_PROMPT_SUBMIT;
  if (strcmp(event_name, "PreCompact") == 0)
    return EVENT_PRE_COMPACT;
  return EVENT_UNKNOWN;
}

static yyjson_mut_val *build_event_object(yyjson_mut_doc *output_doc,
                                          yyjson_val *input_root) {
  // Construct the event metadata object with a unique ID, type, timestamp, and
  // session details. This standardizes the event format for the server
  // protocol, ensuring all required fields are present and providing defaults
  // for missing optional fields to avoid downstream failures.
  assert(output_doc != NULL);
  assert(input_root != NULL);

  yyjson_mut_val *event_object = yyjson_mut_obj(output_doc);
  assert(event_object != NULL);

  // Get event name from input
  yyjson_val *event_name_value = yyjson_obj_get(input_root, "hook_event_name");
  const char *event_name = yyjson_is_str(event_name_value)
                               ? yyjson_get_str(event_name_value)
                               : "Unknown";
  assert(true == yyjson_mut_obj_add_strcpy(output_doc, event_object, "type",
                                           event_name));
  assert(true == yyjson_mut_obj_add_strcpy(output_doc, event_object, "name",
                                           event_name));

  // Add timestamp
  uint64_t timestamp_ms = (uint64_t)time(NULL) * 1000;
  assert(true == yyjson_mut_obj_add_uint(output_doc, event_object, "timestamp",
                                         timestamp_ms));

  // Generate unique ID using timestamp to avoid static ID issues.
  char id_buffer[32];
  int id_length =
      snprintf(id_buffer, sizeof(id_buffer), "evt_%" PRIu64, timestamp_ms);
  assert(id_length > 0 && (size_t)id_length < sizeof(id_buffer));

  // Ensure null termination
  id_buffer[sizeof(id_buffer) - 1] = '\0';

  assert(true ==
         yyjson_mut_obj_add_strcpy(output_doc, event_object, "id", id_buffer));

  // Get session ID from input
  yyjson_val *session_id_value = yyjson_obj_get(input_root, "session_id");
  const char *session_id = yyjson_is_str(session_id_value)
                               ? yyjson_get_str(session_id_value)
                               : "unknown";
  assert(true == yyjson_mut_obj_add_strcpy(output_doc, event_object,
                                           "session_id", session_id));

  return event_object;
}

static bool add_tool_common_fields(yyjson_mut_doc *output_doc,
                                   yyjson_mut_val *data_object,
                                   yyjson_val *input_root) {
  assert(output_doc != NULL);
  assert(data_object != NULL);
  assert(input_root != NULL);

  yyjson_val *tool_name_value = yyjson_obj_get(input_root, "tool_name");
  if (yyjson_is_str(tool_name_value)) {
    if (!yyjson_mut_obj_add_strcpy(output_doc, data_object, "tool_name",
                                   yyjson_get_str(tool_name_value))) {
      return false;
    }
  }

  yyjson_val *tool_input_value = yyjson_obj_get(input_root, "tool_input");
  if (yyjson_is_obj(tool_input_value)) {
    yyjson_mut_val *tool_input_copy =
        yyjson_val_mut_copy(output_doc, tool_input_value);
    assert(tool_input_copy != NULL);
    if (!yyjson_mut_obj_add_val(output_doc, data_object, "tool_input",
                                tool_input_copy)) {
      return false;
    }
  }
  return true;
}

static bool add_post_tool_use_specific_fields(yyjson_mut_doc *output_doc,
                                              yyjson_mut_val *data_object,
                                              yyjson_val *input_root) {
  assert(output_doc != NULL);
  assert(data_object != NULL);
  assert(input_root != NULL);

  yyjson_val *tool_response_value = yyjson_obj_get(input_root, "tool_response");
  if (yyjson_is_obj(tool_response_value)) {
    yyjson_mut_val *tool_response_copy =
        yyjson_val_mut_copy(output_doc, tool_response_value);
    assert(tool_response_copy != NULL);
    if (!yyjson_mut_obj_add_val(output_doc, data_object, "tool_response",
                                tool_response_copy)) {
      return false;
    }
  }
  return true;
}

static bool add_notification_specific_fields(yyjson_mut_doc *output_doc,
                                             yyjson_mut_val *data_object,
                                             yyjson_val *input_root) {
  assert(output_doc != NULL);
  assert(data_object != NULL);
  assert(input_root != NULL);

  yyjson_val *message_value = yyjson_obj_get(input_root, "message");
  if (yyjson_is_str(message_value)) {
    if (!yyjson_mut_obj_add_strcpy(output_doc, data_object, "message",
                                   yyjson_get_str(message_value))) {
      return false;
    }
  }

  yyjson_val *title_value = yyjson_obj_get(input_root, "title");
  if (yyjson_is_str(title_value)) {
    if (!yyjson_mut_obj_add_strcpy(output_doc, data_object, "title",
                                   yyjson_get_str(title_value))) {
      return false;
    }
  }
  return true;
}

static bool add_stop_specific_fields(yyjson_mut_doc *output_doc,
                                     yyjson_mut_val *data_object,
                                     yyjson_val *input_root) {
  assert(output_doc != NULL);
  assert(data_object != NULL);
  assert(input_root != NULL);

  yyjson_val *stop_hook_active_value =
      yyjson_obj_get(input_root, "stop_hook_active");
  if (yyjson_is_bool(stop_hook_active_value)) {
    if (!yyjson_mut_obj_add_bool(output_doc, data_object, "stop_hook_active",
                                 yyjson_get_bool(stop_hook_active_value))) {
      return false;
    }
  }
  return true;
}

static bool add_user_prompt_submit_specific_fields(yyjson_mut_doc *output_doc,
                                                   yyjson_mut_val *data_object,
                                                   yyjson_val *input_root) {
  assert(output_doc != NULL);
  assert(data_object != NULL);
  assert(input_root != NULL);

  yyjson_val *prompt_value = yyjson_obj_get(input_root, "prompt");
  if (yyjson_is_str(prompt_value)) {
    if (!yyjson_mut_obj_add_strcpy(output_doc, data_object, "prompt",
                                   yyjson_get_str(prompt_value))) {
      return false;
    }
  }
  return true;
}

static bool add_pre_compact_specific_fields(yyjson_mut_doc *output_doc,
                                            yyjson_mut_val *data_object,
                                            yyjson_val *input_root) {
  assert(output_doc != NULL);
  assert(data_object != NULL);
  assert(input_root != NULL);

  yyjson_val *trigger_value = yyjson_obj_get(input_root, "trigger");
  if (yyjson_is_str(trigger_value)) {
    if (!yyjson_mut_obj_add_strcpy(output_doc, data_object, "trigger",
                                   yyjson_get_str(trigger_value))) {
      return false;
    }
  }

  yyjson_val *custom_instructions_value =
      yyjson_obj_get(input_root, "custom_instructions");
  if (yyjson_is_str(custom_instructions_value)) {
    if (!yyjson_mut_obj_add_strcpy(output_doc, data_object,
                                   "custom_instructions",
                                   yyjson_get_str(custom_instructions_value))) {
      return false;
    }
  }
  return true;
}

static yyjson_mut_val *build_data_object(yyjson_mut_doc *output_doc,
                                         yyjson_val *input_root) {
  // Extract and copy all relevant data fields based on the hook type.
  // We perform deep copies of objects to ensure the mutable document owns all
  // values, preventing lifetime issues or references to the original immutable
  // input document.
  assert(output_doc != NULL);
  assert(input_root != NULL);

  yyjson_mut_val *data_object = yyjson_mut_obj(output_doc);
  assert(data_object != NULL);

  // Get the hook event name to determine which fields to copy
  yyjson_val *event_name_value = yyjson_obj_get(input_root, "hook_event_name");
  const char *event_name = yyjson_is_str(event_name_value)
                               ? yyjson_get_str(event_name_value)
                               : "Unknown";

  event_type type = get_event_type(event_name);

  // Common fields for tool-based hooks (PreToolUse, PostToolUse)
  if (type == EVENT_PRE_TOOL_USE || type == EVENT_POST_TOOL_USE) {
    if (!add_tool_common_fields(output_doc, data_object, input_root)) {
      return NULL;
    }
  }

  switch (type) {
  case EVENT_POST_TOOL_USE:
    if (!add_post_tool_use_specific_fields(output_doc, data_object,
                                           input_root)) {
      return NULL;
    }
    break;
  case EVENT_NOTIFICATION:
    if (!add_notification_specific_fields(output_doc, data_object,
                                          input_root)) {
      return NULL;
    }
    break;
  case EVENT_STOP:
  case EVENT_SUBAGENT_STOP:
    if (!add_stop_specific_fields(output_doc, data_object, input_root)) {
      return NULL;
    }
    break;
  case EVENT_USER_PROMPT_SUBMIT:
    if (!add_user_prompt_submit_specific_fields(output_doc, data_object,
                                                input_root)) {
      return NULL;
    }
    break;
  case EVENT_PRE_COMPACT:
    if (!add_pre_compact_specific_fields(output_doc, data_object, input_root)) {
      return NULL;
    }
    break;
  case EVENT_UNKNOWN:
  case EVENT_PRE_TOOL_USE:
    // No additional fields.
    break;
  }

  return data_object;
}

static yyjson_mut_doc *
transform_input_to_protocol_format(yyjson_doc *input_doc) {
  // Transform the input JSON to the server protocol format by adding a version,
  // event metadata, and data sections. This ensures compatibility and provides
  // a consistent structure for server-side processing. We build the output as a
  // mutable document to allow efficient construction and serialization.
  assert(input_doc != NULL);

  yyjson_val *input_root = yyjson_doc_get_root(input_doc);
  if (input_root == NULL || !yyjson_is_obj(input_root)) {
    return NULL;
  }
  assert(yyjson_is_obj(input_root));

  yyjson_mut_doc *output_doc = yyjson_mut_doc_new(NULL);
  assert(output_doc != NULL);

  yyjson_mut_val *output_root = yyjson_mut_obj(output_doc);
  assert(output_root != NULL);
  yyjson_mut_doc_set_root(output_doc, output_root);

  assert(true ==
         yyjson_mut_obj_add_strcpy(output_doc, output_root, "version", "1.0"));

  yyjson_mut_val *event_object = build_event_object(output_doc, input_root);
  assert(event_object != NULL);
  assert(true == yyjson_mut_obj_add_val(output_doc, output_root, "event",
                                        event_object));

  yyjson_mut_val *data_object = build_data_object(output_doc, input_root);
  assert(data_object != NULL);
  assert(true ==
         yyjson_mut_obj_add_val(output_doc, output_root, "data", data_object));

  return output_doc;
}

static int send_request_to_server(const Configuration *configuration,
                                  const char *json_payload,
                                  ResponseBuffer *server_response) {
  // Send the JSON payload to the server with retries and exponential backoff to
  // handle transient network issues. We limit retries to prevent indefinite
  // hanging and reset the response buffer on each attempt to avoid data leaks
  // or corruption from failed requests.
  assert(configuration != NULL);
  assert(json_payload != NULL);
  assert(server_response != NULL);

  const int max_attempts = 3;
  int retry_delay_ms = 100;

  for (int attempt = 0; attempt < max_attempts; attempt++) {
    if (attempt > 0) {
      free(server_response->data);
      server_response->data = NULL;
      server_response->size = 0;

      usleep((unsigned int)retry_delay_ms * 1000);
      retry_delay_ms *= 2;
    }

    CURL *curl_handle = curl_easy_init();
    if (curl_handle == NULL) {
      continue;
    }

    struct curl_slist *http_headers =
        curl_slist_append(NULL, "Content-Type: application/json");
    if (http_headers == NULL) {
      curl_easy_cleanup(curl_handle);
      continue;
    }

    curl_easy_setopt(curl_handle, CURLOPT_URL, configuration->server_url);
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, json_payload);
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, http_headers);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, server_response);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS,
                     configuration->timeout_ms);

    CURLcode curl_result = curl_easy_perform(curl_handle);

    long http_status = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_status);

    curl_slist_free_all(http_headers);
    curl_easy_cleanup(curl_handle);

    if (curl_result == CURLE_OK && http_status == 200) {
      return (int)http_status;
    }

    if (attempt < max_attempts - 1) {
      fprintf(stderr, "Request failed (attempt %d/%d), retrying...\n",
              attempt + 1, max_attempts);
    }
  }

  return -1;
}

static bool parse_base_response(yyjson_val *response_root, bool *continue_out,
                                bool *suppress_output_out,
                                const char **stop_reason_out) {
  assert(response_root != NULL);
  assert(continue_out != NULL);
  assert(suppress_output_out != NULL);
  assert(stop_reason_out != NULL);

  // Check for 'continue' field (BaseResponse)
  yyjson_val *continue_value = yyjson_obj_get(response_root, "continue");
  if (yyjson_is_bool(continue_value)) {
    *continue_out = yyjson_get_bool(continue_value);
  } else {
    *continue_out = true; // Default to continue if missing.
  }

  yyjson_val *stop_reason_value = yyjson_obj_get(response_root, "stopReason");
  if (yyjson_is_str(stop_reason_value)) {
    *stop_reason_out = yyjson_get_str(stop_reason_value);
  } else {
    *stop_reason_out = NULL;
  }

  // Check for 'suppressOutput' field (BaseResponse)
  yyjson_val *suppress_output_value =
      yyjson_obj_get(response_root, "suppressOutput");
  if (yyjson_is_bool(suppress_output_value)) {
    *suppress_output_out = yyjson_get_bool(suppress_output_value);
  } else {
    *suppress_output_out = false; // Default to not suppress.
  }

  return true;
}

static const char *parse_decision(yyjson_val *response_root) {
  yyjson_val *decision_value = yyjson_obj_get(response_root, "decision");
  if (yyjson_is_str(decision_value)) {
    return yyjson_get_str(decision_value);
  }

  // If no decision field, check for legacy 'action' field
  yyjson_val *action_value = yyjson_obj_get(response_root, "action");
  if (yyjson_is_str(action_value)) {
    return yyjson_get_str(action_value);
  }

  return NULL;
}

static void handle_decision(const char *decision, yyjson_val *response_root,
                            int *exit_code_out) {
  assert(decision != NULL);
  assert(response_root != NULL);
  assert(exit_code_out != NULL);

  const char *reason = NULL;
  yyjson_val *reason_value = yyjson_obj_get(response_root, "reason");
  if (yyjson_is_str(reason_value)) {
    reason = yyjson_get_str(reason_value);
  }

  if (strcmp(decision, "block") == 0) {
    *exit_code_out = 1;
    if (reason) {
      fprintf(stderr, "Blocked: %s\n", reason);
    }
  } else if (strcmp(decision, "approve") == 0) {
    // Deprecated "approve" for PreToolUse hooks
    *exit_code_out = 0;
    if (reason) {
      // Reason for approval shown to user
      fprintf(stderr, "Approved: %s\n", reason);
    }
  } else if (strcmp(decision, "continue") == 0) {
    // Legacy action mapping
    *exit_code_out = 0;
  }
}

static void handle_modify(yyjson_val *response_root,
                          char **modified_output_ptr) {
  assert(response_root != NULL);
  assert(modified_output_ptr != NULL);

  yyjson_val *modified_value = yyjson_obj_get(response_root, "modified_data");
  if (modified_value != NULL) {
    *modified_output_ptr = yyjson_val_write(modified_value, 0, NULL);
  }
}

static void handle_hook_specific(yyjson_val *response_root,
                                 int *exit_code_out) {
  assert(response_root != NULL);
  assert(exit_code_out != NULL);

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
            fprintf(stderr, "Denied: %s\n", reason);
          }
        } else if (strcmp(perm, "allow") == 0) {
          *exit_code_out = 0;
          if (reason) {
            // Reason shown to user
            fprintf(stderr, "Allowed: %s\n", reason);
          }
        }
        // "ask" would require interactive handling, which we can't do in cchd
      }
    }
  }
}

static int process_server_response(const char *response_data,
                                   char **modified_output_ptr,
                                   const Configuration *configuration,
                                   bool *suppress_output_ptr) {
  // Parse the server response to determine the decision (allow, block, modify)
  // and handle accordingly. In fail-open mode, invalid responses default to
  // allow for liveness over correctness; in fail-closed, they block to
  // prioritize safety.
  assert(response_data != NULL);
  assert(modified_output_ptr != NULL);
  assert(configuration != NULL);
  assert(suppress_output_ptr != NULL);

  *modified_output_ptr = NULL;
  *suppress_output_ptr = false;

  yyjson_doc *response_doc =
      yyjson_read(response_data, strlen(response_data), 0);
  if (response_doc == NULL) {
    return configuration->fail_open ? 0 : 1;
  }

  yyjson_val *response_root = yyjson_doc_get_root(response_doc);
  if (response_root == NULL || !yyjson_is_obj(response_root)) {
    yyjson_doc_free(response_doc);
    return configuration->fail_open ? 0 : 1;
  }
  assert(yyjson_is_obj(response_root));

  bool should_continue = true;
  bool suppress_output = false;
  const char *stop_reason = NULL;
  parse_base_response(response_root, &should_continue, &suppress_output,
                      &stop_reason);

  if (!should_continue) {
    // If continue is false, stop processing
    if (stop_reason) {
      fprintf(stderr, "Stopped: %s\n", stop_reason);
    }
    yyjson_doc_free(response_doc);
    return 1;
  }

  if (suppress_output) {
    // If suppressOutput is true, we should suppress stdout
    *suppress_output_ptr = true;
  }
  int exit_code = 0;

  const char *decision = parse_decision(response_root);
  if (decision != NULL) {
    if (strcmp(decision, "modify") == 0) {
      handle_modify(response_root, modified_output_ptr);
    } else {
      handle_decision(decision, response_root, &exit_code);
    }
  }

  handle_hook_specific(response_root, &exit_code);

  yyjson_doc_free(response_doc);
  return exit_code;
}

static void parse_command_line_arguments(int argc, char *argv[],
                                         Configuration *configuration) {
  // Parse configuration from environment and command-line arguments, with
  // defaults for safety. Environment takes precedence initially, but flags
  // override to allow flexible runtime changes.
  assert(configuration != NULL);

  configuration->server_url = getenv("HOOK_SERVER_URL");
  if (configuration->server_url == NULL) {
    configuration->server_url = DEFAULT_SERVER_URL;
  }
  configuration->timeout_ms = DEFAULT_TIMEOUT_MS;
  configuration->fail_open = true;

  for (int arg_index = 1; arg_index < argc; arg_index++) {
    if (strcmp(argv[arg_index], "--server") == 0 && arg_index + 1 < argc) {
      configuration->server_url = argv[++arg_index];
    } else if (strcmp(argv[arg_index], "--timeout") == 0 &&
               arg_index + 1 < argc) {
      configuration->timeout_ms = atol(argv[++arg_index]);
    } else if (strcmp(argv[arg_index], "--fail-closed") == 0) {
      configuration->fail_open = false;
    }
  }
}

int main(int argc, char *argv[]) {
  Configuration configuration;
  parse_command_line_arguments(argc, argv, &configuration);

  curl_global_init(CURL_GLOBAL_DEFAULT);

  char *input_json_string = read_input_from_stdin();
  if (input_json_string == NULL) {
    fprintf(stderr, "Failed to read input\n");
    curl_global_cleanup();
    return 1;
  }
  yyjson_doc *input_json_document =
      yyjson_read(input_json_string, strlen(input_json_string), 0);
  if (input_json_document == NULL) {
    fprintf(stderr, "Failed to parse input JSON\n");
    free(input_json_string);
    curl_global_cleanup();
    return 1;
  }

  // Extract hook event name for validation
  const char *hook_event_name = NULL;
  yyjson_val *input_root = yyjson_doc_get_root(input_json_document);
  if (input_root && yyjson_is_obj(input_root)) {
    yyjson_val *event_name_val = yyjson_obj_get(input_root, "hook_event_name");
    if (yyjson_is_str(event_name_val)) {
      hook_event_name = yyjson_get_str(event_name_val);
    }
  }
  if (hook_event_name == NULL) {
    hook_event_name = "Unknown"; // Fallback for missing event name
  }

  yyjson_mut_doc *protocol_json_document =
      transform_input_to_protocol_format(input_json_document);
  yyjson_doc_free(input_json_document);
  if (protocol_json_document == NULL) {
    fprintf(stderr, "Failed to transform to protocol format\n");
    free(input_json_string);
    curl_global_cleanup();
    return 1;
  }

  size_t json_len = 0;
  yyjson_write_err err;
  memset(&err, 0, sizeof(err));
  char *protocol_json_string = yyjson_mut_write_opts(
      protocol_json_document, YYJSON_WRITE_NOFLAG, NULL, &json_len, &err);
  yyjson_mut_doc_free(protocol_json_document);
  if (protocol_json_string == NULL) {
    fprintf(stderr, "Failed to serialize JSON: code=%u, msg=%s\n", err.code,
            err.msg ? err.msg : "null");
    free(input_json_string);
    curl_global_cleanup();
    return 1;
  }

  ResponseBuffer server_response = {.data = NULL, .size = 0};
  int server_http_status = send_request_to_server(
      &configuration, protocol_json_string, &server_response);
  free(protocol_json_string);

  int program_exit_code = 0;
  char *modified_output_json = NULL;
  bool suppress_output = false;

  if (server_http_status == 200 && server_response.data != NULL) {
    program_exit_code =
        process_server_response(server_response.data, &modified_output_json,
                                &configuration, &suppress_output);
  } else if (!configuration.fail_open) {
    fprintf(stderr, "Server unavailable, failing closed\n");
    program_exit_code = 1;
  }

  // Only output if not suppressed
  if (!suppress_output) {
    if (modified_output_json != NULL) {
      printf("%s\n", modified_output_json);
      free(modified_output_json);
    } else {
      printf("%s\n", input_json_string);
    }
  } else if (modified_output_json != NULL) {
    // Still need to free the allocated memory even if not printing
    free(modified_output_json);
  }
  free(input_json_string);
  free(server_response.data);
  curl_global_cleanup();

  return program_exit_code;
}
