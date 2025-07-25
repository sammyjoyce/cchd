/*
 * Terminal color handling implementation.
 */

#include "colors.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../core/config.h"

bool cchd_use_colors(const cchd_config_t *config) {
  // Command line flag takes precedence
  if (config && cchd_config_is_no_color(config)) {
    return false;
  }

  // Respect NO_COLOR environment variable
  if (getenv("NO_COLOR") != NULL) {
    return false;
  }

  // Force color if requested
  if (getenv("FORCE_COLOR") != NULL) {
    return true;
  }

  // Check if TERM is dumb
  const char *term = getenv("TERM");
  if (term != NULL && strcmp(term, "dumb") == 0) {
    return false;
  }

  // Check if output is to a terminal
  return isatty(STDERR_FILENO);
}