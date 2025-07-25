/*
 * Claude Code Hooks Dispatcher (cchd).
 *
 * This dispatcher bridges Claude Code hooks with HTTP servers for custom event
 * handling. It receives hook events from Claude Code on stdin, forwards them
 * to a configured HTTP server, and returns the server's response. This design
 * allows developers to implement custom policies and validations for Claude
 * Code operations without modifying the core application.
 *
 * Code style: Google C++ Style Guide adapted for C.
 * Format with: clang-format -style='{BasedOnStyle: Google, IndentWidth: 2}'.
 */

#include <assert.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

#include "cli/init.h"

// Version is defined via compiler flag to enable build-time versioning without
// source code modification.

// C23 compatibility: These macros provide forward compatibility with C23
// features while maintaining backward compatibility with C11/C17 compilers.
// This allows us to write cleaner, more modern C code that gracefully
// degrades on older compilers.
#if __STDC_VERSION__ < 202311L
#define nullptr ((void *)0)
#define typeof_unqual(x) typeof(x)
#define _BitInt(N) __int##N##_t
#else
// Use C23 auto type inference where available to reduce type repetition
// and make code more maintainable.
#define auto __auto_type
#endif

// C23 attribute for functions whose return value should not be ignored.
// This helps prevent bugs where error codes are silently discarded.
#if __STDC_VERSION__ >= 202311L
#define CCHD_NODISCARD [[nodiscard("Return value should be checked")]]
#else
#define CCHD_NODISCARD
#endif

/*
 * Constants and configuration_t: This section defines compile-time constants
 * and the main configuration structure. Constants are grouped by purpose and
 * validated at compile-time where possible to catch configuration errors early.
 */

// ANSI color codes: These provide visual feedback in terminal output to help
// users quickly identify errors, warnings, and important information. Colors
// are automatically disabled when output is piped or NO_COLOR is set.
#define COLOR_RED "\033[0;31m"
#define COLOR_GREEN "\033[0;32m"
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_BLUE "\033[0;34m"
#define COLOR_BOLD "\033[1m"
#define COLOR_RESET "\033[0m"

// Compile-time assertions for safety: These validate our assumptions about
// the target platform at compile time, preventing subtle runtime bugs when
// building on systems with unexpected characteristics.
static_assert(PATH_MAX >= 1024, "PATH_MAX too small for config paths");
static_assert(sizeof(size_t) >= 4, "size_t must be at least 32-bit");
static_assert(sizeof(int64_t) == 8, "int64_t must be exactly 64-bit");

// Forward declaration: Required because configuration_t is referenced in
// function signatures before its full definition.
typedef struct configuration_t configuration_t;

/*
 * ============================================================================
 * Data Structures: Core data types used throughout the application. These are
 * designed to be lightweight and cache-friendly while providing clear APIs.
 * ============================================================================
 */

typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} response_buffer_t;

struct configuration_t {
  const char **server_urls;  // Array of server URLs for fallback support.
  size_t server_count;       // Number of servers in the array.
  const char *api_key;
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

// Check if colors should be used: This function implements a hierarchy of
// color preferences, respecting both environment variables and command-line
// flags to ensure proper behavior in different terminal environments.
static bool use_colors(const configuration_t *config) {
  // Command line flag takes precedence: Users can explicitly disable colors
  // via --no-color, overriding all other settings.
  if (config && config->no_color) {
    return false;
  }

  // Respect NO_COLOR environment variable: This is a standard way to disable
  // colors across multiple tools (see https://no-color.org).
  if (getenv("NO_COLOR") != NULL) {
    return false;
  }

  // Force color if requested: FORCE_COLOR overrides terminal detection,
  // useful for CI systems that support colors but don't report as TTY.
  if (getenv("FORCE_COLOR") != NULL) {
    return true;
  }

  // Check if TERM is dumb: Dumb terminals don't support ANSI escape codes,
  // so we disable colors to prevent garbled output.
  const char *term = getenv("TERM");
  if (term != NULL && strcmp(term, "dumb") == 0) {
    return false;
  }

  // Check if output is to a terminal: Only use colors when writing to an
  // interactive terminal, not when output is piped or redirected.
  return isatty(STDERR_FILENO);
}

// IMPORTANT: For production use, HTTPS is strongly recommended to protect data
// in transit. HTTP is used by default for local development convenience because
// setting up TLS certificates for localhost adds friction during development.
// Always use HTTPS when connecting to remote servers.
#define DEFAULT_SERVER_URL "http://localhost:8080/hook"
#define DEFAULT_TIMEOUT_MS 5000
#define INPUT_BUFFER_INITIAL_SIZE (128 * 1024)  // 128KB initial allocation.
#define INPUT_BUFFER_READ_CHUNK_SIZE 8192       // 8KB read chunks.
#define INPUT_MAX_SIZE (512 * 1024)             // 512KB max input size.
#define RESPONSE_BUFFER_INITIAL_SIZE \
  (64 * 1024)  // 64KB initial response buffer.
#define TIMESTAMP_BUFFER_SIZE 32
#define ID_BUFFER_SIZE 64
#define INITIAL_RETRY_DELAY_MS 500
#define TYPE_BUFFER_SIZE 256

// Compile-time validation of buffer sizes: These assertions ensure our buffer
// sizes are reasonable and prevent accidental misconfiguration that could lead
// to buffer overflows or insufficient space for expected data.
static_assert(TIMESTAMP_BUFFER_SIZE >= 25,
              "Timestamp buffer too small for RFC3339");
static_assert(ID_BUFFER_SIZE >= 32, "ID buffer too small for hex encoding");
static_assert(TYPE_BUFFER_SIZE >= 128, "Type buffer too small for CloudEvents");
static_assert(INPUT_BUFFER_INITIAL_SIZE >= 8192,
              "Initial input buffer too small");
static_assert(RESPONSE_BUFFER_INITIAL_SIZE >= 4096,
              "Initial response buffer too small");

// Error code definitions for cchd-specific errors: These map directly to
// process exit codes, allowing Claude Code to understand the result of hook
// execution. Exit codes 0-2 have special meaning - they control whether
// Claude Code proceeds with, blocks, or requests user approval for operations.
enum cchd_error {
  // Success and hook-specific codes (0-2): These codes are interpreted by
  // Claude Code to determine whether to proceed with the requested operation.
  CCHD_SUCCESS = 0,         // Operation allowed.
  CCHD_ERROR_BLOCKED = 1,   // Operation blocked by server.
  CCHD_ERROR_ASK_USER = 2,  // User approval required.

  // Input/configuration errors (3-9): These indicate problems with the
  // input data or configuration that prevent the hook from executing.
  CCHD_ERROR_INVALID_ARG = 3,     // Invalid command line arguments.
  CCHD_ERROR_INVALID_URL = 4,     // Malformed server URL.
  CCHD_ERROR_INVALID_JSON = 5,    // JSON parse error.
  CCHD_ERROR_INVALID_HOOK = 6,    // Invalid hook event or missing fields.
  CCHD_ERROR_CONFIG = 7,          // Configuration file error.
  CCHD_ERROR_CONFIG_PARSE = 8,    // Configuration file JSON parse error.
  CCHD_ERROR_CONFIG_INVALID = 9,  // Configuration file has invalid values.

  // Network/communication errors (10-19): These indicate failures in
  // communicating with the hook server, which may be transient.
  CCHD_ERROR_NETWORK = 10,      // General network error.
  CCHD_ERROR_CONNECTION = 11,   // Connection refused/failed.
  CCHD_ERROR_TIMEOUT = 12,      // Request timeout.
  CCHD_ERROR_TLS = 13,          // TLS/SSL error.
  CCHD_ERROR_DNS = 14,          // DNS resolution failure.
  CCHD_ERROR_HTTP_CLIENT = 15,  // HTTP 4xx client error.
  CCHD_ERROR_HTTP_SERVER = 16,  // HTTP 5xx server error.
  CCHD_ERROR_RATE_LIMIT = 17,   // HTTP 429 rate limiting.
  CCHD_ERROR_AUTH = 18,         // Authentication/authorization error.
  CCHD_ERROR_PROXY = 19,        // Proxy connection error.

  // System errors (20-29): These indicate problems with system resources
  // or internal failures that prevent normal operation.
  CCHD_ERROR_MEMORY = 20,      // Memory allocation error.
  CCHD_ERROR_IO = 21,          // I/O error (stdin/stdout).
  CCHD_ERROR_PERMISSION = 22,  // Permission denied.
  CCHD_ERROR_INTERNAL = 23,    // Internal error/assertion.
  CCHD_ERROR_THREADING = 24,   // Thread/mutex error.
  CCHD_ERROR_RESOURCE = 25,    // Resource exhaustion.
  CCHD_ERROR_SIGNAL = 26,      // Signal handling error.

  // Server response errors (30-39): These indicate problems with the hook
  // server's response that prevent safe operation.
  CCHD_ERROR_SERVER_INVALID = 30,      // Invalid server response format.
  CCHD_ERROR_SERVER_MODIFY = 31,       // Server returned modified data.
  CCHD_ERROR_ALL_SERVERS_FAILED = 32,  // All fallback servers failed.
  CCHD_ERROR_PROTOCOL = 33,            // Protocol violation.
  CCHD_ERROR_JSON_MISSING_FIELD = 34,  // Required JSON field missing.
  CCHD_ERROR_JSON_TYPE_MISMATCH = 35,  // JSON field has wrong type.
  CCHD_ERROR_SERVER_BUSY = 36,         // Server temporarily unavailable.
  CCHD_ERROR_UNSUPPORTED = 37,         // Unsupported operation/feature.
};

// Logging levels for controlling output verbosity: These allow runtime control
// of log detail without recompilation, useful for debugging production issues.
#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARNING 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_DEBUG 3

static int g_log_level = -1;

// Global curl handle for connection reuse: Maintaining a persistent handle
// improves performance by reusing TCP connections and DNS cache across
// requests.
static CURL *g_curl_handle = nullptr;
static pthread_mutex_t g_curl_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * ============================================================================
 * Error Handling and Logging: Centralized error handling and logging functions
 * that provide consistent output formatting and error reporting throughout the
 * application. These functions support runtime log level control for debugging.
 * ============================================================================
 */

// Update log level from environment variable: This reads CCHD_LOG_LEVEL to
// allow users to control verbosity without modifying code or command line args.
static void update_log_level(void) {
  const char *level = getenv("CCHD_LOG_LEVEL");
  if (level == nullptr)
    g_log_level = LOG_LEVEL_ERROR;
  else if (strcmp(level, "WARNING") == 0)
    g_log_level = LOG_LEVEL_WARNING;
  else if (strcmp(level, "INFO") == 0)
    g_log_level = LOG_LEVEL_INFO;
  else if (strcmp(level, "DEBUG") == 0)
    g_log_level = LOG_LEVEL_DEBUG;
  else
    g_log_level = LOG_LEVEL_ERROR;
}

// Get current log level from environment or default to ERROR: This lazy
// initialization ensures we read the environment variable only once, improving
// performance while still allowing runtime control of logging verbosity.
static inline int get_log_level(void) {
  if (g_log_level == -1) {
    update_log_level();
  }
  return g_log_level;
}

static_assert(INPUT_MAX_SIZE >= 512 * 1024, "Input max size too small");

// Internal logging function with timestamp: This provides consistent formatting
// for all log messages, including millisecond precision timestamps and source
// location information to aid in debugging.
static inline void log_with_time(const char *level, const char *file, int line,
                                 const char *fmt, va_list args) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  char time_buf[32];
  struct tm tm_info_buf;
  localtime_r(&tv.tv_sec, &tm_info_buf);
  strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info_buf);

  fprintf(stderr, "[%s.%03ld] [%s] %s:%d: ", time_buf,
          (long)(tv.tv_usec / 1000), level, file, line);
  vfprintf(stderr, fmt, args);

  fprintf(stderr, "\n");
}

