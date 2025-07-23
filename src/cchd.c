/*
 * Claude Code Hooks Dispatcher (cchd)
 *
 * Bridges Claude Code hooks with HTTP servers for custom event handling.
 */

#include <assert.h>
#include <curl/curl.h>
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
  int fail_open;
} Configuration;

static size_t write_callback(void *contents, size_t size, size_t nmemb,
                             void *userp) {
  size_t real_size = size * nmemb;
  ResponseBuffer *response_buffer = (ResponseBuffer *)userp;

  char *new_data =
      realloc(response_buffer->data, response_buffer->size + real_size + 1);
  if (new_data == NULL) {
    return 0;
  }

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
  if (buffer == NULL) {
    return NULL;
  }

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
      if (new_buffer == NULL) {
        free(buffer);
        return NULL;
      }
      buffer = new_buffer;
      capacity = new_capacity;
      remaining_capacity = capacity - total_size;
    }

    size_t bytes_to_read = remaining_capacity < INPUT_BUFFER_READ_CHUNK_SIZE
                               ? remaining_capacity
                               : INPUT_BUFFER_READ_CHUNK_SIZE;

    size_t bytes_read = fread(buffer + total_size, 1, bytes_to_read, stdin);
    total_size += bytes_read;

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

static yyjson_mut_val *build_event_object(yyjson_mut_doc *output_doc,
                                          yyjson_val *input_root) {
  // Construct the event metadata object with a unique ID, type, timestamp, and
  // session details. This standardizes the event format for the server
  // protocol, ensuring all required fields are present and providing defaults
  // for missing optional fields to avoid downstream failures.
  yyjson_mut_val *event_object = yyjson_mut_obj(output_doc);
  if (event_object == NULL) {
    return NULL;
  }

  // For now, use a static event ID to avoid sprintf issues
  yyjson_mut_obj_add_str(output_doc, event_object, "id", "evt_static");

  // Get event name from input
  yyjson_val *event_name_value = yyjson_obj_get(input_root, "hook_event_name");
  const char *event_name = yyjson_is_str(event_name_value)
                               ? yyjson_get_str(event_name_value)
                               : "Unknown";
  yyjson_mut_obj_add_strcpy(output_doc, event_object, "type", event_name);
  yyjson_mut_obj_add_strcpy(output_doc, event_object, "name", event_name);

  // Add timestamp
  uint64_t timestamp_ms = (uint64_t)time(NULL) * 1000;
  yyjson_mut_obj_add_uint(output_doc, event_object, "timestamp", timestamp_ms);

  // Get session ID from input
  yyjson_val *session_id_value = yyjson_obj_get(input_root, "session_id");
  const char *session_id = yyjson_is_str(session_id_value)
                               ? yyjson_get_str(session_id_value)
                               : "unknown";
  yyjson_mut_obj_add_strcpy(output_doc, event_object, "session_id", session_id);

  return event_object;
}

static yyjson_mut_val *build_data_object(yyjson_mut_doc *output_doc,
                                         yyjson_val *input_root) {
  // Extract and copy tool-related data if present. We perform a deep copy of
  // the tool_input object to ensure the mutable document owns all values,
  // preventing lifetime issues or references to the original immutable input
  // document.
  yyjson_mut_val *data_object = yyjson_mut_obj(output_doc);
  if (data_object == NULL) {
    return NULL;
  }

  yyjson_val *tool_name_value = yyjson_obj_get(input_root, "tool_name");
  if (yyjson_is_str(tool_name_value)) {
    if (!yyjson_mut_obj_add_strcpy(output_doc, data_object, "tool_name",
                                   yyjson_get_str(tool_name_value))) {
      return NULL;
    }
  }

  yyjson_val *tool_input_value = yyjson_obj_get(input_root, "tool_input");
  if (yyjson_is_obj(tool_input_value)) {
    yyjson_mut_val *tool_input_copy =
        yyjson_val_mut_copy(output_doc, tool_input_value);
    if (tool_input_copy == NULL) {
      return NULL;
    }
    if (!yyjson_mut_obj_add_val(output_doc, data_object, "tool_input",
                                tool_input_copy)) {
      return NULL;
    }
  }

  return data_object;
}

