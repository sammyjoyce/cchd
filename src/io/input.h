/*
 * Input handling for CCHD.
 * 
 * Manages reading hook data from stdin with proper buffering and size limits.
 * We read from stdin to support pipeline integration where other tools generate
 * hook events. The module handles both blocking and async reads to accommodate
 * different integration patterns.
 */

#pragma once

#include "../core/types.h"

// Read input from stdin with automatic buffer growth.
// Returns allocated string that must be freed by caller, or NULL on error.
// Enforces size limits to prevent memory exhaustion from malicious input.
// This blocking read is suitable for most pipeline use cases.
CCHD_NODISCARD char *cchd_read_input_from_stdin(void);

// Read input from stdin asynchronously using C23 thread features.
// Enables timeout support and graceful cancellation for interactive use.
// Falls back to blocking read on platforms without C23 support to maintain
// compatibility while leveraging modern features where available.
#if __STDC_VERSION__ >= 202311L
CCHD_NODISCARD char *cchd_read_input_from_stdin_async(void);
#endif