/*
 * Terminal color handling for CCHD.
 * 
 * Provides ANSI color codes with intelligent terminal detection to enhance readability.
 * Colors are automatically disabled for non-TTY output, when NO_COLOR is set, or when
 * explicitly disabled via config. This ensures clean output in logs and pipes while
 * providing helpful visual cues in interactive terminals.
 */

#pragma once

#include <stdbool.h>

// ANSI color codes for semantic highlighting of output.
// Red for errors, green for success, yellow for warnings, blue for info.
// These standard codes work across most modern terminals and are ignored
// by terminals that don't support them, ensuring graceful degradation.
#define COLOR_RED "\033[0;31m"
#define COLOR_GREEN "\033[0;32m"
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_BLUE "\033[0;34m"
#define COLOR_BOLD "\033[1m"
#define COLOR_RESET "\033[0m"

// Forward declaration to access color preferences from config.
// This avoids circular dependencies while respecting user preferences.
typedef struct cchd_config cchd_config_t;

// Check if colors should be used based on terminal capabilities and config.
// Returns false for: non-TTY output, NO_COLOR env var, --no-color flag, or
// dumb terminals. This multi-layer check ensures colors only appear when helpful.
bool cchd_use_colors(const cchd_config_t *config);