// Public logging functions: These provide type-safe wrappers around the
// internal logging function, with automatic log level filtering to reduce
// overhead when messages won't be displayed.
static inline void log_error(const char *file, int line, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log_with_time("ERROR", file, line, fmt, args);
  va_end(args);
}

static inline void log_warning(const char *file, int line, const char *fmt,
                               ...) {
  if (get_log_level() >= LOG_LEVEL_WARNING) {
    va_list args;
    va_start(args, fmt);
    log_with_time("WARNING", file, line, fmt, args);
    va_end(args);
  }
}

static inline void log_info(const char *file, int line, const char *fmt, ...) {
  if (get_log_level() >= LOG_LEVEL_INFO) {
    va_list args;
    va_start(args, fmt);
    log_with_time("INFO", file, line, fmt, args);
    va_end(args);
  }
}

static inline void log_debug(const char *file, int line, const char *fmt, ...) {
  if (get_log_level() >= LOG_LEVEL_DEBUG) {
    va_list args;
    va_start(args, fmt);
    log_with_time("DEBUG", file, line, fmt, args);
    va_end(args);
  }
}

// Convenience macros that pass file and line automatically: These eliminate
// boilerplate by automatically inserting source location, making logging calls
// cleaner while preserving debugging information.
#define LOG_ERROR(fmt, ...) log_error(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) \
  log_warning(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) log_info(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) log_debug(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

enum retry_state { RETRY_NETWORK = -1, RETRY_SERVER = 500 };

// Runtime verification macro that logs errors without crashing.
// Unlike assertions which detect programmer errors, VERIFY handles runtime
// conditions that may fail due to external factors. This follows the fail-fast
// principle while maintaining service availability.
#define VERIFY(condition)                               \
  do {                                                  \
    if (!(condition)) {                                 \
      LOG_ERROR("Verification failed: %s", #condition); \
    }                                                   \
  } while (0)

// Check pointer and return on NULL: This macro helps enforce null-safety at
// function boundaries by providing a consistent pattern for null checks with
// logging. It reduces boilerplate while ensuring all null pointer errors are
// properly logged for debugging.
#define CHECK_NULL(ptr, ret)                            \
  do {                                                  \
    if ((ptr) == nullptr) {                             \
      LOG_ERROR("Null pointer check failed: %s", #ptr); \
      return (ret);                                     \
    }                                                   \
  } while (0)

// Check condition and return on failure.
// Provides a generic assertion mechanism with custom error messages.
#define CHECK_COND(cond, ret, fmt, ...) \
  do {                                  \
    if (!(cond)) {                      \
      LOG_ERROR(fmt, ##__VA_ARGS__);    \
      return (ret);                     \
    }                                   \
  } while (0)

// Get human-readable error description for diagnostics.
CCHD_NODISCARD static inline const char *cchd_strerror(
    enum cchd_error error_code) {
  switch (error_code) {
  // Success and hook-specific codes
  case CCHD_SUCCESS:
    return "Success";
  case CCHD_ERROR_BLOCKED:
    return "Operation blocked";
  case CCHD_ERROR_ASK_USER:
    return "User approval required";

  // Input/configuration errors
  case CCHD_ERROR_INVALID_ARG:
    return "Invalid argument";
  case CCHD_ERROR_INVALID_URL:
    return "Invalid URL format";
  case CCHD_ERROR_INVALID_JSON:
    return "JSON parse error";
  case CCHD_ERROR_INVALID_HOOK:
    return "Invalid hook event or missing required fields";
  case CCHD_ERROR_CONFIG:
    return "Configuration file error";
  case CCHD_ERROR_CONFIG_PARSE:
    return "Configuration file JSON parse error";
  case CCHD_ERROR_CONFIG_INVALID:
    return "Configuration file has invalid values";

  // Network/communication errors
  case CCHD_ERROR_NETWORK:
    return "Network error";
  case CCHD_ERROR_CONNECTION:
    return "Connection refused or failed";
  case CCHD_ERROR_TIMEOUT:
    return "Request timeout";
  case CCHD_ERROR_TLS:
    return "TLS/SSL error";
  case CCHD_ERROR_DNS:
    return "DNS resolution failure";
  case CCHD_ERROR_HTTP_CLIENT:
    return "HTTP client error (4xx)";
  case CCHD_ERROR_HTTP_SERVER:
    return "HTTP server error (5xx)";
  case CCHD_ERROR_RATE_LIMIT:
    return "Rate limit exceeded";
  case CCHD_ERROR_AUTH:
    return "Authentication/authorization error";
  case CCHD_ERROR_PROXY:
    return "Proxy connection error";

  // System errors
  case CCHD_ERROR_MEMORY:
    return "Memory allocation error";
  case CCHD_ERROR_IO:
    return "I/O error";
  case CCHD_ERROR_PERMISSION:
    return "Permission denied";
  case CCHD_ERROR_INTERNAL:
    return "Internal error";
  case CCHD_ERROR_THREADING:
    return "Thread/mutex error";
  case CCHD_ERROR_RESOURCE:
    return "Resource exhaustion";
  case CCHD_ERROR_SIGNAL:
    return "Signal handling error";

  // Server response errors
  case CCHD_ERROR_SERVER_INVALID:
    return "Invalid server response format";
  case CCHD_ERROR_SERVER_MODIFY:
    return "Server returned modified data";
  case CCHD_ERROR_ALL_SERVERS_FAILED:
    return "All servers failed";
  case CCHD_ERROR_PROTOCOL:
    return "Protocol violation";
  case CCHD_ERROR_JSON_MISSING_FIELD:
    return "Required JSON field missing";
  case CCHD_ERROR_JSON_TYPE_MISMATCH:
    return "JSON field has wrong type";
  case CCHD_ERROR_SERVER_BUSY:
    return "Server temporarily unavailable";
  case CCHD_ERROR_UNSUPPORTED:
    return "Unsupported operation/feature";

  default:
    return "Unknown error";
  }
}

/*
 * Memory Management: Functions for secure memory handling, particularly for
 * sensitive data like API keys. These ensure data is properly zeroed when
 * no longer needed to prevent information leakage.
 */

// Securely zero memory to prevent sensitive data from persisting.
// This function uses platform-specific secure zeroing when available,
// falling back to a volatile write pattern with a compiler barrier
// to prevent dead store elimination optimizations.
static void secure_zero(void *ptr, size_t len) {
  if (ptr == nullptr || len == 0)
    return;

#if defined(__STDC_LIB_EXT1__)
  // C11 Annex K provides memset_s for secure zeroing.
  memset_s(ptr, len, 0, len);
#elif defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__)
  // BSD systems provide explicit_bzero for this purpose.
  explicit_bzero(ptr, len);
#else
  // Fallback implementation using volatile to prevent optimization.
  // The compiler barrier ensures the zeroing is not eliminated.
  volatile unsigned char *p = ptr;
  while (len--) {
    *p++ = 0;
  }
  // Compiler barrier to prevent dead store elimination.
  __asm__ __volatile__("" : : "r"(ptr) : "memory");
#endif
}

static void *secure_malloc(size_t size) {
  if (size == 0)
    return NULL;

  void *ptr = malloc(size);
  if (ptr == NULL) {
    return NULL;
  }

  // Attempt to lock memory to prevent swapping to disk.
  // This is best-effort as it may fail due to system limits.
  // We continue even on failure to maintain availability.
  if (mlock(ptr, size) != 0) {
    LOG_WARNING("Failed to mlock() %zu bytes. Check user limits (ulimit -l).",
                size);
  }

  // Zero the memory to ensure no previous data leaks through.
  memset(ptr, 0, size);
  return ptr;
}

static void *secure_realloc(void *ptr, size_t old_size, size_t new_size) {
  if (new_size == 0) {
    if (ptr != nullptr) {
      secure_zero(ptr, old_size);
      munlock(ptr, old_size);
      free(ptr);
    }
    return nullptr;
  }

  void *new_ptr = secure_malloc(new_size);
  if (new_ptr == nullptr) {
    return nullptr;
  }

  if (ptr != NULL) {
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_size);
    secure_zero(ptr, old_size);
    munlock(ptr, old_size);
    free(ptr);
  }

  return new_ptr;
}

static void secure_free(void *ptr, size_t size) {
  if (ptr == nullptr)
    return;

  secure_zero(ptr, size);
  munlock(ptr, size);
  free(ptr);
}

CCHD_NODISCARD static char *secure_strdup(const char *s) {
  if (s == nullptr) {
    return nullptr;
  }
  size_t len = strlen(s);
  char *new_str = secure_malloc(len + 1);
  if (new_str == nullptr) {
    return nullptr;
  }
  memcpy(new_str, s, len + 1);
  return new_str;
}

/*
 * ============================================================================
 * Utility Functions: Helper functions for common operations like JSON handling,
 * string manipulation, and system interactions. These abstract away platform
 * differences and provide consistent error handling.
 * ============================================================================
 */

// Generate an RFC 3339 formatted timestamp for CloudEvents.
// Returns a newly allocated string that must be freed by the caller.
CCHD_NODISCARD static char *generate_rfc3339_timestamp(void) {
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

  char *timestamp = secure_malloc(TIMESTAMP_BUFFER_SIZE);
  if (timestamp == nullptr) {
    return nullptr;
  }

  // Format timestamp according to RFC 3339: YYYY-MM-DDTHH:MM:SSZ.
  if (strftime(timestamp, TIMESTAMP_BUFFER_SIZE, "%Y-%m-%dT%H:%M:%SZ",
               tm_info) == 0) {
    secure_free(timestamp, TIMESTAMP_BUFFER_SIZE);
    return NULL;
  }

  return timestamp;
}

// Display concise help when no arguments are provided.
static void print_concise_help(const char *program_name) {
  if (program_name == nullptr || strlen(program_name) == 0) {
    LOG_ERROR("Invalid program name");
    program_name = "cchd";
  }

  printf("cchd - commandline Claude Code hooks dispatcher [version %s]\n\n",
         CCHD_VERSION);

  printf("Usage:    %s [options]\n", program_name);
  printf("          %s init <template> [filename]\n\n", program_name);

  printf(
      "cchd is a tool for processing Claude Code hook events, applying "
      "custom\n");
  printf(
      "server logic to allow, block, or modify operations before they "
      "execute.\n\n");

  printf("Commands:\n");
  printf("  init      Initialize a new hook server from a template\n\n");

  printf("Example:\n\n");
  printf(
      "    $ echo '{\"hook_event_name\": \"PreToolUse\", \"session_id\": "
      "\"abc123\"}' | %s\n",
      program_name);
  printf("    Connecting to http://localhost:8080/hook...\n");
  printf(
      "    {\"hook_event_name\": \"PreToolUse\", \"session_id\": "
      "\"abc123\"}\n\n");

  printf("For a listing of options, use %s --help.\n", program_name);
}

// Display full help for --help flag.
static void print_verbose_usage(const char *program_name) {
  if (program_name == nullptr || strlen(program_name) == 0) {
    LOG_ERROR("Invalid program name");
    program_name = "cchd";
  }

  const char *bold = use_colors(nullptr) ? COLOR_BOLD : "";
  const char *reset = use_colors(NULL) ? COLOR_RESET : "";

  printf("cchd - commandline Claude Code hooks dispatcher [version %s]\n\n",
         CCHD_VERSION);

  printf("%sUSAGE%s\n", bold, reset);
  printf("  %s [options]\n", program_name);
  printf("  %s init <template> [filename]\n\n", program_name);

  printf("%sDESCRIPTION%s\n", bold, reset);
  printf("  Processes Claude Code hook events through custom servers.\n\n");

  printf("%sCOMMANDS%s\n", bold, reset);
  printf("  init <template>       Initialize a new hook server from a template\n");
  printf("                        Available templates: python, typescript, go\n\n");

  printf("%sOPTIONS%s\n", bold, reset);
  printf("  -h, --help            Show this help message\n");
  printf("  -q, --quiet           Suppress non-essential output\n");
  printf("  -d, --debug           Enable debug output\n");
  printf("  --server URL          Server endpoint (default: %s)\n",
         DEFAULT_SERVER_URL);
  printf("  --timeout MS          Request timeout (default: %dms)\n",
         DEFAULT_TIMEOUT_MS);
  printf(
      "  --fail-open           Allow if server unavailable (default: block)\n");
  printf("  --api-key KEY         API key for authentication\n");
  printf("  --json                Output JSON format\n");
  printf("  --no-color            Disable colors\n\n");

  printf("%sQUICK START%s\n", bold, reset);
  printf("  1. Create and start a template server:\n");
  printf("     $ %s init python\n", program_name);
  printf("     $ uv run quickstart-python.py\n\n");
  printf("     Or use other templates:\n");
  printf("     $ %s init typescript\n", program_name);
  printf("     $ bun quickstart-typescript.ts\n\n");

  printf("  2. Configure Claude to use http://localhost:8080/hook\n\n");

  printf("  3. Test: echo '{\"hook_event_name\":\"PreToolUse\"}' | %s\n\n",
         program_name);

  printf("%sSERVER RESPONSE FORMAT%s\n", bold, reset);
  printf("  {\"decision\": \"allow\"}                    # Allow operation\n");
  printf(
      "  {\"decision\": \"block\", \"reason\": \"...\"}    # Block with "
      "reason\n");
  printf(
      "  {\"decision\": \"modify\", \"modified_data\": {...}}  # Modify "
      "data\n\n");

  printf("Docs & Templates: https://github.com/sammyjoyce/cchd\n");
}

/*
 * Input/Output Handling: Functions for reading hook data from stdin and writing
 * responses to stdout. These handle large inputs efficiently with growing
 * buffers and proper error checking.
 */
#if __STDC_VERSION__ >= 202311L
// Forward declaration
static char *read_input_from_stdin(void);

// Set file descriptor to non-blocking mode
static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Read input from stdin using asynchronous I/O for better responsiveness
CCHD_NODISCARD static char *read_input_from_stdin_async(void) {
  if (stdin == NULL) {
    LOG_ERROR("stdin is NULL");
    return nullptr;
  }

  int stdin_fd = fileno(stdin);

  // Check if stdin is a regular file (not async-capable)
  struct stat st;
  if (fstat(stdin_fd, &st) == 0 && S_ISREG(st.st_mode)) {
    // Fall back to synchronous read for regular files
    return read_input_from_stdin();
  }

  // Set stdin to non-blocking mode
  int original_flags = fcntl(stdin_fd, F_GETFL, 0);
  if (original_flags == -1 || set_nonblocking(stdin_fd) == -1) {
    LOG_WARNING("Failed to set non-blocking mode, falling back to sync read");
    return read_input_from_stdin();
  }

  size_t capacity = INPUT_BUFFER_INITIAL_SIZE;
  char *buffer = secure_malloc(capacity);
  if (buffer == NULL) {
    fcntl(stdin_fd, F_SETFL, original_flags);  // Restore original flags
    errno = ENOMEM;
    return nullptr;
  }

  size_t total_size = 0;
  struct pollfd pfd = {.fd = stdin_fd, .events = POLLIN};

  while (1) {
    // Poll with 100ms timeout for responsiveness
    int poll_result = poll(&pfd, 1, 100);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;  // Interrupted by signal, retry
      }
      LOG_ERROR("poll() failed: %s", strerror(errno));
      secure_free(buffer, capacity);
      fcntl(stdin_fd, F_SETFL, original_flags);
      return nullptr;
    }

    if (poll_result == 0) {
      // Timeout - check if we should continue waiting
      continue;
    }

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      // Error or hangup
      if (total_size > 0) {
        break;  // We have data, treat as EOF
      }
      secure_free(buffer, capacity);
      fcntl(stdin_fd, F_SETFL, original_flags);
      return nullptr;
    }

    if (pfd.revents & POLLIN) {
      // Data available to read
      size_t remaining_capacity = capacity - total_size;
      if (remaining_capacity < INPUT_BUFFER_READ_CHUNK_SIZE) {
        if (capacity >= INPUT_MAX_SIZE) {
          LOG_ERROR("Input exceeds maximum size limit (%d bytes)",
                    INPUT_MAX_SIZE);
          secure_free(buffer, capacity);
          fcntl(stdin_fd, F_SETFL, original_flags);
          errno = E2BIG;
          return nullptr;
        }
        size_t new_capacity = capacity * 2;
        if (new_capacity > INPUT_MAX_SIZE) {
          new_capacity = INPUT_MAX_SIZE;
        }
        char *new_buffer = secure_realloc(buffer, capacity, new_capacity);
        if (new_buffer == NULL) {
          secure_free(buffer, capacity);
          fcntl(stdin_fd, F_SETFL, original_flags);
          errno = ENOMEM;
          return nullptr;
        }
        buffer = new_buffer;
        capacity = new_capacity;
        remaining_capacity = capacity - total_size;
      }

      size_t bytes_to_read = remaining_capacity < INPUT_BUFFER_READ_CHUNK_SIZE
                                 ? remaining_capacity
                                 : INPUT_BUFFER_READ_CHUNK_SIZE;

      ssize_t bytes_read = read(stdin_fd, buffer + total_size, bytes_to_read);
      if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;  // No data available right now
        }
        LOG_ERROR("read() failed: %s", strerror(errno));
        secure_free(buffer, capacity);
        fcntl(stdin_fd, F_SETFL, original_flags);
        return nullptr;
      }

      if (bytes_read == 0) {
        break;  // EOF
      }

      total_size += bytes_read;
    }
  }

  // Restore original flags
  fcntl(stdin_fd, F_SETFL, original_flags);

  buffer[total_size] = '\0';
  if (strlen(buffer) != total_size || total_size > INPUT_MAX_SIZE) {
    LOG_ERROR("Buffer size validation failed");
    secure_free(buffer, capacity);
    return NULL;
  }

  // Shrink buffer to exact size
  size_t exact_size = total_size + 1;
  if (capacity > exact_size) {
    char *shrunk = secure_realloc(buffer, capacity, exact_size);
    if (shrunk) {
      buffer = shrunk;
      capacity = exact_size;
    }
  }

  return buffer;
}
#endif

