/*
 * Claude Code Hooks Dispatcher (cchd) - Main entry point.
 *
 * This dispatcher bridges Claude Code hooks with HTTP servers for custom event
 * handling. It receives hook events from Claude Code on stdin, forwards them
 * to a configured HTTP server, and returns the server's response.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

#include "cli/args.h"
#include "cli/help.h"
#include "core/config.h"
#include "core/error.h"
#include "core/types.h"
#include "io/input.h"
#include "io/output.h"
#include "network/http.h"
#include "protocol/json.h"
#include "protocol/validation.h"
#include "utils/colors.h"
#include "utils/logging.h"
#include "utils/memory.h"

#if __STDC_VERSION__ >= 201112L
static _Thread_local int thread_id = 0;
#endif

static cchd_error initialize_cchd(int argc, char *argv[],
                                  cchd_config_t **config) {
  // Show help if no input and no arguments
  if (isatty(STDIN_FILENO) && argc == 1) {
    cchd_print_concise_help(argv[0]);
    exit(0);
  }

  // Create configuration
  cchd_error err = cchd_config_create(config);
  if (err != CCHD_SUCCESS) {
    return err;
  }

  // Load configuration from various sources
  (void)cchd_config_load_file(*config, NULL);  // Load from default locations
  (void)cchd_config_load_env(*config);

  // Parse command line arguments (may exit for --help or --version)
  err = cchd_parse_args(argc, argv, *config);
  if (err != CCHD_SUCCESS) {
    cchd_config_destroy(*config);
    return err;
  }

  // Validate server URLs
  bool has_valid_server = false;
  for (size_t i = 0; i < cchd_config_get_server_count(*config); i++) {
    const char *url = cchd_config_get_server_url(*config, i);
    if (cchd_validate_server_url(url, *config)) {
      has_valid_server = true;
    }
  }

  if (!has_valid_server) {
    if (!cchd_config_is_quiet(*config) &&
        !cchd_config_is_json_output(*config)) {
      fprintf(stderr, "Error: No valid server URLs configured\n");
      fprintf(stderr, "Use --server to specify a server URL\n");
    }
    cchd_config_destroy(*config);
    return CCHD_ERROR_INVALID_URL;
  }

  // Set up debug logging if requested
  if (cchd_config_is_debug(*config)) {
    cchd_log_set_level(LOG_LEVEL_DEBUG);
    LOG_DEBUG("Debug mode enabled");
    LOG_DEBUG("Configuration:");
    for (size_t i = 0; i < cchd_config_get_server_count(*config); i++) {
      LOG_DEBUG("  server_url[%zu]: %s", i,
                cchd_config_get_server_url(*config, i));
    }
    LOG_DEBUG("  timeout_ms: %lld",
              (long long)cchd_config_get_timeout_ms(*config));
    LOG_DEBUG("  fail_open: %s",
              cchd_config_is_fail_open(*config) ? "true" : "false");
  }

  // Initialize HTTP subsystem
  err = cchd_http_init();
  if (err != CCHD_SUCCESS) {
    cchd_config_destroy(*config);
    return err;
  }

  // Check if stdin is a terminal (no piped input)
  if (isatty(STDIN_FILENO)) {
    cchd_print_concise_help(argv[0]);
    cchd_http_cleanup();
    cchd_config_destroy(*config);
    exit(0);
  }

  return CCHD_SUCCESS;
}

static char *read_and_validate_input(const cchd_config_t *config,
                                     const char *program_name) {
  (void)program_name;  // Unused parameter
  if (cchd_config_is_no_input(config)) {
    if (!cchd_config_is_quiet(config)) {
      fprintf(stderr, "No input mode - exiting\n");
    }
    exit(0);
  }

  char *input = cchd_read_input_from_stdin();
  if (input == NULL) {
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      const char *red = cchd_use_colors(config) ? COLOR_RED : "";
      const char *reset = cchd_use_colors(config) ? COLOR_RESET : "";
      fprintf(stderr, "%sError reading input from stdin%s\n", red, reset);
    }
    exit(CCHD_ERROR_IO);
  }

  if (strlen(input) == 0) {
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      fprintf(stderr, "Error: Empty input\n");
      fprintf(stderr, "Expected JSON hook event on stdin\n");
    }
    cchd_secure_free(input, strlen(input) + 1);
    exit(CCHD_ERROR_INVALID_JSON);
  }

  return input;
}

static char *transform_input_json(const char *input_json_string,
                                  const cchd_config_t *config,
                                  const char *program_name,
                                  size_t input_capacity) {
  (void)program_name;  // Unused parameter
  char *protocol_json =
      cchd_process_input_to_protocol(input_json_string, config);
  if (protocol_json == NULL) {
    // The error has already been reported by cchd_process_input_to_protocol
    // Check if it's a validation error (valid JSON but missing fields)
    // by trying to parse the JSON before freeing it
    yyjson_doc *test_doc =
        yyjson_read(input_json_string, strlen(input_json_string), 0);
    cchd_secure_free((char *)input_json_string, input_capacity);

    if (test_doc != NULL) {
      // JSON is valid but validation failed
      yyjson_doc_free(test_doc);
      exit(CCHD_ERROR_INVALID_HOOK);
    } else {
      // JSON parse error
      exit(CCHD_ERROR_INVALID_JSON);
    }
  }
  return protocol_json;
}

static int32_t process_request_and_response(const cchd_config_t *config,
                                            const char *protocol_json_string,
                                            char **modified_output_json,
                                            bool *suppress_output,
                                            const char *program_name) {
  cchd_response_buffer_t server_response = {
      .data = NULL, .size = 0, .capacity = 0};
  int32_t server_http_status = cchd_send_request_to_server(
      config, protocol_json_string, &server_response, program_name);

  int32_t program_exit_code = 0;

  if (server_http_status == 200 && server_response.data != NULL) {
    cchd_error err = cchd_process_server_response(
        server_response.data, modified_output_json, config, suppress_output,
        server_http_status, &program_exit_code);
    if (err != CCHD_SUCCESS) {
      LOG_ERROR("Failed to process server response: %s", cchd_strerror(err));
    }
  } else if (!cchd_config_is_fail_open(config)) {
    if (!cchd_config_is_quiet(config)) {
      fprintf(stderr, "Error: Server unavailable (fail-closed mode)\n\n");
      fprintf(stderr, "The operation was blocked because the server");
      if (cchd_config_get_server_count(config) > 1) {
        fprintf(stderr, "s are not responding:\n");
        for (size_t i = 0; i < cchd_config_get_server_count(config); i++) {
          fprintf(stderr, "  • %s\n", cchd_config_get_server_url(config, i));
        }
        fprintf(stderr, "\n");
      } else {
        fprintf(stderr, " at\n%s is not responding.\n\n",
                cchd_config_get_server_url(config, 0));
      }
      fprintf(stderr, "To allow operations when server is down:\n");
      fprintf(stderr, "  • Remove the --fail-closed flag\n");
      fprintf(stderr, "  • Or fix the server connection\n");
    }
    // Map negative error codes
    if (server_http_status < 0) {
      program_exit_code = -server_http_status;
    } else {
      program_exit_code = CCHD_ERROR_BLOCKED;
    }
    *suppress_output = true;
  }

  if (server_response.data != NULL) {
    cchd_secure_free(server_response.data, server_response.capacity);
  }

  return program_exit_code;
}

static void cleanup_resources(char *input_json_string,
                              size_t input_json_capacity,
                              char *modified_output_json,
                              cchd_config_t *config) {
  cchd_secure_free(input_json_string, input_json_capacity);
  if (modified_output_json != NULL) {
    cchd_secure_free(modified_output_json, strlen(modified_output_json) + 1);
  }
  cchd_http_cleanup();
  cchd_config_destroy(config);
}

int main(int argc, char *argv[]) {
#if __STDC_VERSION__ >= 201112L
  (void)thread_id;  // Suppress unused variable warning
#endif

  // Start performance timing
  struct timespec start_time, end_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);

  // Initialize logging
  cchd_log_init();

  // Initialize configuration
  cchd_config_t *config = NULL;
  cchd_error err = initialize_cchd(argc, argv, &config);
  if (err != CCHD_SUCCESS) {
    return err;
  }

  // Read and validate input
  char *input_json_string = read_and_validate_input(config, argv[0]);
  size_t input_json_len = strlen(input_json_string);
  size_t input_json_capacity = input_json_len + 1;

  // Transform input to protocol format
  char *protocol_json_string = transform_input_json(
      input_json_string, config, argv[0], input_json_capacity);
  size_t protocol_json_len = strlen(protocol_json_string);

  // Process request and response
  char *modified_output_json = NULL;
  bool suppress_output = false;
  int32_t program_exit_code = process_request_and_response(
      config, protocol_json_string, &modified_output_json, &suppress_output,
      argv[0]);
  cchd_secure_free(protocol_json_string, protocol_json_len + 1);

  // Handle output
  cchd_handle_output(suppress_output, modified_output_json, input_json_string,
                     config, program_exit_code);

  // Cleanup resources
  cleanup_resources(input_json_string, input_json_capacity,
                    modified_output_json, config);

  // Calculate total processing time
  clock_gettime(CLOCK_MONOTONIC, &end_time);
  int64_t elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 +
                       (end_time.tv_nsec - start_time.tv_nsec) / 1000000;
  LOG_INFO("Request processed in %ld ms with exit code %d", (long)elapsed_ms,
           program_exit_code);

  return program_exit_code;
}