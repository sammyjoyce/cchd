/*
 * Template initialization command implementation.
 * 
 * Provides a streamlined way for users to bootstrap hook servers without manually
 * copying templates or configuring settings. This reduces friction in getting started
 * with Claude Code hooks, especially for users unfamiliar with the project structure.
 * By automatically downloading templates and updating settings.json, we ensure a
 * consistent setup experience across all platforms.
 */

#include "init.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yyjson.h>

#include "../core/error.h"
#include "../utils/colors.h"
#include "../utils/logging.h"
#include "../utils/memory.h"

// GitHub raw content base URL. We use GitHub's raw content API rather than the
// regular API to avoid rate limits and authentication requirements. This ensures
// the init command works for all users without requiring GitHub credentials.
// Using the main branch ensures users always get the latest stable templates.
#define GITHUB_RAW_BASE \
  "https://raw.githubusercontent.com/sammyjoyce/cchd/main/templates/"

// Template file information. We store template metadata in a static array rather
// than fetching it dynamically to avoid network roundtrips for listing templates.
// This makes the 'init --help' command instant and ensures it works offline.
// The filenames are compile-time constants to ensure consistency with the actual
// template files in the repository.
typedef struct {
  const char *name;
  const char *filename;
  const char *description;
} template_info_t;

static const template_info_t templates[] = {
    {"python", CCHD_TEMPLATE_PYTHON,
     "Python server using aiohttp (requires UV)"},
    {"typescript", CCHD_TEMPLATE_TYPESCRIPT,
     "TypeScript server using Bun or Node.js"},
    {"go", CCHD_TEMPLATE_GO, "Go server using net/http"},
};

static const size_t template_count = sizeof(templates) / sizeof(templates[0]);

// Structure to hold downloaded data. We use a growable buffer rather than
// fixed-size allocation because template sizes vary and may grow over time.
// This prevents buffer overflows while avoiding excessive memory allocation
// for smaller templates. The separate size/capacity tracking follows the
// standard vector pattern for efficient reallocation.
typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} download_buffer_t;

static void print_init_usage(const char *program_name) {
  const char *bold = cchd_use_colors(NULL) ? COLOR_BOLD : "";
  const char *reset = cchd_use_colors(NULL) ? COLOR_RESET : "";

  printf("%sUSAGE%s\n", bold, reset);
  printf("  %s init <template> [filename]\n\n", program_name);

  printf("%sDESCRIPTION%s\n", bold, reset);
  printf("  Initialize a new hook server from a template.\n");
  printf("  By default, creates the server in .claude/hooks/ directory.\n");
  printf("  Also updates .claude/settings.json with the hook command.\n\n");

  printf("%sTEMPLATES%s\n", bold, reset);
  for (size_t i = 0; i < template_count; i++) {
    printf("  %-12s  %s\n", templates[i].name, templates[i].description);
  }
  printf("\n");

  printf("%sEXAMPLES%s\n", bold, reset);
  printf("  %s init python                # Creates .claude/hooks/%s\n",
         program_name, CCHD_TEMPLATE_PYTHON);
  printf("  %s init typescript            # Creates .claude/hooks/%s\n",
         program_name, CCHD_TEMPLATE_TYPESCRIPT);
  printf("  %s init go custom.go          # Creates .claude/hooks/custom.go\n",
         program_name);
  printf("  %s init python /tmp/hook.py   # Creates /tmp/hook.py\n",
         program_name);
}

static const template_info_t *find_template(const char *name) {
  for (size_t i = 0; i < template_count; i++) {
    if (strcmp(templates[i].name, name) == 0) {
      return &templates[i];
    }
  }
  return NULL;
}

