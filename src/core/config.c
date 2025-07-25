/*
 * Configuration management implementation.
 */

#include "config.h"

#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <yyjson.h>

#include "../utils/logging.h"
#include "../utils/memory.h"

#define MAX_SERVERS 10

struct cchd_config {
  char *server_urls[MAX_SERVERS];
  size_t server_count;
  char *api_key;
  int64_t timeout_ms;
  bool fail_open;
  bool quiet;
  bool debug;
  bool json_output;
  bool plain_output;
  bool no_color;
  bool no_input;
  bool insecure;
};

cchd_error cchd_config_create(cchd_config_t **config) {
  CHECK_NULL(config, CCHD_ERROR_INVALID_ARG);

  *config = calloc(1, sizeof(cchd_config_t));
  if (*config == NULL) {
    return CCHD_ERROR_MEMORY;
  }

  // Set defaults
  (*config)->timeout_ms = DEFAULT_TIMEOUT_MS;
  (*config)->server_urls[0] = strdup(DEFAULT_SERVER_URL);
  (*config)->server_count = 1;

  return CCHD_SUCCESS;
}

void cchd_config_destroy(cchd_config_t *config) {
  if (config == NULL) {
    return;
  }

  // Free server URLs
  for (size_t i = 0; i < config->server_count; i++) {
    free(config->server_urls[i]);
  }

  // Free API key if allocated
  if (config->api_key) {
    cchd_secure_free(config->api_key, strlen(config->api_key) + 1);
  }

  free(config);
}

static char *get_config_file_path(void) {
  char *config_path = NULL;

  // Try $CCHD_CONFIG_PATH first
  const char *env_path = getenv("CCHD_CONFIG_PATH");
  if (env_path && access(env_path, R_OK) == 0) {
    return strdup(env_path);
  }

  // Try $HOME/.config/cchd/config.json
  const char *home = getenv("HOME");
  if (!home) {
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
      home = pw->pw_dir;
    }
  }

  if (home) {
    config_path = malloc(PATH_MAX);
    if (config_path) {
      snprintf(config_path, PATH_MAX, "%s/.config/cchd/config.json", home);
      if (access(config_path, R_OK) == 0) {
        return config_path;
      }
      free(config_path);
    }
  }

  // Try /etc/cchd/config.json
  if (access("/etc/cchd/config.json", R_OK) == 0) {
    return strdup("/etc/cchd/config.json");
  }

  return NULL;
}

cchd_error cchd_config_load_file(cchd_config_t *config, const char *path) {
  CHECK_NULL(config, CCHD_ERROR_INVALID_ARG);

  char *config_path = path ? strdup(path) : get_config_file_path();
  if (config_path == NULL) {
    return CCHD_SUCCESS;  // No config file is not an error
  }

  FILE *file = fopen(config_path, "r");
  if (file == NULL) {
    free(config_path);
    return CCHD_ERROR_CONFIG;
  }

  // Read config file
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (file_size <= 0 || file_size > 65536) {
    fclose(file);
    free(config_path);
    return CCHD_ERROR_CONFIG;
  }

  char *config_data = malloc(file_size + 1);
  if (config_data == NULL) {
    fclose(file);
    free(config_path);
    return CCHD_ERROR_MEMORY;
  }

  size_t read_size = fread(config_data, 1, file_size, file);
  fclose(file);
  config_data[read_size] = '\0';

  // Parse JSON config
  yyjson_doc *doc = yyjson_read(config_data, read_size, 0);
  if (doc != NULL) {
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root)) {
      // Load server_urls array
      yyjson_val *servers_array = yyjson_obj_get(root, "server_urls");
      if (yyjson_is_arr(servers_array)) {
        size_t server_count = yyjson_arr_size(servers_array);
        if (server_count > 0 && server_count <= MAX_SERVERS) {
          // Clear existing servers
          for (size_t i = 0; i < config->server_count; i++) {
            free(config->server_urls[i]);
          }
          config->server_count = 0;

          size_t idx, max;
          yyjson_val *server_val;
          yyjson_arr_foreach(servers_array, idx, max, server_val) {
            if (yyjson_is_str(server_val) &&
                config->server_count < MAX_SERVERS) {
              config->server_urls[config->server_count++] =
                  strdup(yyjson_get_str(server_val));
            }
          }
        }
      } else {
        // Try single server_url for backward compatibility
        yyjson_val *server = yyjson_obj_get(root, "server_url");
        if (yyjson_is_str(server)) {
          free(config->server_urls[0]);
          config->server_urls[0] = strdup(yyjson_get_str(server));
          config->server_count = 1;
        }
      }

      // Load other settings
      yyjson_val *timeout = yyjson_obj_get(root, "timeout_ms");
      if (yyjson_is_int(timeout)) {
        config->timeout_ms = yyjson_get_int(timeout);
      }

      yyjson_val *fail_open = yyjson_obj_get(root, "fail_open");
      if (yyjson_is_bool(fail_open)) {
        config->fail_open = yyjson_get_bool(fail_open);
      }

      yyjson_val *debug = yyjson_obj_get(root, "debug");
      if (yyjson_is_bool(debug)) {
        config->debug = yyjson_get_bool(debug);
      }

      yyjson_val *api_key_val = yyjson_obj_get(root, "api_key");
      if (yyjson_is_str(api_key_val)) {
        if (config->api_key) {
          cchd_secure_free(config->api_key, strlen(config->api_key) + 1);
        }
        config->api_key = cchd_secure_strdup(yyjson_get_str(api_key_val));
      }
    }
    yyjson_doc_free(doc);
  }

  free(config_data);
  LOG_INFO("Loaded configuration from %s", config_path);
  free(config_path);

  return CCHD_SUCCESS;
}