// Original synchronous version kept for compatibility
CCHD_NODISCARD static char *read_input_from_stdin(void) {
  // Read all input from stdin into a dynamically growing buffer.
  // We enforce an upper bound on total size to prevent denial-of-service
  // from unbounded input. The buffer grows exponentially to amortize
  // reallocation costs while maintaining a strict size limit.
  if (stdin == NULL) {
    LOG_ERROR("stdin is NULL");
    return nullptr;
  }

  // Optimize initial allocation by checking if stdin is a regular file.
  // For files, we can pre-allocate the exact size needed.
  size_t capacity = INPUT_BUFFER_INITIAL_SIZE;
  struct stat st;
  if (fstat(fileno(stdin), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
    // Use file size as capacity hint, capped at maximum allowed size.
    capacity = st.st_size < INPUT_MAX_SIZE ? st.st_size + 1 : INPUT_MAX_SIZE;
  }

  if (capacity == 0 || capacity > INPUT_MAX_SIZE) {
    LOG_ERROR("Invalid initial buffer capacity");
    return nullptr;
  }
  char *buffer = secure_malloc(capacity);
  if (buffer == NULL) {
    errno = ENOMEM;
    return nullptr;
  }

  size_t total_size = 0;
  while (1) {
    size_t remaining_capacity = capacity - total_size;
    if (remaining_capacity < INPUT_BUFFER_READ_CHUNK_SIZE) {
      if (capacity >= INPUT_MAX_SIZE) {
        LOG_ERROR("Input exceeds maximum size limit (%d bytes)",
                  INPUT_MAX_SIZE);
        secure_free(buffer, capacity);
        errno = E2BIG;
        return nullptr;
      }
      size_t new_capacity = capacity * 2;
      if (new_capacity > INPUT_MAX_SIZE) {
        new_capacity = INPUT_MAX_SIZE;
      }
      char *new_buffer = secure_realloc(buffer, capacity, new_capacity);
      if (new_buffer == NULL) {
        secure_free(buffer, capacity);
        errno = ENOMEM;
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
      // Short read indicates EOF or error.
      // We must distinguish between these cases: EOF is normal termination,
      // while read errors indicate system problems that must be handled.
      if (ferror(stdin)) {
        secure_free(buffer, capacity);
        return nullptr;
      }
      break;
    }
  }

  buffer[total_size] = '\0';
  if (strlen(buffer) != total_size || total_size > INPUT_MAX_SIZE) {
    LOG_ERROR("Buffer size validation failed");
    secure_free(buffer, capacity);
    return NULL;
  }

  // Shrink buffer to exact size to ensure all allocated memory is properly
  // tracked. This is critical for secure_free to zero the correct amount of
  // memory.
  size_t exact_size = total_size + 1;
  if (capacity > exact_size) {
    char *shrunk = secure_realloc(buffer, capacity, exact_size);
    if (shrunk) {
      buffer = shrunk;
      capacity = exact_size;
    } else {
      LOG_ERROR("Failed to shrink input buffer to exact size");
      // Continue with original buffer as this is not a fatal error.
    }
  }

  return buffer;
}

// Output the appropriate JSON to stdout unless suppressed.
// Uses modified output if available, otherwise echoes input.
static void handle_output(bool suppress_output,
                          const char *modified_output_json,
                          const char *input_json_string,
                          const configuration_t *config, int32_t exit_code) {
  if (input_json_string == nullptr || config == nullptr) {
    LOG_ERROR("Invalid parameters in handle_output");
    return;
  }
  // modified_output_json can be nullptr

  if (!suppress_output) {
    if (config->json_output) {
      // Output structured JSON response
      printf("{\"status\":\"%s\",\"exit_code\":%d,\"modified\":%s",
             exit_code == 0 ? "allowed"
                            : (exit_code == 1 ? "blocked" : "ask_user"),
             exit_code, modified_output_json ? "true" : "false");
      if (modified_output_json) {
        printf(",\"data\":%s", modified_output_json);
      }
      printf("}\n");
    } else if (config->plain_output) {
      // Plain output without any formatting
      const char *output =
          modified_output_json ? modified_output_json : input_json_string;
      printf("%s\n", output);
    } else {
      // Default formatted output
      const char *output =
          modified_output_json ? modified_output_json : input_json_string;
      printf("%s\n", output);
    }
  }
}

/*
 * HTTP Communication: Functions for sending hook events to HTTP servers using
 * libcurl. These handle connection pooling, retries, and proper error reporting
 * to ensure reliable communication with hook servers.
 */

static size_t write_callback(void *contents, size_t size, size_t nmemb,
                             void *userp) {
  if (contents == NULL || userp == NULL || size == 0 || nmemb == 0) {
    LOG_ERROR("Invalid parameters in write_callback");
    return 0;
  }

  response_buffer_t *response_buffer = (response_buffer_t *)userp;

  // Prevent overflow in size calculation.
  if (nmemb > SIZE_MAX / size) {
    return 0;
  }
  size_t real_size = size * nmemb;

  // Prevent overflow in new size calculation.
  if (response_buffer->size > SIZE_MAX - real_size - 1) {
    return 0;
  }
  size_t required_size = response_buffer->size + real_size + 1;

  // Only reallocate if we need more space
  if (required_size > response_buffer->capacity) {
    // Double the capacity or use required size, whichever is larger
    size_t new_capacity = response_buffer->capacity * 2;
    if (new_capacity < required_size) {
      new_capacity = required_size;
    }

    char *new_data = secure_realloc(response_buffer->data,
                                    response_buffer->capacity, new_capacity);
    if (new_data == NULL) {
      LOG_ERROR("Failed to allocate memory for response buffer (size: %zu)",
                new_capacity);
      return 0;  // Tell curl to stop the transfer
    }

    response_buffer->data = new_data;
    response_buffer->capacity = new_capacity;
  }
  memcpy(&(response_buffer->data[response_buffer->size]), contents, real_size);
  response_buffer->size += real_size;
  response_buffer->data[response_buffer->size] = 0;

  return real_size;
}

// Get or create the global curl handle for connection reuse
static CURL *get_global_curl_handle(void) {
  pthread_mutex_lock(&g_curl_mutex);
  if (g_curl_handle == NULL) {
    g_curl_handle = curl_easy_init();
    if (g_curl_handle != NULL) {
      // Set persistent options that don't change between requests
      curl_easy_setopt(g_curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);
      curl_easy_setopt(g_curl_handle, CURLOPT_TCP_KEEPIDLE, 120L);
      curl_easy_setopt(g_curl_handle, CURLOPT_TCP_KEEPINTVL, 60L);
    }
  }
  pthread_mutex_unlock(&g_curl_mutex);
  return g_curl_handle;
}

// Perform a single HTTP POST request using the global curl handle.
// Returns HTTP status code on success, -1 on network/curl errors.
CCHD_NODISCARD static int32_t perform_single_request_with_handle(
    CURL *curl_handle, const configuration_t *configuration,
    const char *json_payload, response_buffer_t *server_response,
    const char *program_name) {
  if (curl_handle == nullptr || configuration == nullptr ||
      json_payload == nullptr || server_response == nullptr ||
      configuration->server_urls == nullptr ||
      configuration->server_count == 0 || configuration->timeout_ms <= 0) {
    LOG_ERROR("Invalid parameters in perform_single_request_with_handle");
    return -1;
  }

  // Buffer for detailed curl error messages.
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

  if (configuration->api_key && strlen(configuration->api_key) > 0) {
    char auth_buffer[1024];
    int auth_len = snprintf(auth_buffer, sizeof(auth_buffer),
                            "Authorization: Bearer %s", configuration->api_key);
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

  curl_easy_setopt(curl_handle, CURLOPT_URL, configuration->server_urls[0]);
  curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, json_payload);
  curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, http_headers);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, server_response);
  curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, configuration->timeout_ms);

  if (configuration->insecure) {
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
  }

  // Enable compression to reduce network bandwidth usage.
  curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");

  // Enable TCP keepalive for connection reuse across retries.
  curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);

  // Enable detailed error messages for better diagnostics.
  curl_easy_setopt(curl_handle, CURLOPT_ERRORBUFFER, curl_error_buffer);
  LOG_DEBUG("Sending request to %s (timeout: %ldms)",
            configuration->server_urls[0], (long)configuration->timeout_ms);
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

    // Map CURL errors to specific CCHD error codes
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

    // Provide actionable suggestions based on error type.
    if (!configuration->quiet && !configuration->json_output) {
      const char *red = use_colors(configuration) ? COLOR_RED : "";
      const char *yellow = use_colors(configuration) ? COLOR_YELLOW : "";
      const char *reset = use_colors(configuration) ? COLOR_RESET : "";

      if (curl_result == CURLE_COULDNT_CONNECT) {
        fprintf(stderr, "\n%sCould not connect to %s%s\n\n", red,
                configuration->server_urls[0], reset);
        fprintf(stderr, "Check that:\n");
        fprintf(stderr, "  • The server is running\n");
        fprintf(stderr, "  • The URL is correct\n");
        fprintf(stderr, "  • No firewall is blocking the connection\n\n");
        fprintf(stderr, "You might need to:\n");
        fprintf(stderr, "  %s%s --server http://different-server.com%s\n",
                yellow, program_name ? program_name : "cchd", reset);
      } else if (curl_result == CURLE_OPERATION_TIMEDOUT) {
        fprintf(stderr, "\n%sRequest timed out after %ldms%s\n\n", red,
                (long)configuration->timeout_ms, reset);
        fprintf(stderr, "Try:\n");
        fprintf(stderr, "  • Increasing timeout: %s%s --timeout 10000%s\n",
                yellow, program_name ? program_name : "cchd", reset);
        fprintf(stderr, "  • Checking your network connection\n");
        fprintf(stderr, "  • Verifying the server is responding\n");
      } else if (curl_result == CURLE_URL_MALFORMAT) {
        fprintf(stderr, "\n%sInvalid URL format: %s%s\n\n", red,
                configuration->server_urls[0], reset);
        fprintf(stderr, "URLs should be like:\n");
        fprintf(stderr, "  • http://localhost:8080/hook\n");
        fprintf(stderr, "  • https://example.com/webhook\n\n");
        fprintf(stderr, "Example:\n");
        fprintf(stderr, "  %s%s --server https://api.example.com/hook%s\n",
                yellow, program_name ? program_name : "cchd", reset);
      }
    }
    // Return negative of the specific error code to distinguish from HTTP
    // status
    return -error_code;
  }

  LOG_DEBUG("HTTP request completed with status %ld", (long)http_status);

  return (int32_t)http_status;
}

