/*
 * HTTP communication implementation.
 */

#include "http.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../core/config.h"
#include "../utils/colors.h"
#include "../utils/logging.h"
#include "../utils/memory.h"
#include "retry.h"

// Global curl handle for connection reuse
static CURL *g_curl_handle = nullptr;
static pthread_mutex_t g_curl_mutex = PTHREAD_MUTEX_INITIALIZER;

static size_t write_callback(void *contents, size_t size, size_t nmemb,
                             void *userp) {
  if (contents == NULL || userp == NULL || size == 0 || nmemb == 0) {
    LOG_ERROR("Invalid parameters in write_callback");
    return 0;
  }

  cchd_response_buffer_t *response_buffer = (cchd_response_buffer_t *)userp;

  // Prevent overflow
  if (nmemb > SIZE_MAX / size) {
    return 0;
  }
  size_t real_size = size * nmemb;

  if (response_buffer->size > SIZE_MAX - real_size - 1) {
    return 0;
  }
  size_t required_size = response_buffer->size + real_size + 1;

  // Only reallocate if needed
  if (required_size > response_buffer->capacity) {
    size_t new_capacity = response_buffer->capacity * 2;
    if (new_capacity < required_size) {
      new_capacity = required_size;
    }

    char *new_data = cchd_secure_realloc(
        response_buffer->data, response_buffer->capacity, new_capacity);
    if (new_data == NULL) {
      LOG_ERROR("Failed to allocate memory for response buffer (size: %zu)",
                new_capacity);
      return 0;
    }

    response_buffer->data = new_data;
    response_buffer->capacity = new_capacity;
  }

  memcpy(&(response_buffer->data[response_buffer->size]), contents, real_size);
  response_buffer->size += real_size;
  response_buffer->data[response_buffer->size] = 0;

  return real_size;
}

static CURL *get_global_curl_handle(void) {
  pthread_mutex_lock(&g_curl_mutex);
  if (g_curl_handle == NULL) {
    g_curl_handle = curl_easy_init();
    if (g_curl_handle != NULL) {
      curl_easy_setopt(g_curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);
      curl_easy_setopt(g_curl_handle, CURLOPT_TCP_KEEPIDLE, 120L);
      curl_easy_setopt(g_curl_handle, CURLOPT_TCP_KEEPINTVL, 60L);
    }
  }
  pthread_mutex_unlock(&g_curl_mutex);
  return g_curl_handle;
}

