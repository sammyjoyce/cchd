/*
 * Logging implementation for CCHD.
 * 
 * Provides a lightweight, level-based logging system that integrates with Claude Code's
 * output handling. We avoid heavy logging frameworks to keep the tool fast and minimize
 * dependencies. The design prioritizes clear error reporting for hook developers while
 * maintaining performance for production use.
 */

#include "logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "../core/types.h"

// Global log level defaults to ERROR to minimize output in production environments.
// Hook developers can increase verbosity via CCHD_LOG_LEVEL when debugging issues.
// We track initialization state to ensure environment variables are read exactly once,
// preventing inconsistent behavior if the environment changes during execution.
static cchd_log_level g_log_level = LOG_LEVEL_ERROR;
static bool g_log_initialized = false;

void cchd_log_init(void) {
  // Initialize only once to ensure consistent logging behavior throughout the
  // program's lifetime. Reading environment variables multiple times could lead
  // to confusing behavior if they change mid-execution (e.g., in subprocesses).
  if (!g_log_initialized) {
    cchd_log_update_level();
    g_log_initialized = true;
  }
}

void cchd_log_update_level(void) {
  // Read log level from environment to allow runtime configuration without recompilation.
  // This is essential for hook developers who need to debug issues in production
  // environments where they can't modify the binary. We use string comparison rather
  // than numeric levels for clarity in configuration (CCHD_LOG_LEVEL=DEBUG is clearer
  // than CCHD_LOG_LEVEL=3).
  const char *level = getenv("CCHD_LOG_LEVEL");
  if (level == nullptr) {
    g_log_level = LOG_LEVEL_ERROR;
  } else if (strcmp(level, "WARNING") == 0) {
    g_log_level = LOG_LEVEL_WARNING;
  } else if (strcmp(level, "INFO") == 0) {
    g_log_level = LOG_LEVEL_INFO;
  } else if (strcmp(level, "DEBUG") == 0) {
    g_log_level = LOG_LEVEL_DEBUG;
  } else {
    // Default to ERROR for invalid values rather than failing. This ensures hooks
    // continue to work even with misconfigured environments, following the principle
    // of graceful degradation.
    g_log_level = LOG_LEVEL_ERROR;
  }
}

cchd_log_level cchd_log_get_level(void) {
  if (!g_log_initialized) {
    cchd_log_init();
  }
  return g_log_level;
}

void cchd_log_set_level(cchd_log_level level) {
  g_log_level = level;
  g_log_initialized = true;
}

static const char *log_level_to_string(cchd_log_level level) {
  switch (level) {
  case LOG_LEVEL_ERROR:
    return "ERROR";
  case LOG_LEVEL_WARNING:
    return "WARNING";
  case LOG_LEVEL_INFO:
    return "INFO";
  case LOG_LEVEL_DEBUG:
    return "DEBUG";
  default:
    return "UNKNOWN";
  }
}

void cchd_log_with_location(cchd_log_level level, const char *file, int line,
                            const char *fmt, ...) {
  // Lazy initialization allows logging to work immediately without requiring explicit
  // setup in main(). This is crucial for error reporting during early startup failures.
  if (!g_log_initialized) {
    cchd_log_init();
  }

  // Early return for filtered messages avoids formatting overhead. Since debug logging
  // can be verbose, skipping disabled messages significantly improves performance in
  // production where only errors are logged.
  if (level > g_log_level) {
    return;
  }

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  char time_buf[32];
  struct tm tm_info_buf;
  localtime_r(&tv.tv_sec, &tm_info_buf);
  strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info_buf);

  fprintf(stderr, "[%s.%03ld] [%s] %s:%d: ", time_buf,
          (long)(tv.tv_usec / 1000), log_level_to_string(level), file, line);

  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);

  fprintf(stderr, "\n");
}