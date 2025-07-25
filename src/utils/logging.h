/*
 * Logging utilities for CCHD.
 * 
 * Provides structured logging with automatic file/line tracking for debugging.
 * Logs go to stderr to keep stdout clean for pipeline output. The logging level
 * can be controlled via CCHD_LOG_LEVEL environment variable, enabling debug output
 * without recompilation. Macros capture source location automatically.
 */

#pragma once

#include <stdarg.h>
#include <stdbool.h>

// Logging levels follow standard severity hierarchy.
// Lower values = higher severity. This ordering ensures that setting a level
// shows all messages at that severity and above, matching user expectations.
typedef enum {
  LOG_LEVEL_ERROR = 0,
  LOG_LEVEL_WARNING = 1,
  LOG_LEVEL_INFO = 2,
  LOG_LEVEL_DEBUG = 3
} cchd_log_level;

// Initialize logging system by reading environment variables.
// Sets up initial log level from CCHD_LOG_LEVEL env var. Must be called
// early in main() to ensure all startup messages respect the configured level.
void cchd_log_init(void);

// Update log level from CCHD_LOG_LEVEL environment variable.
// Allows dynamic log level changes without restart, useful for debugging
// production issues by temporarily enabling debug logging.
void cchd_log_update_level(void);

// Get current log level for conditional logic.
// Useful when expensive debug operations should only run when debugging
// is enabled, avoiding performance impact in normal operation.
cchd_log_level cchd_log_get_level(void);

// Set log level programmatically for testing or special modes.
// Overrides environment variable setting. Primarily used in test code
// to ensure consistent logging behavior regardless of environment.
void cchd_log_set_level(cchd_log_level level);

// Internal logging function captures source location for debugging.
// Don't call directly - use LOG_* macros which automatically provide
// file and line information, making log messages much more actionable.
void cchd_log_with_location(cchd_log_level level, const char *file, int line,
                            const char *fmt, ...);

// Convenience macros automatically capture source location.
// These macros make logging effortless while providing crucial debugging
// context. The ##__VA_ARGS__ syntax handles both with and without arguments.
#define LOG_ERROR(fmt, ...)                                        \
  cchd_log_with_location(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, \
                         ##__VA_ARGS__)

#define LOG_WARNING(fmt, ...)                                        \
  cchd_log_with_location(LOG_LEVEL_WARNING, __FILE__, __LINE__, fmt, \
                         ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
  cchd_log_with_location(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...)                                        \
  cchd_log_with_location(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, \
                         ##__VA_ARGS__)

// Verification macro for debug-mode sanity checks.
// Use for conditions that should always be true in correct code.
// Unlike assert(), this logs failures rather than crashing, allowing
// graceful degradation in production while alerting to logic errors.
#define VERIFY(condition)                               \
  do {                                                  \
    if (!(condition)) {                                 \
      LOG_ERROR("Verification failed: %s", #condition); \
    }                                                   \
  } while (0)

// Check pointer and return on NULL for defensive programming.
// Prevents segfaults by validating pointers at function entry.
// The macro logs the exact pointer name that was NULL, speeding debugging
// compared to generic "NULL pointer" errors.
#define CHECK_NULL(ptr, ret)                            \
  do {                                                  \
    if ((ptr) == nullptr) {                             \
      LOG_ERROR("Null pointer check failed: %s", #ptr); \
      return (ret);                                     \
    }                                                   \
  } while (0)

// Check condition and return on failure with custom error message.
// Combines validation with logging for clean error handling.
// Use when specific error context is needed beyond a simple condition check.
// The do-while(0) wrapper ensures macro works correctly in all contexts.
#define CHECK_COND(cond, ret, fmt, ...) \
  do {                                  \
    if (!(cond)) {                      \
      LOG_ERROR(fmt, ##__VA_ARGS__);    \
      return (ret);                     \
    }                                   \
  } while (0)