// Calculate adaptive retry delay based on error type
CCHD_NODISCARD static int32_t calculate_retry_delay(int32_t http_status,
                                                    int32_t base_delay_ms,
                                                    int32_t attempt) {
  int32_t delay_ms = base_delay_ms;

  if (http_status < 0) {
    // Negative values are CCHD error codes
    int32_t error_code = -http_status;
    switch (error_code) {
    case CCHD_ERROR_CONNECTION:
    case CCHD_ERROR_DNS:
      // Connection/DNS errors: shorter initial delay
      delay_ms = 250 + (rand() % 250);  // 250-500ms initial
      if (attempt > 0) {
        delay_ms *= (1 << attempt);  // 2^attempt multiplier
      }
      if (delay_ms > 3000)
        delay_ms = 3000;  // Cap at 3 seconds
      break;
    case CCHD_ERROR_TIMEOUT:
      // Timeout: longer delay as server might be overloaded
      delay_ms = 1000 + (rand() % 500);  // 1-1.5s initial
      if (attempt > 0) {
        delay_ms *= 2;
      }
      if (delay_ms > 5000)
        delay_ms = 5000;  // Cap at 5 seconds
      break;
    default:
      // Other network errors
      delay_ms = 500 + (rand() % 500);  // 500ms-1s initial
      if (attempt > 0) {
        delay_ms *= 2;
      }
      if (delay_ms > 3000)
        delay_ms = 3000;  // Cap at 3 seconds
      break;
    }
  } else if (http_status >= 500 && http_status < 600) {
    // Server errors: longer delays to give server time to recover
    delay_ms = 1000 + (rand() % 500);  // 1-1.5s initial
    if (attempt > 0) {
      delay_ms *= (2 + attempt);  // More aggressive backoff
    }
    if (delay_ms > 10000)
      delay_ms = 10000;  // Cap at 10 seconds
  } else if (http_status == 429) {
    // Rate limiting: respect server's rate limits
    delay_ms = 5000 + (rand() % 2000);  // 5-7s initial
    if (attempt > 0) {
      delay_ms *= 2;
    }
    if (delay_ms > 30000)
      delay_ms = 30000;  // Cap at 30 seconds
  }

  return delay_ms;
}