static int32_t perform_single_request_with_handle(
    CURL *curl_handle, const cchd_config_t *config, const char *json_payload,
    cchd_response_buffer_t *server_response, const char *program_name,
    const char *server_url) {
  if (curl_handle == nullptr || config == nullptr || json_payload == nullptr ||
      server_response == nullptr || server_url == nullptr ||
      cchd_config_get_timeout_ms(config) <= 0) {
    LOG_ERROR("Invalid parameters in perform_single_request_with_handle");
    return -1;
  }

  char curl_error_buffer[CURL_ERROR_SIZE] = {0};
  struct curl_slist *http_headers = nullptr;
  struct curl_slist *temp_headers = nullptr;

  http_headers = curl_slist_append(nullptr, "Content-Type: application/json");
  CHECK_NULL(http_headers, -1);

  char ua_buffer[64];
  snprintf(ua_buffer, sizeof(ua_buffer), "User-Agent: cchd/%s", CCHD_VERSION);
  temp_headers = curl_slist_append(http_headers, ua_buffer);
  if (!temp_headers) {
    LOG_ERROR("curl_slist_append failed for User-Agent");
    curl_slist_free_all(http_headers);
    return -1;
  }
  http_headers = temp_headers;

  const char *api_key = cchd_config_get_api_key(config);
  if (api_key && strlen(api_key) > 0) {
    char auth_buffer[1024];
    int auth_len = snprintf(auth_buffer, sizeof(auth_buffer),
                            "Authorization: Bearer %s", api_key);
    if (auth_len < 0 || (size_t)auth_len >= sizeof(auth_buffer)) {
      LOG_ERROR("Authorization header too long");
      curl_slist_free_all(http_headers);
      return -1;
    }
    temp_headers = curl_slist_append(http_headers, auth_buffer);
    if (!temp_headers) {
      LOG_ERROR("curl_slist_append failed for Authorization");
      curl_slist_free_all(http_headers);
      return -1;
    }
    http_headers = temp_headers;
  }

  curl_easy_setopt(curl_handle, CURLOPT_URL, server_url);
  curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, json_payload);
  curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, http_headers);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, server_response);
  curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS,
                   cchd_config_get_timeout_ms(config));

  if (cchd_config_is_insecure(config)) {
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
  curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl_handle, CURLOPT_ERRORBUFFER, curl_error_buffer);

  LOG_DEBUG("Sending request to %s (timeout: %ldms)", server_url,
            (long)cchd_config_get_timeout_ms(config));

  CURLcode curl_result = curl_easy_perform(curl_handle);

  int64_t http_status = 0;
  long curl_http_status = 0;
  curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &curl_http_status);
  http_status = (int64_t)curl_http_status;

  curl_slist_free_all(http_headers);

  if (curl_result != CURLE_OK) {
    LOG_ERROR("HTTP request failed: %s (code: %d)",
              curl_error_buffer[0] ? curl_error_buffer
                                   : curl_easy_strerror(curl_result),
              curl_result);

    // Map CURL errors to CCHD error codes
    int32_t error_code = CCHD_ERROR_NETWORK;
    switch (curl_result) {
    case CURLE_COULDNT_CONNECT:
      error_code = CCHD_ERROR_CONNECTION;
      break;
    case CURLE_OPERATION_TIMEDOUT:
      error_code = CCHD_ERROR_TIMEOUT;
      break;
    case CURLE_URL_MALFORMAT:
      error_code = CCHD_ERROR_INVALID_URL;
      break;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT:
      error_code = CCHD_ERROR_TLS;
      break;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
      error_code = CCHD_ERROR_DNS;
      break;
    case CURLE_AUTH_ERROR:
    case CURLE_LOGIN_DENIED:
      error_code = CCHD_ERROR_AUTH;
      break;
    case CURLE_OUT_OF_MEMORY:
      error_code = CCHD_ERROR_MEMORY;
      break;
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
      error_code = CCHD_ERROR_IO;
      break;
    default:
      error_code = CCHD_ERROR_NETWORK;
      break;
    }

    // Provide actionable suggestions
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      const char *red = cchd_use_colors(config) ? COLOR_RED : "";
      const char *yellow = cchd_use_colors(config) ? COLOR_YELLOW : "";
      const char *reset = cchd_use_colors(config) ? COLOR_RESET : "";

      if (curl_result == CURLE_COULDNT_CONNECT) {
        fprintf(stderr, "\n%sCould not connect to %s%s\n\n", red, server_url,
                reset);
        fprintf(stderr, "Check that:\n");
        fprintf(stderr, "  • The server is running\n");
        fprintf(stderr, "  • The URL is correct\n");
        fprintf(stderr, "  • No firewall is blocking the connection\n\n");
        fprintf(stderr, "You might need to:\n");
        fprintf(stderr, "  %s%s --server http://different-server.com%s\n",
                yellow, program_name ? program_name : "cchd", reset);
      } else if (curl_result == CURLE_OPERATION_TIMEDOUT) {
        fprintf(stderr, "\n%sRequest timed out after %ldms%s\n\n", red,
                (long)cchd_config_get_timeout_ms(config), reset);
        fprintf(stderr, "Try:\n");
        fprintf(stderr, "  • Increasing timeout: %s%s --timeout 10000%s\n",
                yellow, program_name ? program_name : "cchd", reset);
        fprintf(stderr, "  • Checking your network connection\n");
        fprintf(stderr, "  • Verifying the server is responding\n");
      } else if (curl_result == CURLE_URL_MALFORMAT) {
        fprintf(stderr, "\n%sInvalid URL format: %s%s\n\n", red, server_url,
                reset);
        fprintf(stderr, "URLs should be like:\n");
        fprintf(stderr, "  • http://localhost:8080/hook\n");
        fprintf(stderr, "  • https://example.com/webhook\n\n");
        fprintf(stderr, "Example:\n");
        fprintf(stderr, "  %s%s --server https://api.example.com/hook%s\n",
                yellow, program_name ? program_name : "cchd", reset);
      }
    }
    return -error_code;
  }

  LOG_DEBUG("HTTP request completed with status %ld", (long)http_status);
  return (int32_t)http_status;
}

cchd_error cchd_http_init(void) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  get_global_curl_handle();
  return CCHD_SUCCESS;
}

void cchd_http_cleanup(void) {
  pthread_mutex_lock(&g_curl_mutex);
  if (g_curl_handle != NULL) {
    curl_easy_cleanup(g_curl_handle);
    g_curl_handle = NULL;
  }
  pthread_mutex_unlock(&g_curl_mutex);
  curl_global_cleanup();
}