cchd_error cchd_config_load_env(cchd_config_t *config) {
  CHECK_NULL(config, CCHD_ERROR_INVALID_ARG);

  const char *env_server = getenv("HOOK_SERVER_URL");
  if (env_server != NULL) {
    free(config->server_urls[0]);
    config->server_urls[0] = strdup(env_server);
    config->server_count = 1;
  }

  const char *env_api_key = getenv("HOOK_API_KEY");
  if (env_api_key) {
    if (config->api_key) {
      cchd_secure_free(config->api_key, strlen(config->api_key) + 1);
    }
    config->api_key = cchd_secure_strdup(env_api_key);
  }

  return CCHD_SUCCESS;
}

cchd_error cchd_config_load_args(cchd_config_t *config, int argc,
                                 char *argv[]) {
  CHECK_NULL(config, CCHD_ERROR_INVALID_ARG);
  CHECK_NULL(argv, CCHD_ERROR_INVALID_ARG);

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
      i++;
      const char *server_arg = argv[i];
      if (strchr(server_arg, ',') != NULL) {
        // Comma-separated list
        char *servers_copy = strdup(server_arg);
        if (servers_copy == NULL) {
          return CCHD_ERROR_MEMORY;
        }

        // Clear existing servers
        for (size_t j = 0; j < config->server_count; j++) {
          free(config->server_urls[j]);
        }
        config->server_count = 0;

        char *token = strtok(servers_copy, ",");
        while (token != NULL && config->server_count < MAX_SERVERS) {
          // Trim whitespace
          while (*token == ' ')
            token++;
          char *end = token + strlen(token) - 1;
          while (end > token && *end == ' ')
            *end-- = '\0';

          config->server_urls[config->server_count++] = strdup(token);
          token = strtok(NULL, ",");
        }
        free(servers_copy);
      } else {
        // Single server
        free(config->server_urls[0]);
        config->server_urls[0] = strdup(server_arg);
        config->server_count = 1;
      }
    } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
      config->timeout_ms = atol(argv[++i]);
      if (config->timeout_ms <= 0) {
        config->timeout_ms = DEFAULT_TIMEOUT_MS;
      }
    } else if (strcmp(argv[i], "--fail-open") == 0) {
      config->fail_open = true;
    } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
      config->quiet = true;
    } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
      config->debug = true;
    } else if (strcmp(argv[i], "--json") == 0) {
      config->json_output = true;
    } else if (strcmp(argv[i], "--plain") == 0) {
      config->plain_output = true;
    } else if (strcmp(argv[i], "--no-color") == 0) {
      config->no_color = true;
    } else if (strcmp(argv[i], "--no-input") == 0) {
      config->no_input = true;
    } else if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc) {
      if (config->api_key) {
        cchd_secure_free(config->api_key, strlen(config->api_key) + 1);
      }
      config->api_key = cchd_secure_strdup(argv[++i]);
    } else if (strcmp(argv[i], "--insecure") == 0) {
      config->insecure = true;
    }
  }

  return CCHD_SUCCESS;
}

// Getters
const char *cchd_config_get_server_url(const cchd_config_t *config,
                                       size_t index) {
  if (config == NULL || index >= config->server_count) {
    return NULL;
  }
  return config->server_urls[index];
}

size_t cchd_config_get_server_count(const cchd_config_t *config) {
  return config ? config->server_count : 0;
}

const char *cchd_config_get_api_key(const cchd_config_t *config) {
  return config ? config->api_key : NULL;
}

int64_t cchd_config_get_timeout_ms(const cchd_config_t *config) {
  return config ? config->timeout_ms : DEFAULT_TIMEOUT_MS;
}

bool cchd_config_is_fail_open(const cchd_config_t *config) {
  return config ? config->fail_open : false;
}

bool cchd_config_is_quiet(const cchd_config_t *config) {
  return config ? config->quiet : false;
}

bool cchd_config_is_debug(const cchd_config_t *config) {
  return config ? config->debug : false;
}

bool cchd_config_is_json_output(const cchd_config_t *config) {
  return config ? config->json_output : false;
}

bool cchd_config_is_plain_output(const cchd_config_t *config) {
  return config ? config->plain_output : false;
}

bool cchd_config_is_no_color(const cchd_config_t *config) {
  return config ? config->no_color : false;
}

bool cchd_config_is_no_input(const cchd_config_t *config) {
  return config ? config->no_input : false;
}

bool cchd_config_is_insecure(const cchd_config_t *config) {
  return config ? config->insecure : false;
}

// Setters
void cchd_config_set_debug(cchd_config_t *config, bool debug) {
  if (config) {
    config->debug = debug;
  }
}

void cchd_config_add_server_url(cchd_config_t *config, const char *url) {
  if (config && url && config->server_count < MAX_SERVERS) {
    config->server_urls[config->server_count++] = strdup(url);
  }
}