// CURL write callback for downloading. CURL calls this function multiple times
// as data arrives from the network, so we must handle incremental data reception.
// This callback pattern allows us to process large files without loading them
// entirely into memory at once, though our templates are small enough that this
// is mainly future-proofing.
static size_t write_callback(void *contents, size_t size, size_t nmemb,
                             void *userp) {
  size_t realsize = size * nmemb;
  download_buffer_t *buf = (download_buffer_t *)userp;

  // Grow buffer if needed. We use exponential growth (doubling) to minimize
  // reallocation count, which is O(log n) instead of O(n) for linear growth.
  // Starting at 4KB handles most templates in a single allocation while not
  // wasting memory for very small files.
  if (buf->size + realsize + 1 > buf->capacity) {
    size_t new_capacity = (buf->capacity == 0) ? 4096 : buf->capacity * 2;
    while (new_capacity < buf->size + realsize + 1) {
      new_capacity *= 2;
    }

    char *new_data =
        cchd_secure_realloc(buf->data, buf->capacity, new_capacity);
    if (new_data == NULL) {
      return 0;  // Return 0 to signal error to CURL. This is CURL's convention:
                 // returning less than the requested size indicates failure.
    }
    buf->data = new_data;
    buf->capacity = new_capacity;
  }

  memcpy(&(buf->data[buf->size]), contents, realsize);
  buf->size += realsize;
  buf->data[buf->size] = '\0';

  return realsize;
}

static cchd_error download_template(const char *filename,
                                    download_buffer_t *buffer) {
  CURL *curl;
  CURLcode res;
  char url[512];

  // Build URL
  snprintf(url, sizeof(url), "%s%s", GITHUB_RAW_BASE, filename);

  curl = curl_easy_init();
  if (!curl) {
    LOG_ERROR("Failed to initialize CURL");
    return CCHD_ERROR_NETWORK;
  }

  // Set up CURL options. Each option is chosen for reliability and security:
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // Follow redirects in case GitHub URLs change
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);        // 30s timeout prevents hanging on slow networks
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "cchd/" CCHD_VERSION);  // Identify ourselves for GitHub stats

  // Perform the request
  res = curl_easy_perform(curl);

  if (res != CURLE_OK) {
    LOG_ERROR("Failed to download template: %s", curl_easy_strerror(res));
    curl_easy_cleanup(curl);
    return CCHD_ERROR_NETWORK;
  }

  // Check HTTP status
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);

  if (http_code != 200) {
    LOG_ERROR("Failed to download template: HTTP %ld", http_code);
    return CCHD_ERROR_NETWORK;
  }

  return CCHD_SUCCESS;
}

static cchd_error save_template_file(const char *content,
                                     const char *dest_path) {
  FILE *dest = fopen(dest_path, "w");
  if (dest == NULL) {
    LOG_ERROR("Failed to create file: %s", dest_path);
    return CCHD_ERROR_IO;
  }

  // Write content. We check the return value of fwrite to ensure all data
  // was written successfully. Network issues or disk space problems could
  // cause partial writes, which would result in corrupted template files.
  size_t len = strlen(content);
  if (fwrite(content, 1, len, dest) != len) {
    fclose(dest);
    unlink(dest_path);  // Remove partial file to prevent confusion. A partial
                        // template is worse than no template because it might
                        // appear to work but fail in subtle ways.
    LOG_ERROR("Failed to write to file: %s", dest_path);
    return CCHD_ERROR_IO;
  }

  fclose(dest);

  // Make the file executable (for scripts with shebang). Python and shell scripts
  // need execute permission to run directly. We use 0755 (rwxr-xr-x) to allow
  // the owner full access while giving read/execute to others, following standard
  // Unix conventions for user scripts.
  chmod(dest_path, 0755);

  return CCHD_SUCCESS;
}

static cchd_error ensure_directory_exists(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    if (!S_ISDIR(st.st_mode)) {
      LOG_ERROR("Path exists but is not a directory: %s", path);
      return CCHD_ERROR_IO;
    }
    return CCHD_SUCCESS;
  }

  // Create parent directories first. We recursively ensure all parent directories
  // exist before creating the target directory. This mimics 'mkdir -p' behavior,
  // allowing users to specify deep paths like '.claude/hooks/custom/myhandler.py'
  // without manually creating each level. This is especially useful for project-
  // specific hook organizations.
  char *parent = cchd_secure_strdup(path);
  if (!parent) {
    return CCHD_ERROR_MEMORY;
  }

  char *last_slash = strrchr(parent, '/');
  if (last_slash && last_slash != parent) {
    *last_slash = '\0';
    cchd_error err = ensure_directory_exists(parent);
    cchd_secure_free(parent, strlen(parent) + 1);
    if (err != CCHD_SUCCESS) {
      return err;
    }
  } else {
    cchd_secure_free(parent, strlen(parent) + 1);
  }

  // Create the directory
  if (mkdir(path, 0755) != 0) {
    LOG_ERROR("Failed to create directory %s: %s", path, strerror(errno));
    return CCHD_ERROR_IO;
  }

  return CCHD_SUCCESS;
}