int32_t cchd_send_request_to_server(const cchd_config_t *config,
                                    const char *json_payload,
                                    cchd_response_buffer_t *server_response,
                                    const char *program_name) {
  if (config == NULL || json_payload == NULL || server_response == NULL ||
      cchd_config_get_server_count(config) == 0) {
    return -1;
  }

  // Adaptive retry configuration
  const int32_t max_network_retries = 3;
  const int32_t max_server_error_retries = 2;
  const int32_t no_retry_for_client_errors = 1;

  CURL *reusable_curl_handle = get_global_curl_handle();
  if (reusable_curl_handle == NULL) {
    return -1;
  }

  pthread_mutex_lock(&g_curl_mutex);

  // Pre-allocate response buffer
  if (server_response->data == NULL) {
    server_response->data = cchd_secure_malloc(RESPONSE_BUFFER_INITIAL_SIZE);
    if (server_response->data != NULL) {
      server_response->capacity = RESPONSE_BUFFER_INITIAL_SIZE;
    }
  }

  // Try each server in the list
  for (size_t server_idx = 0; server_idx < cchd_config_get_server_count(config);
       server_idx++) {
    const char *current_server_url =
        cchd_config_get_server_url(config, server_idx);
    if (current_server_url == NULL || strlen(current_server_url) == 0) {
      continue;
    }

    // Show progress message
    if (!cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      if (server_idx > 0) {
        fprintf(stderr, "Trying fallback server %s...\n", current_server_url);
      } else {
        fprintf(stderr, "Connecting to %s...\n", current_server_url);
      }
      fflush(stderr);
    }

    int32_t last_http_status = -1;
    int32_t max_attempts = max_network_retries;

    // Try current server with adaptive retries
    for (int32_t attempt = 0; attempt < max_attempts; attempt++) {
      if (attempt > 0 || server_idx > 0) {
        // Reset response buffer
        server_response->size = 0;
        if (server_response->data != NULL) {
          server_response->data[0] = '\0';
        }

        // Reset curl handle
        curl_easy_reset(reusable_curl_handle);

        if (attempt > 0) {
          // Calculate adaptive delay
          int32_t retry_delay_ms = cchd_calculate_retry_delay(
              last_http_status, INITIAL_RETRY_DELAY_MS, attempt - 1);
          LOG_DEBUG("Waiting %dms before retry (error was %d)", retry_delay_ms,
                    last_http_status);
          usleep((uint32_t)retry_delay_ms * 1000);
        }
      }

      int32_t http_status = perform_single_request_with_handle(
          reusable_curl_handle, config, json_payload, server_response,
          program_name, current_server_url);

      last_http_status = http_status;

      if (http_status == 200) {
        if (!cchd_config_is_quiet(config) &&
            !cchd_config_is_json_output(config) && server_idx > 0) {
          fprintf(stderr, "Successfully connected to fallback server\n");
        }
        pthread_mutex_unlock(&g_curl_mutex);
        return http_status;
      }

      // Determine retry strategy
      bool should_retry = false;

      if (http_status < 0) {
        // Negative values are CCHD error codes
        int32_t error_code = -http_status;
        switch (error_code) {
        case CCHD_ERROR_CONNECTION:
        case CCHD_ERROR_TIMEOUT:
        case CCHD_ERROR_NETWORK:
        case CCHD_ERROR_DNS:
          should_retry = true;
          max_attempts = max_network_retries;
          break;
        case CCHD_ERROR_INVALID_URL:
        case CCHD_ERROR_TLS:
          should_retry = false;
          break;
        default:
          should_retry = true;
          max_attempts = max_network_retries;
          break;
        }
      } else if (http_status >= 500 && http_status < 600) {
        should_retry = true;
        max_attempts = max_server_error_retries;
      } else if (http_status == 429) {
        should_retry = true;
        max_attempts = max_server_error_retries;
      } else if (http_status >= 400 && http_status < 500) {
        should_retry = false;
        max_attempts = no_retry_for_client_errors;
      }

      if (!should_retry) {
        if (!cchd_config_is_quiet(config) &&
            !cchd_config_is_json_output(config)) {
          if (http_status >= 400 && http_status < 500) {
            fprintf(stderr, "Client error (HTTP %d) - not retrying\n",
                    http_status);
          }
        }
        break;
      }

      if (attempt < max_attempts - 1 && !cchd_config_is_quiet(config) &&
          !cchd_config_is_json_output(config)) {
        fprintf(stderr,
                "Request failed (HTTP %d, attempt %d/%d), retrying...\n",
                http_status, attempt + 1, max_attempts);
        fflush(stderr);
      }
    }

    // If we have more servers to try
    if (server_idx < cchd_config_get_server_count(config) - 1 &&
        !cchd_config_is_quiet(config) && !cchd_config_is_json_output(config)) {
      fprintf(stderr, "Server %s unavailable, trying next server...\n",
              current_server_url);
    }
  }

  pthread_mutex_unlock(&g_curl_mutex);
  return -CCHD_ERROR_ALL_SERVERS_FAILED;
}