CCHD_NODISCARD static int32_t send_request_to_server(
    const configuration_t *configuration, const char *json_payload,
    response_buffer_t *server_response, const char *program_name) {
  // Send JSON payload to servers with adaptive retry on transient failures.
  // Try each server in the array before moving to the next.
  // We use adaptive backoff based on error type to optimize retry behavior.
  // The response buffer is reset between attempts to prevent data corruption.

  if (configuration == NULL || json_payload == NULL ||
      server_response == NULL || configuration->server_count == 0) {
    return -1;
  }

  // Adaptive retry configuration based on error type
  const int32_t max_network_retries = 3;
  const int32_t max_server_error_retries = 2;
  const int32_t no_retry_for_client_errors = 1;

  // Get the global curl handle for reuse across all servers and retries.
  CURL *reusable_curl_handle = get_global_curl_handle();
  if (reusable_curl_handle == NULL) {
    return -1;
  }

  // Lock the curl handle for thread safety
  pthread_mutex_lock(&g_curl_mutex);

  // Pre-allocate response buffer to avoid reallocation during response
  // handling.
  if (server_response->data == NULL) {
    server_response->data = secure_malloc(RESPONSE_BUFFER_INITIAL_SIZE);
    if (server_response->data != NULL) {
      server_response->capacity = RESPONSE_BUFFER_INITIAL_SIZE;
    }
  }

  // Try each server in the list
  for (size_t server_idx = 0; server_idx < configuration->server_count;
       server_idx++) {
    const char *current_server_url = configuration->server_urls[server_idx];
    if (current_server_url == NULL || strlen(current_server_url) == 0) {
      continue;
    }

    // Show progress message before network operation.
    if (!configuration->quiet && !configuration->json_output) {
      if (server_idx > 0) {
        fprintf(stderr, "Trying fallback server %s...\n", current_server_url);
      } else {
        fprintf(stderr, "Connecting to %s...\n", current_server_url);
      }
      fflush(stderr);  // Ensure immediate output
    }

    int32_t last_http_status = -1;
    int32_t max_attempts =
        max_network_retries;  // Default to network retry count

    // Try current server with adaptive retries
    for (int32_t attempt = 0; attempt < max_attempts; attempt++) {
      if (attempt > 0 || server_idx > 0) {
        // Reset response buffer but keep allocation to avoid reallocation.
        server_response->size = 0;
        if (server_response->data != NULL) {
          server_response->data[0] = '\0';
        }

        // Reset curl handle to clear previous request state.
        curl_easy_reset(reusable_curl_handle);

        if (attempt > 0) {
          // Calculate adaptive delay based on last error
          int32_t retry_delay_ms = calculate_retry_delay(
              last_http_status, INITIAL_RETRY_DELAY_MS, attempt - 1);
          LOG_DEBUG("Waiting %dms before retry (error was %d)", retry_delay_ms,
                    last_http_status);
          usleep((uint32_t)retry_delay_ms * 1000);
        }
      }

      // Create a temporary configuration with the current server URL
      configuration_t temp_config = *configuration;
      temp_config.server_urls = &current_server_url;
      temp_config.server_count = 1;

      int32_t http_status = perform_single_request_with_handle(
          reusable_curl_handle, &temp_config, json_payload, server_response,
          program_name);

      last_http_status = http_status;

      if (http_status == 200) {
        if (!configuration->quiet && !configuration->json_output &&
            server_idx > 0) {
          fprintf(stderr, "Successfully connected to fallback server\n");
        }
        pthread_mutex_unlock(&g_curl_mutex);
        return http_status;
      }

      // Determine retry strategy based on error type
      bool should_retry = false;

      if (http_status < 0) {
        // Negative values are CCHD error codes from curl failures
        int32_t error_code = -http_status;
        switch (error_code) {
        case CCHD_ERROR_CONNECTION:
        case CCHD_ERROR_TIMEOUT:
        case CCHD_ERROR_NETWORK:
        case CCHD_ERROR_DNS:
          // Network errors: retry with adaptive delay
          should_retry = true;
          max_attempts = max_network_retries;
          break;
        case CCHD_ERROR_INVALID_URL:
        case CCHD_ERROR_TLS:
          // configuration_t errors: don't retry
          should_retry = false;
          break;
        default:
          should_retry = true;
          max_attempts = max_network_retries;
          break;
        }
      } else if (http_status >= 500 && http_status < 600) {
        // Server errors: fewer retries with longer delays
        should_retry = true;
        max_attempts = max_server_error_retries;
      } else if (http_status == 429) {
        // Rate limiting: always retry with appropriate delay
        should_retry = true;
        max_attempts = max_server_error_retries;
      } else if (http_status >= 400 && http_status < 500) {
        // Client errors: don't retry (except 429 above)
        should_retry = false;
        max_attempts = no_retry_for_client_errors;
      }

      if (!should_retry) {
        // Non-retryable error - try next server if available
        if (!configuration->quiet && !configuration->json_output) {
          if (http_status >= 400 && http_status < 500) {
            fprintf(stderr, "Client error (HTTP %d) - not retrying\n",
                    http_status);
          }
        }
        break;
      }

      if (attempt < max_attempts - 1 && !configuration->quiet &&
          !configuration->json_output) {
        fprintf(stderr,
                "Request failed (HTTP %d, attempt %d/%d), retrying...\n",
                http_status, attempt + 1, max_attempts);
        fflush(stderr);
      }
    }

    // If we have more servers to try, continue to the next one
    if (server_idx < configuration->server_count - 1 && !configuration->quiet &&
        !configuration->json_output) {
      fprintf(stderr, "Server %s unavailable, trying next server...\n",
              current_server_url);
    }
  }

  pthread_mutex_unlock(&g_curl_mutex);
  return -CCHD_ERROR_ALL_SERVERS_FAILED;
}

/*
 * Input Validation: Functions to validate and sanitize input data before
 * processing. These prevent security issues and ensure the application
 * behaves predictably with malformed or malicious input.
 */