static cchd_error update_settings_json(const char *hook_path) {
  const char *settings_path = ".claude/settings.json";

  // Ensure .claude directory exists
  cchd_error err = ensure_directory_exists(".claude");
  if (err != CCHD_SUCCESS) {
    return err;
  }

  // Read existing settings if present. We preserve existing settings rather than
  // overwriting them because users may have customized other options. This merge
  // approach respects user configuration while adding our hook command. If the
  // file doesn't exist, we create a minimal one with just our setting.
  yyjson_doc *doc = NULL;
  yyjson_mut_doc *mut_doc = NULL;

  FILE *f = fopen(settings_path, "r");
  if (f) {
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = cchd_secure_malloc(size + 1);
    if (!content) {
      fclose(f);
      return CCHD_ERROR_MEMORY;
    }

    if (fread(content, 1, size, f) != (size_t)size) {
      cchd_secure_free(content, size + 1);
      fclose(f);
      return CCHD_ERROR_IO;
    }
    content[size] = '\0';
    fclose(f);

    // Parse existing JSON
    doc = yyjson_read(content, size, 0);
    cchd_secure_free(content, size + 1);

    if (doc) {
      mut_doc = yyjson_doc_mut_copy(doc, NULL);
      yyjson_doc_free(doc);
    }
  }

  // Create new document if needed
  if (!mut_doc) {
    mut_doc = yyjson_mut_doc_new(NULL);
    if (!mut_doc) {
      return CCHD_ERROR_MEMORY;
    }
  }

  yyjson_mut_val *root = yyjson_mut_doc_get_root(mut_doc);
  if (!root) {
    root = yyjson_mut_obj(mut_doc);
    yyjson_mut_doc_set_root(mut_doc, root);
  }

  // Add or update hookCommand
  yyjson_mut_val *hook_cmd = yyjson_mut_str(mut_doc, hook_path);
  yyjson_mut_obj_put(root, yyjson_mut_str(mut_doc, "hookCommand"), hook_cmd);

  // Write the updated settings
  size_t json_len = 0;
  char *json_str = yyjson_mut_write(mut_doc, YYJSON_WRITE_PRETTY, &json_len);
  if (!json_str) {
    yyjson_mut_doc_free(mut_doc);
    return CCHD_ERROR_MEMORY;
  }

  f = fopen(settings_path, "w");
  if (!f) {
    free(json_str);
    yyjson_mut_doc_free(mut_doc);
    LOG_ERROR("Failed to open %s for writing", settings_path);
    return CCHD_ERROR_IO;
  }

  if (fwrite(json_str, 1, json_len, f) != json_len) {
    free(json_str);
    yyjson_mut_doc_free(mut_doc);
    fclose(f);
    return CCHD_ERROR_IO;
  }

  fclose(f);
  free(json_str);
  yyjson_mut_doc_free(mut_doc);

  return CCHD_SUCCESS;
}