static yyjson_mut_doc *
transform_input_to_protocol_format(yyjson_doc *input_doc) {
  // Transform the input JSON to the server protocol format by adding a version,
  // event metadata, and data sections. This ensures compatibility and provides
  // a consistent structure for server-side processing. We build the output as a
  // mutable document to allow efficient construction and serialization.
  yyjson_val *input_root = yyjson_doc_get_root(input_doc);
  if (input_root == NULL || !yyjson_is_obj(input_root)) {
    return NULL;
  }

  yyjson_mut_doc *output_doc = yyjson_mut_doc_new(NULL);
  if (output_doc == NULL) {
    return NULL;
  }

  yyjson_mut_val *output_root = yyjson_mut_obj(output_doc);
  if (output_root == NULL) {
    yyjson_mut_doc_free(output_doc);
    return NULL;
  }
  yyjson_mut_doc_set_root(output_doc, output_root);

  if (!yyjson_mut_obj_add_str(output_doc, output_root, "version", "1.0")) {
    yyjson_mut_doc_free(output_doc);
    return NULL;
  }

  yyjson_mut_val *event_object = build_event_object(output_doc, input_root);
  if (event_object == NULL) {
    yyjson_mut_doc_free(output_doc);
    return NULL;
  }
  if (!yyjson_mut_obj_add_val(output_doc, output_root, "event", event_object)) {
    yyjson_mut_doc_free(output_doc);
    return NULL;
  }

  yyjson_mut_val *data_object = build_data_object(output_doc, input_root);
  if (data_object == NULL) {
    yyjson_mut_doc_free(output_doc);
    return NULL;
  }
  if (!yyjson_mut_obj_add_val(output_doc, output_root, "data", data_object)) {
    yyjson_mut_doc_free(output_doc);
    return NULL;
  }

  return output_doc;
}

static int send_request_to_server(const Configuration *configuration,
                                  const char *json_payload,
                                  ResponseBuffer *server_response) {
  // Send the JSON payload to the server with retries and exponential backoff to
  // handle transient network issues. We limit retries to prevent indefinite
  // hanging and reset the response buffer on each attempt to avoid data leaks
  // or corruption from failed requests.
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

static int process_server_response(const char *response_data,
                                   char **modified_output_ptr,
                                   const Configuration *configuration) {
  // Parse the server response to determine the decision (allow, block, modify)
  // and handle accordingly. In fail-open mode, invalid responses default to
  // allow for liveness over correctness; in fail-closed, they block to
  // prioritize safety.
  *modified_output_ptr = NULL;

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

  yyjson_val *decision_value = yyjson_obj_get(response_root, "decision");
  const char *decision =
      yyjson_is_str(decision_value) ? yyjson_get_str(decision_value) : "allow";

  int exit_code = 0;

  if (strcmp(decision, "block") == 0) {
    exit_code = 1;

    yyjson_val *reason_value = yyjson_obj_get(response_root, "reason");
    if (yyjson_is_str(reason_value)) {
      fprintf(stderr, "Blocked: %s\n", yyjson_get_str(reason_value));
    }
  } else if (strcmp(decision, "modify") == 0) {
    yyjson_val *modified_value = yyjson_obj_get(response_root, "modified_data");
    if (modified_value != NULL) {
      *modified_output_ptr = yyjson_val_write(modified_value, 0, NULL);
    }
  }

  yyjson_doc_free(response_doc);
  return exit_code;
}

static void parse_command_line_arguments(int argc, char *argv[],
                                         Configuration *configuration) {
  // Parse configuration from environment and command-line arguments, with
  // defaults for safety. Environment takes precedence initially, but flags
  // override to allow flexible runtime changes.
  configuration->server_url = getenv("HOOK_SERVER_URL");
  if (configuration->server_url == NULL) {
    configuration->server_url = DEFAULT_SERVER_URL;
  }
  configuration->timeout_ms = DEFAULT_TIMEOUT_MS;
  configuration->fail_open = 1;

  for (int index = 1; index < argc; index++) {
    if (strcmp(argv[index], "--server") == 0 && index + 1 < argc) {
      configuration->server_url = argv[++index];
    } else if (strcmp(argv[index], "--timeout") == 0 && index + 1 < argc) {
      configuration->timeout_ms = atol(argv[++index]);
    } else if (strcmp(argv[index], "--fail-closed") == 0) {
      configuration->fail_open = 0;
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

  if (server_http_status == 200 && server_response.data != NULL) {
    program_exit_code = process_server_response(
        server_response.data, &modified_output_json, &configuration);
  } else if (configuration.fail_open == 0) {
    fprintf(stderr, "Server unavailable, failing closed\n");
    program_exit_code = 1;
  }

  if (modified_output_json != NULL) {
    printf("%s\n", modified_output_json);
    free(modified_output_json);
  } else {
    printf("%s\n", input_json_string);
  }

  free(input_json_string);
  free(server_response.data);
  curl_global_cleanup();

  return program_exit_code;
}