static bool validate_server_url(const char *url,
                                const configuration_t *config) {
  if (url == NULL || strlen(url) == 0) {
    if (!config->quiet && !config->json_output) {
      fprintf(stderr, "Error: Server URL cannot be empty\n");
    }
    return false;
  }

  // Basic URL format validation using simple pattern matching
  // Must start with http:// or https://
  if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
    if (!config->quiet && !config->json_output) {
      const char *red = use_colors(config) ? COLOR_RED : "";
      const char *reset = use_colors(config) ? COLOR_RESET : "";
      fprintf(stderr, "%sError: Invalid URL format: %s%s\n", red, url, reset);
      fprintf(stderr, "URLs must start with 'http://' or 'https://'\n");
    }
    return false;
  }

  // Check for HTTPS enforcement
  if (!config->insecure && strncmp(url, "http://", 7) == 0) {
    // Allow localhost and 127.0.0.1 for development
    const char *host_start = url + 7;  // Skip "http://"
    if (strncmp(host_start, "localhost", 9) != 0 &&
        strncmp(host_start, "127.0.0.1", 9) != 0 &&
        strncmp(host_start, "[::1]", 5) != 0) {
      if (!config->quiet && !config->json_output) {
        const char *yellow = use_colors(config) ? COLOR_YELLOW : "";
        const char *reset = use_colors(config) ? COLOR_RESET : "";
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
    return false;  // Already checked above, shouldn't happen
  }

  const char *host_start = scheme_end + 3;
  if (*host_start == '\0') {
    if (!config->quiet && !config->json_output) {
      fprintf(stderr, "Error: URL missing host: %s\n", url);
    }
    return false;
  }

  // Check for basic host validity (not empty, not just port)
  if (*host_start == ':' || *host_start == '/') {
    if (!config->quiet && !config->json_output) {
      fprintf(stderr, "Error: URL missing hostname: %s\n", url);
    }
    return false;
  }

  // Validate URL length
  if (strlen(url) > 2048) {
    if (!config->quiet && !config->json_output) {
      fprintf(stderr, "Error: URL too long (max 2048 characters)\n");
    }
    return false;
  }

  // Check for common URL mistakes
  if (strstr(url, " ") != NULL) {
    if (!config->quiet && !config->json_output) {
      fprintf(stderr, "Error: URL contains spaces: %s\n", url);
    }
    return false;
  }

  return true;
}

/*
 * Input Validation
 */

static bool validate_hook_event_fields(yyjson_val *input_root,
                                       const configuration_t *config) {
  if (!yyjson_is_obj(input_root)) {
    if (!config->quiet && !config->json_output) {
      fprintf(stderr, "Error: Input must be a JSON object\n");
    }
    return false;
  }

  // Required fields for all Claude Code hooks
  const char *required_fields[] = {"hook_event_name", "session_id"};

  for (size_t i = 0; i < sizeof(required_fields) / sizeof(required_fields[0]);
       i++) {
    yyjson_val *field = yyjson_obj_get(input_root, required_fields[i]);
    if (field == nullptr) {
      if (!config->quiet && !config->json_output) {
        fprintf(stderr, "Error: Missing required field '%s'\n",
                required_fields[i]);
      }
      return false;
    }
  }

  // Validate hook_event_name is a known type
  yyjson_val *hook_name = yyjson_obj_get(input_root, "hook_event_name");
  if (!yyjson_is_str(hook_name)) {
    if (!config->quiet && !config->json_output) {
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

  if (!valid_hook && !config->quiet && !config->json_output) {
    fprintf(stderr, "Warning: Unknown hook_event_name '%s'\n", hook_name_str);
    fprintf(stderr, "See documentation for a list of valid hook events.\n");
  }

  // Validate session_id is a string
  yyjson_val *session_id = yyjson_obj_get(input_root, "session_id");
  if (!yyjson_is_str(session_id)) {
    if (!config->quiet && !config->json_output) {
      fprintf(stderr, "Error: 'session_id' must be a string\n");
    }
    return false;
  }

  // Hook-specific validation
  if (strcmp(hook_name_str, "PreToolUse") == 0 ||
      strcmp(hook_name_str, "PostToolUse") == 0) {
    yyjson_val *tool_name = yyjson_obj_get(input_root, "tool_name");
    if (tool_name == nullptr && !config->quiet && !config->json_output) {
      fprintf(stderr, "Warning: %s hook should include 'tool_name' field\n",
              hook_name_str);
    }
  }

  if (strcmp(hook_name_str, "CommandExecution") == 0) {
    yyjson_val *command = yyjson_obj_get(input_root, "command");
    if (command == nullptr && !config->quiet && !config->json_output) {
      fprintf(
          stderr,
          "Warning: CommandExecution hook should include 'command' field\n");
    }
  }

  if (strcmp(hook_name_str, "FileChange") == 0) {
    yyjson_val *file_path = yyjson_obj_get(input_root, "file_path");
    if (file_path == nullptr && !config->quiet && !config->json_output) {
      fprintf(stderr,
              "Warning: FileChange hook should include 'file_path' field\n");
    }
  }

  // Check for common optional fields
  yyjson_val *transcript_path = yyjson_obj_get(input_root, "transcript_path");
  if (transcript_path == NULL && !config->quiet && !config->json_output) {
    LOG_DEBUG("Note: 'transcript_path' field not provided");
  }

  yyjson_val *cwd = yyjson_obj_get(input_root, "cwd");
  if (cwd == NULL && !config->quiet && !config->json_output) {
    LOG_DEBUG("Note: 'cwd' (current working directory) field not provided");
  }

  return true;
}

/*
 * JSON Processing
 */

CCHD_NODISCARD static yyjson_mut_val *build_data_object(
    yyjson_mut_doc *output_doc, yyjson_val *input_root) {
  // Copy the entire input JSON as the CloudEvents data payload.
  // This preserves all fields from the original stdin input without
  // modification, ensuring the server receives the complete hook context.
  if (output_doc == nullptr || input_root == nullptr) {
    LOG_ERROR("Invalid parameters in build_data_object");
    return nullptr;
  }
  CHECK_NULL(output_doc, nullptr);
  CHECK_NULL(input_root, nullptr);

  // Create a deep copy of the entire input root object.
  yyjson_mut_val *data_copy = yyjson_val_mut_copy(output_doc, input_root);
  if (data_copy == NULL) {
    return nullptr;
  }

  return data_copy;
}

// Add required CloudEvents v1.0 attributes to the output document.
// These attributes are mandatory per the CloudEvents specification
// and provide essential metadata for event routing and processing.
static bool add_required_cloudevents_attributes(yyjson_mut_doc *output_doc,
                                                yyjson_mut_val *output_root,
                                                yyjson_val *input_root) {
  if (output_doc == NULL || output_root == NULL || input_root == NULL ||
      !yyjson_is_obj(input_root)) {
    LOG_ERROR("Invalid parameters in add_required_cloudevents_attributes");
    return false;
  }
  // CloudEvents specification version.
  if (!yyjson_mut_obj_add_strcpy(output_doc, output_root, "specversion",
                                 "1.0")) {
    return false;
  }

  // Event type using CloudEvents reverse-DNS format.
  // This allows servers to route events based on hook type.
  yyjson_val *event_name_value = yyjson_obj_get(input_root, "hook_event_name");
  const char *event_name = yyjson_is_str(event_name_value)
                               ? yyjson_get_str(event_name_value)
                               : "Unknown";
  char type_buffer[TYPE_BUFFER_SIZE];
  int written = snprintf(type_buffer, sizeof(type_buffer),
                         "com.claudecode.hook.%s", event_name);
  if (written < 0 || (size_t)written >= sizeof(type_buffer)) {
    LOG_ERROR("Type buffer overflow (needed %d bytes)", written);
    return false;
  }
  if (!yyjson_mut_obj_add_strcpy(output_doc, output_root, "type",
                                 type_buffer)) {
    return false;
  }

  // Source URI identifies where the event originated.
  if (!yyjson_mut_obj_add_strcpy(output_doc, output_root, "source",
                                 "/claude-code/hooks")) {
    return false;
  }

  // Generate unique event identifier.
  // We use high-resolution timestamp to ensure uniqueness.
  char id_buffer[ID_BUFFER_SIZE] = {0};
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    // Fallback to lower resolution time with atomic counter if clock_gettime
    // fails.
    static atomic_uint_fast64_t counter = 0;
    ts.tv_sec = time(nullptr);
    ts.tv_nsec = atomic_fetch_add(&counter, 1);
    LOG_ERROR("clock_gettime failed, using fallback ID generation");
  }
  snprintf(id_buffer, sizeof(id_buffer), "%" PRIx64 "-%" PRIx64,
           (int64_t)ts.tv_sec, (int64_t)ts.tv_nsec);
  if (!yyjson_mut_obj_add_strcpy(output_doc, output_root, "id", id_buffer)) {
    return false;
  }

  return true;
}

// Add optional CloudEvents attributes and extensions.
// These provide additional context for event processing and correlation.
static bool add_optional_cloudevents_attributes(yyjson_mut_doc *output_doc,
                                                yyjson_mut_val *output_root,
                                                yyjson_val *input_root) {
  if (output_doc == NULL || output_root == NULL || input_root == NULL ||
      !yyjson_is_obj(input_root)) {
    LOG_ERROR("Invalid parameters in add_optional_cloudevents_attributes");
    return false;
  }
  // Event timestamp for temporal ordering and debugging.
  char *timestamp = generate_rfc3339_timestamp();
  if (timestamp != NULL) {
    if (!yyjson_mut_obj_add_strcpy(output_doc, output_root, "time",
                                   timestamp)) {
      secure_free(timestamp, TIMESTAMP_BUFFER_SIZE);
      return false;
    }
    secure_free(timestamp, TIMESTAMP_BUFFER_SIZE);
  }

  // Content type declaration for proper parsing.
  if (!yyjson_mut_obj_add_strcpy(output_doc, output_root, "datacontenttype",
                                 "application/json")) {
    return false;
  }

  // CloudEvents extensions for request correlation.
  // Session ID links events within a Claude Code session.
  yyjson_val *session_id_value = yyjson_obj_get(input_root, "session_id");
  if (yyjson_is_str(session_id_value)) {
    if (!yyjson_mut_obj_add_strcpy(output_doc, output_root, "sessionid",
                                   yyjson_get_str(session_id_value))) {
      return false;
    }
  }

  yyjson_val *correlation_id_value =
      yyjson_obj_get(input_root, "correlation_id");
  if (yyjson_is_str(correlation_id_value)) {
    if (!yyjson_mut_obj_add_strcpy(output_doc, output_root, "correlationid",
                                   yyjson_get_str(correlation_id_value))) {
      return false;
    }
  }

  return true;
}

static yyjson_mut_doc *transform_input_to_protocol_format(
    yyjson_doc *input_doc) {
  // Transform Claude Code hook JSON to CloudEvents format.
  // This standardization enables consistent event processing across
  // different hook types and server implementations.
  if (input_doc == NULL) {
    LOG_ERROR("Invalid input document");
    return NULL;
  }
  CHECK_NULL(input_doc, NULL);

  yyjson_val *input_root = yyjson_doc_get_root(input_doc);
  if (input_root == NULL || !yyjson_is_obj(input_root)) {
    LOG_ERROR("Invalid input document - root is not an object");
    return NULL;
  }

  LOG_DEBUG("Transforming input JSON to CloudEvents format");

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

  // Build CloudEvents structure with required attributes.
  if (!add_required_cloudevents_attributes(output_doc, output_root,
                                           input_root)) {
    yyjson_mut_doc_free(output_doc);
    return NULL;
  }

  // Add optional attributes for enhanced context.
  if (!add_optional_cloudevents_attributes(output_doc, output_root,
                                           input_root)) {
    yyjson_mut_doc_free(output_doc);
    return NULL;
  }

  // Embed original hook data as CloudEvents payload.
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

// Helper function to process input JSON and transform to CloudEvents
// format
static char *process_input_to_protocol(const char *input_json_string,
                                       const configuration_t *config) {
  if (input_json_string == NULL || strlen(input_json_string) == 0) {
    LOG_ERROR("Invalid input JSON string");
    return NULL;
  }

  yyjson_read_err err;
  memset(&err, 0, sizeof(err));
  yyjson_doc *input_json_document = yyjson_read_opts(
      (char *)input_json_string, strlen(input_json_string), 0, NULL, &err);
  if (input_json_document == NULL) {
    if (!config->quiet && !config->json_output) {
      const char *red = use_colors(config) ? COLOR_RED : "";
      const char *yellow = use_colors(config) ? COLOR_YELLOW : "";
      const char *reset = use_colors(config) ? COLOR_RESET : "";

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
  if (!validate_hook_event_fields(input_root, config)) {
    yyjson_doc_free(input_json_document);
    return NULL;
  }

  yyjson_mut_doc *protocol_json_document =
      transform_input_to_protocol_format(input_json_document);
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

  // Copy to secure memory and free the yyjson allocated memory
  char *secure_json = secure_malloc(json_len + 1);
  if (secure_json == NULL) {
    free(protocol_json_string);
    return NULL;
  }
  memcpy(secure_json, protocol_json_string, json_len + 1);
  free(protocol_json_string);

  return secure_json;
}

/*
 * Response Processing
 */

static void parse_base_response(yyjson_val *response_root, bool *continue_out,
                                bool *suppress_output_out,
                                const char **stop_reason_out) {
  if (response_root == NULL || continue_out == NULL ||
      suppress_output_out == NULL || stop_reason_out == NULL ||
      !yyjson_is_obj(response_root)) {
    LOG_ERROR("Invalid parameters in parse_base_response");
    return;
  }

  // Check for 'continue' field to determine if processing should proceed.
  yyjson_val *continue_value = yyjson_obj_get(response_root, "continue");
  if (yyjson_is_bool(continue_value)) {
    *continue_out = yyjson_get_bool(continue_value);
  } else {
    *continue_out = true;  // Default to continue if missing
  }

  yyjson_val *stop_reason_value = yyjson_obj_get(response_root, "stopReason");
  if (yyjson_is_str(stop_reason_value)) {
    *stop_reason_out = yyjson_get_str(stop_reason_value);
  } else {
    *stop_reason_out = NULL;
  }

  // Check whether to suppress stdout output.
  // This allows servers to prevent data from being passed through.
  yyjson_val *suppress_output_value =
      yyjson_obj_get(response_root, "suppressOutput");
  if (yyjson_is_bool(suppress_output_value)) {
    *suppress_output_out = yyjson_get_bool(suppress_output_value);
  } else {
    *suppress_output_out = false;  // Default to not suppress
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
      // Reason for approval shown to user
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
      // Copy to secure memory to protect potentially sensitive data.
      char *secure_json = secure_malloc(json_len + 1);
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
            // Reason shown to user
            fprintf(stderr, "✓ Allowed: %s\n", reason);
          }
        } else if (strcmp(perm, "ask") == 0) {
          // Return special exit code to signal Claude Code to prompt
          // user. Exit code 2 triggers interactive approval flow.
          *exit_code_out = 2;
          if (reason) {
            fprintf(stderr, "⚠ User approval required: %s\n", reason);
          }
        }
      }
    }
  }
}

static int32_t process_server_response(const char *response_data,
                                       char **modified_output_ptr,
                                       const configuration_t *configuration,
                                       bool *suppress_output_ptr,
                                       int32_t server_http_status) {
  // Parse the server response to determine the decision (allow, block,
  // modify) and handle accordingly. In fail-open mode, invalid
  // responses default to allow for liveness over correctness; in
  // fail-closed, they block to prioritize safety.
  if (response_data == NULL || modified_output_ptr == NULL ||
      configuration == NULL || suppress_output_ptr == NULL ||
      strlen(response_data) == 0) {
    LOG_ERROR("Invalid parameters in process_server_response");
    return configuration && configuration->fail_open ? 0 : 1;
  }

  if (server_http_status >= 400 && server_http_status < 500) {
    LOG_ERROR("Client error from server: HTTP %d", server_http_status);
    return 1;  // Block on client errors
  }
  if (server_http_status >= 500) {
    LOG_ERROR("Server error: HTTP %d", server_http_status);
    return configuration->fail_open ? 0 : 1;
  }

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
  // Already validated that response_root is obj in previous check

  bool should_continue = true;
  bool suppress_output = false;
  const char *stop_reason = NULL;
  parse_base_response(response_root, &should_continue, &suppress_output,
                      &stop_reason);

  if (!should_continue) {
    // Server requested to stop processing.
    if (stop_reason) {
      fprintf(stderr, "Stopped: %s\n", stop_reason);
    }
    yyjson_doc_free(response_doc);
    return 1;
  }

  if (suppress_output) {
    // Server requested output suppression.
    *suppress_output_ptr = true;
  }
  int32_t exit_code = 0;

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

/*
 * configuration_t File Support
 */

static char *get_config_file_path(void) {
  // Check for config file in order of precedence:
  // 1. CCHD_CONFIG environment variable
  // 2. ~/.cchd/config.json
  // 3. ~/.config/cchd/config.json

  const char *env_config = getenv("CCHD_CONFIG");
  if (env_config != NULL && access(env_config, R_OK) == 0) {
    return strdup(env_config);
  }

  const char *home = getenv("HOME");
  if (home == NULL) {
    struct passwd *pw = getpwuid(getuid());
    if (pw != NULL) {
      home = pw->pw_dir;
    }
  }

  if (home != NULL) {
    char path[PATH_MAX];

    // Try ~/.cchd/config.json
    snprintf(path, sizeof(path), "%s/.cchd/config.json", home);
    if (access(path, R_OK) == 0) {
      return strdup(path);
    }

    // Try ~/.config/cchd/config.json
    snprintf(path, sizeof(path), "%s/.config/cchd/config.json", home);
    if (access(path, R_OK) == 0) {
      return strdup(path);
    }
  }

  return NULL;
}

static void load_config_file(configuration_t *configuration) {
  char *config_path = get_config_file_path();
  if (config_path == NULL) {
    return;  // No config file found, use defaults
  }

  FILE *file = fopen(config_path, "r");
  if (file == NULL) {
    free(config_path);
    return;
  }

  // Read config file
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (file_size <= 0 || file_size > 65536) {  // Max 64KB config file
    fclose(file);
    free(config_path);
    return;
  }

  char *config_data = malloc(file_size + 1);
  if (config_data == NULL) {
    fclose(file);
    free(config_path);
    return;
  }

  size_t read_size = fread(config_data, 1, file_size, file);
  fclose(file);
  config_data[read_size] = '\0';

  // Parse JSON config
  yyjson_doc *doc = yyjson_read(config_data, read_size, 0);
  if (doc != NULL) {
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root)) {
      // Load server_urls (array) or server_url (single string)
      yyjson_val *servers_array = yyjson_obj_get(root, "server_urls");
      if (yyjson_is_arr(servers_array)) {
        size_t server_count = yyjson_arr_size(servers_array);
        if (server_count > 0 && server_count <= 10) {
          configuration->server_count = 0;
          size_t idx, max;
          yyjson_val *server_val;
          yyjson_arr_foreach(servers_array, idx, max, server_val) {
            if (yyjson_is_str(server_val) && configuration->server_count < 10) {
              configuration->server_urls[configuration->server_count++] =
                  strdup(yyjson_get_str(server_val));
            }
          }
        }
      } else {
        // Try single server_url for backward compatibility
        yyjson_val *server = yyjson_obj_get(root, "server_url");
        if (yyjson_is_str(server)) {
          configuration->server_urls[0] = strdup(yyjson_get_str(server));
          configuration->server_count = 1;
        }
      }

      // Load timeout_ms
      yyjson_val *timeout = yyjson_obj_get(root, "timeout_ms");
      if (yyjson_is_int(timeout)) {
        configuration->timeout_ms = yyjson_get_int(timeout);
      }

      // Load fail_open
      yyjson_val *fail_open = yyjson_obj_get(root, "fail_open");
      if (yyjson_is_bool(fail_open)) {
        configuration->fail_open = yyjson_get_bool(fail_open);
      }

      // Load debug
      yyjson_val *debug = yyjson_obj_get(root, "debug");
      if (yyjson_is_bool(debug)) {
        configuration->debug = yyjson_get_bool(debug);
      }

      // Load api_key
      yyjson_val *api_key_val = yyjson_obj_get(root, "api_key");
      if (yyjson_is_str(api_key_val)) {
        configuration->api_key = strdup(yyjson_get_str(api_key_val));
      }
    }
    yyjson_doc_free(doc);
  }

  // Validate configuration values
  if (configuration->timeout_ms < 1000 || configuration->timeout_ms > 60000) {
    LOG_WARNING("Invalid timeout in config, using default");
    configuration->timeout_ms = DEFAULT_TIMEOUT_MS;
  }

  free(config_data);
  LOG_INFO("Loaded configuration from %s", config_path);
  free(config_path);
}