cchd_error cchd_handle_init(int argc, char *argv[]) {
  const char *program_name = argv[0];

  // Check if we have the right number of arguments
  if (argc < 3) {
    print_init_usage(program_name);
    return CCHD_ERROR_INVALID_ARG;
  }

  const char *template_name = argv[2];
  const char *output_filename = NULL;

  // Check for help flag
  if (strcmp(template_name, "-h") == 0 ||
      strcmp(template_name, "--help") == 0) {
    print_init_usage(program_name);
    exit(0);
  }

  // Find the template
  const template_info_t *template = find_template(template_name);
  if (template == NULL) {
    fprintf(stderr, "Error: Unknown template '%s'\n\n", template_name);
    fprintf(stderr, "Available templates:\n");
    for (size_t i = 0; i < template_count; i++) {
      fprintf(stderr, "  • %s\n", templates[i].name);
    }
    return CCHD_ERROR_INVALID_ARG;
  }

  // Determine output path
  char *full_path = NULL;
  bool use_default_dir = false;

  if (argc > 3) {
    // User provided a path
    output_filename = argv[3];
    // Check if it's just a filename or includes a path
    if (strchr(output_filename, '/') == NULL) {
      // Just a filename, use default directory
      use_default_dir = true;
    }
  } else {
    // No path provided, use default
    output_filename = template->filename;
    use_default_dir = true;
  }

  if (use_default_dir) {
    // Create .claude/hooks directory
    cchd_error err = ensure_directory_exists(".claude/hooks");
    if (err != CCHD_SUCCESS) {
      fprintf(stderr, "Error: Failed to create .claude/hooks directory\n");
      return err;
    }

    // Build full path
    size_t path_len = strlen(".claude/hooks/") + strlen(output_filename) + 1;
    full_path = cchd_secure_malloc(path_len);
    if (!full_path) {
      return CCHD_ERROR_MEMORY;
    }
    snprintf(full_path, path_len, ".claude/hooks/%s", output_filename);
  } else {
    full_path = cchd_secure_strdup(output_filename);
    if (!full_path) {
      return CCHD_ERROR_MEMORY;
    }
  }

  // Check if file already exists. We refuse to overwrite existing files to prevent
  // accidental data loss. Users might have customized their hooks, and silently
  // replacing them would be destructive. This forces users to be intentional about
  // file replacement by removing the old file first.
  struct stat st;
  if (stat(full_path, &st) == 0) {
    fprintf(stderr, "Error: File '%s' already exists\n", full_path);
    fprintf(stderr, "Use a different filename or remove the existing file\n");
    cchd_secure_free(full_path, strlen(full_path) + 1);
    return CCHD_ERROR_IO;
  }
  // Initialize CURL
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
    fprintf(stderr, "Error: Failed to initialize network library\n");
    return CCHD_ERROR_NETWORK;
  }

  // Download template
  download_buffer_t buffer = {NULL, 0, 0};

  printf("Downloading template from GitHub...\n");
  cchd_error err = download_template(template->filename, &buffer);

  if (err != CCHD_SUCCESS) {
    fprintf(stderr, "Error: Failed to download template\n");
    fprintf(stderr, "Check your internet connection and try again\n");
    if (buffer.data) {
      cchd_secure_free(buffer.data, buffer.capacity);
    }
    curl_global_cleanup();
    cchd_secure_free(full_path, strlen(full_path) + 1);
    return err;
  }

  // Save template to file
  err = save_template_file(buffer.data, full_path);
  cchd_secure_free(buffer.data, buffer.capacity);
  curl_global_cleanup();

  if (err != CCHD_SUCCESS) {
    fprintf(stderr, "Error: Failed to save template file\n");
    cchd_secure_free(full_path, strlen(full_path) + 1);
    return err;
  }

  // Update settings.json with hook command
  char hook_command[512];
  snprintf(hook_command, sizeof(hook_command),
           "cchd --server http://localhost:8080/hook");
  err = update_settings_json(hook_command);
  if (err != CCHD_SUCCESS) {
    fprintf(stderr, "Warning: Failed to update .claude/settings.json\n");
    fprintf(stderr, "You may need to manually configure the hook command\n");
    // Don't fail the whole operation for this. The template file is the primary
    // deliverable; settings.json is a convenience. Users can still manually add
    // the hook command, so we treat this as a warning rather than an error.
  }  // Success message
  const char *green = cchd_use_colors(NULL) ? COLOR_GREEN : "";
  const char *reset = cchd_use_colors(NULL) ? COLOR_RESET : "";

  printf("%s✓ Created %s%s\n", green, full_path, reset);
  printf("%s✓ Updated .claude/settings.json%s\n\n", green, reset);

  // Print next steps based on template
  printf("Next steps:\n");
  if (strcmp(template_name, "python") == 0) {
    printf("  1. Run the server:  uv run %s\n", full_path);
  } else if (strcmp(template_name, "typescript") == 0) {
    printf("  1. Run the server:  bun %s\n", full_path);
  } else if (strcmp(template_name, "go") == 0) {
    printf("  1. Run the server:  go run %s\n", full_path);
  }
  printf("  2. The hook is already configured in .claude/settings.json\n");
  printf("  3. Customize the handler functions for your needs\n");

  cchd_secure_free(full_path, strlen(full_path) + 1);
  return CCHD_SUCCESS;
}