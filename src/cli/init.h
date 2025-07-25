/*
 * Template initialization command for CCHD.
 *
 * Provides functionality to download and set up hook server templates. This reduces
 * the barrier to entry for new users by automating the setup process that would
 * otherwise require manually finding templates, copying them to the right location,
 * and configuring settings.json. By providing ready-to-run templates in multiple
 * languages, we accommodate developers with different language preferences while
 * ensuring consistent hook behavior across implementations.
 */

#pragma once

#include "../core/error.h"

// Handle the init command to set up a template. This function manages the entire
// initialization workflow: parsing arguments, downloading templates from GitHub,
// creating necessary directories, and updating configuration. We handle this as
// a separate command rather than a flag to maintain clear separation between
// hook dispatching (the main function) and project setup (this auxiliary function).
// Returns CCHD_SUCCESS on success, or appropriate error code for various failure
// modes (network errors, file conflicts, permission issues).
CCHD_NODISCARD cchd_error cchd_handle_init(int argc, char *argv[]);