/*
 * Command Line Processing
 */

static void parse_command_line_arguments(int32_t argc, char *argv[],
                                         configuration_t *configuration) {
  // Parse configuration from environment variables and command-line arguments.
  // Command-line flags override environment variables to allow runtime
  // customization.
  if (argc < 1 || argv == NULL || configuration == NULL) {
    LOG_ERROR("Invalid parameters in parse_command_line_arguments");
    return;
  }

  // Check for help flag first - it should ignore all other flags
  for (int32_t i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_verbose_usage(argv[0]);
      exit(0);
    }
  }

  // Set defaults using designated initializers
  static const char *default_servers[] = {DEFAULT_SERVER_URL};
  *configuration = (configuration_t){.server_urls = default_servers,
                                     .server_count = 1,
                                     .api_key = NULL,
                                     .timeout_ms = DEFAULT_TIMEOUT_MS,
                                     .fail_open = false,
                                     .quiet = false,
                                     .debug = false,
                                     .json_output = false,
                                     .plain_output = false,
                                     .no_color = false,
                                     .no_input = false,
                                     .insecure = false};

  // Allocate dynamic array for servers
  configuration->server_urls = malloc(sizeof(char *) * 10);  // Max 10 servers
  if (configuration->server_urls == NULL) {
    LOG_ERROR("Failed to allocate memory for server URLs");
    exit(CCHD_ERROR_MEMORY);
  }
  configuration->server_urls[0] = DEFAULT_SERVER_URL;
  configuration->server_count = 1;

  // Load from config file (lowest priority)
  load_config_file(configuration);

  // Override with environment variables (medium priority)
  const char *env_server = getenv("HOOK_SERVER_URL");
  if (env_server != NULL) {
    // Replace the default with environment variable
    configuration->server_urls[0] = env_server;
    configuration->server_count = 1;
  }

  const char *env_api_key = getenv("HOOK_API_KEY");
  if (env_api_key)
    configuration->api_key = env_api_key;

  for (int32_t arg_index = 1; arg_index < argc; arg_index++) {
    if (strcmp(argv[arg_index], "--version") == 0) {
      printf("cchd %s\n", CCHD_VERSION);
      printf("Copyright (c) 2025 Sam Joyce\n");
      printf("License: MIT\n");
      printf("Built with: Zig, C11, yyjson, libcurl\n");
      exit(0);
    } else if (strcmp(argv[arg_index], "-v") == 0) {
      fprintf(stderr, "Error: -v is ambiguous\n\n");
      fprintf(stderr, "Did you mean:\n");
      fprintf(stderr, "  • --version  Show version information\n");
      fprintf(stderr, "  • --debug    Enable debug output\n");
      exit(CCHD_ERROR_INVALID_ARG);
    } else if (strcmp(argv[arg_index], "--server") == 0 &&
               arg_index + 1 < argc) {
      arg_index++;
      // Parse comma-separated servers or add single server
      const char *server_arg = argv[arg_index];
      if (strchr(server_arg, ',') != NULL) {
        // Comma-separated list of servers
        char *servers_copy = strdup(server_arg);
        if (servers_copy == NULL) {
          LOG_ERROR("Failed to allocate memory for server list");
          exit(CCHD_ERROR_MEMORY);
        }
        configuration->server_count = 0;
        char *token = strtok(servers_copy, ",");
        while (token != NULL && configuration->server_count < 10) {
          // Trim whitespace
          while (*token == ' ')
            token++;
          char *end = token + strlen(token) - 1;
          while (end > token && *end == ' ')
            *end-- = '\0';

          configuration->server_urls[configuration->server_count++] =
              strdup(token);
          token = strtok(NULL, ",");
        }
        free(servers_copy);
      } else {
        // Single server - replace the default
        configuration->server_urls[0] = server_arg;
        configuration->server_count = 1;
      }
    } else if (strcmp(argv[arg_index], "--timeout") == 0 &&
               arg_index + 1 < argc) {
      int64_t timeout_value = atol(argv[++arg_index]);
      if (timeout_value <= 0) {
        LOG_ERROR("Invalid timeout value, using default %d ms",
                  DEFAULT_TIMEOUT_MS);
        configuration->timeout_ms = DEFAULT_TIMEOUT_MS;
      } else {
        configuration->timeout_ms = timeout_value;
      }
    } else if (strcmp(argv[arg_index], "--fail-open") == 0) {
      configuration->fail_open = true;
    } else if (strcmp(argv[arg_index], "-q") == 0 ||
               strcmp(argv[arg_index], "--quiet") == 0) {
      configuration->quiet = true;
    } else if (strcmp(argv[arg_index], "-d") == 0 ||
               strcmp(argv[arg_index], "--debug") == 0) {
      configuration->debug = true;
    } else if (strcmp(argv[arg_index], "--json") == 0) {
      configuration->json_output = true;
    } else if (strcmp(argv[arg_index], "--plain") == 0) {
      configuration->plain_output = true;
    } else if (strcmp(argv[arg_index], "--no-color") == 0) {
      configuration->no_color = true;
    } else if (strcmp(argv[arg_index], "--no-input") == 0) {
      configuration->no_input = true;
    } else if (strcmp(argv[arg_index], "--api-key") == 0 &&
               arg_index + 1 < argc) {
      configuration->api_key = argv[++arg_index];
    } else if (strcmp(argv[arg_index], "--insecure") == 0) {
      configuration->insecure = true;
    } else {
      fprintf(stderr, "Error: Unknown option '%s'\n\n", argv[arg_index]);
      fprintf(stderr, "Run '%s --help' for usage information\n", argv[0]);
      exit(CCHD_ERROR_INVALID_ARG);
    }
  }
}

/*
 * Main Entry Point Helper Functions
 */

static void initialize_cchd(int32_t argc, char *argv[],
                            configuration_t *config) {
  if (isatty(STDIN_FILENO) && argc == 1) {
    print_concise_help(argv[0]);
    exit(0);
  }

  parse_command_line_arguments(argc, argv, config);

  // Validate all server URLs
  bool has_valid_server = false;
  for (size_t i = 0; i < config->server_count; i++) {
    if (validate_server_url(config->server_urls[i], config)) {
      has_valid_server = true;
    } else {
      // Remove invalid URL by shifting remaining URLs
      for (size_t j = i; j < config->server_count - 1; j++) {
        config->server_urls[j] = config->server_urls[j + 1];
      }
      config->server_count--;
      i--;  // Re-check current position
    }
  }

  if (!has_valid_server || config->server_count == 0) {
    if (!config->quiet && !config->json_output) {
      fprintf(stderr, "Error: No valid server URLs configured\n");
      fprintf(stderr, "Use --server to specify a server URL\n");
    }
    curl_global_cleanup();
    exit(CCHD_ERROR_INVALID_ARG);
  }

  if (config->debug) {
    setenv("CCHD_LOG_LEVEL", "DEBUG", 1);
    update_log_level();
    LOG_DEBUG("Debug mode enabled");
    LOG_DEBUG("configuration_t:");
    for (size_t i = 0; i < config->server_count; i++) {
      LOG_DEBUG("  server_url[%zu]: %s", i, config->server_urls[i]);
    }
    LOG_DEBUG("  timeout_ms: %lld", (long long)config->timeout_ms);
    LOG_DEBUG("  fail_open: %s", config->fail_open ? "true" : "false");
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  // Pre-initialize global curl handle for connection reuse
  get_global_curl_handle();

  if (isatty(STDIN_FILENO)) {
    print_concise_help(argv[0]);
    curl_global_cleanup();
    exit(0);
  }
}

static char *read_and_validate_input(const configuration_t *config,
                                     char *program_name) {
  // Use synchronous I/O for now to fix the issue
  char *input_json_string = read_input_from_stdin();
  if (input_json_string == NULL) {
    if (!config->quiet) {
      const char *red = use_colors(config) ? COLOR_RED : "";
      const char *yellow = use_colors(config) ? COLOR_YELLOW : "";
      const char *reset = use_colors(config) ? COLOR_RESET : "";

      fprintf(stderr, "%sError: Failed to read input from stdin%s\n\n", red,
              reset);
      fprintf(stderr, "Possible causes:\n");
      fprintf(stderr, "  • No data was piped to the command\n");
      fprintf(stderr, "  • Input exceeds maximum size (%d KB)\n",
              INPUT_MAX_SIZE / 1024);
      fprintf(stderr, "  • Read error occurred\n\n");
      fprintf(stderr, "Example usage:\n");
      fprintf(stderr,
              "  %secho '{\"hook_event_name\":\"PreToolUse\",...}' | %s%s\n",
              yellow, program_name, reset);
    }
    curl_global_cleanup();
    exit(CCHD_ERROR_IO);
  }
  return input_json_string;
}

static char *transform_input_json(const char *input_json_string,
                                  const configuration_t *config,
                                  char *program_name,
                                  size_t input_json_capacity) {
  char *protocol_json_string =
      process_input_to_protocol(input_json_string, config);
  if (protocol_json_string == NULL) {
    if (!config->quiet) {
      const char *red = use_colors(config) ? COLOR_RED : "";
      const char *reset = use_colors(config) ? COLOR_RESET : "";

      fprintf(stderr, "%sError: Invalid JSON input%s\n\n", red, reset);
      fprintf(stderr, "The input JSON is missing required fields:\n");
      fprintf(stderr,
              "  • hook_event_name (e.g., \"PreToolUse\", \"PostToolUse\")\n");
      fprintf(stderr, "  • session_id\n\n");
      fprintf(stderr, "Run '%s --help' for more information\n", program_name);
    }
    secure_free((void *)input_json_string, input_json_capacity);
    curl_global_cleanup();
    exit(CCHD_ERROR_INVALID_HOOK);
  }
  return protocol_json_string;
}

static int32_t process_request_and_response(const configuration_t *config,
                                            const char *protocol_json_string,
                                            char **modified_output_json,
                                            bool *suppress_output,
                                            char *program_name) {
  response_buffer_t server_response = {.data = NULL, .size = 0, .capacity = 0};
  int32_t server_http_status = send_request_to_server(
      config, protocol_json_string, &server_response, program_name);

  int32_t program_exit_code = 0;

  if (server_http_status == 200 && server_response.data != NULL) {
    program_exit_code =
        process_server_response(server_response.data, modified_output_json,
                                config, suppress_output, server_http_status);
  } else if (!config->fail_open) {
    if (!config->quiet) {
      fprintf(stderr, "Error: Server unavailable (fail-closed mode)\n\n");
      fprintf(stderr, "The operation was blocked because the server");
      if (config->server_count > 1) {
        fprintf(stderr, "s are not responding:\n");
        for (size_t i = 0; i < config->server_count; i++) {
          fprintf(stderr, "  • %s\n", config->server_urls[i]);
        }
        fprintf(stderr, "\n");
      } else {
        fprintf(stderr, " at\n%s is not responding.\n\n",
                config->server_urls[0]);
      }
      fprintf(stderr, "To allow operations when server is down:\n");
      fprintf(stderr, "  • Remove the --fail-closed flag\n");
      fprintf(stderr, "  • Or fix the server connection\n");
    }
    // Map negative error codes from send_request_to_server
    if (server_http_status < 0) {
      program_exit_code =
          -server_http_status;  // Convert back to positive error code
    } else {
      program_exit_code =
          CCHD_ERROR_BLOCKED;  // Generic blocked for fail-closed
    }
    *suppress_output = true;
  }

  if (server_response.data != NULL) {
    secure_free(server_response.data, server_response.capacity);
  }

  return program_exit_code;
}

static void cleanup_resources(char *input_json_string,
                              size_t input_json_capacity,
                              char *modified_output_json) {
  secure_free(input_json_string, input_json_capacity);
  if (modified_output_json != NULL) {
    secure_free(modified_output_json, strlen(modified_output_json) + 1);
  }

  // Cleanup global curl handle
  pthread_mutex_lock(&g_curl_mutex);
  if (g_curl_handle != NULL) {
    curl_easy_cleanup(g_curl_handle);
    g_curl_handle = NULL;
  }
  pthread_mutex_unlock(&g_curl_mutex);

  curl_global_cleanup();
}

/*
 * Main Entry Point
 */

int main(int32_t argc, char *argv[]) {
#if __STDC_VERSION__ >= 201112L
  static _Thread_local int thread_id = 0;
  (void)thread_id;  // Suppress unused variable warning
#endif
  // TODO: Add mutex for multi-threaded use if expanded beyond single-process

  // Start performance timing for diagnostics.
  struct timespec start_time, end_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);

  // Check for subcommands first
  if (argc >= 2 && strcmp(argv[1], "init") == 0) {
    // Handle init command
    cchd_error err = cchd_handle_init(argc, argv);
    return err;
  }

  // Initialize configuration and curl
  configuration_t configuration;
  initialize_cchd(argc, argv, &configuration);

  // Read and validate input
  char *input_json_string = read_and_validate_input(&configuration, argv[0]);
  size_t input_json_len = strlen(input_json_string);
  size_t input_json_capacity = input_json_len + 1;

  // Transform input to protocol format
  char *protocol_json_string = transform_input_json(
      input_json_string, &configuration, argv[0], input_json_capacity);
  size_t protocol_json_len = strlen(protocol_json_string);

  // Process request and response
  char *modified_output_json = NULL;
  bool suppress_output = false;
  int32_t program_exit_code = process_request_and_response(
      &configuration, protocol_json_string, &modified_output_json,
      &suppress_output, argv[0]);
  secure_free(protocol_json_string, protocol_json_len + 1);

  // Handle output
  handle_output(suppress_output, modified_output_json, input_json_string,
                &configuration, program_exit_code);

  // Cleanup resources
  cleanup_resources(input_json_string, input_json_capacity,
                    modified_output_json);

  // Calculate total processing time for performance monitoring.
  clock_gettime(CLOCK_MONOTONIC, &end_time);
  int64_t elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 +
                       (end_time.tv_nsec - start_time.tv_nsec) / 1000000;
  LOG_INFO("Request processed in %ld ms with exit code %d", (long)elapsed_ms,
           program_exit_code);
  return program_exit_code;
}