/*
 * Command-line argument parsing implementation.
 */

#include "args.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/config.h"
#include "../utils/logging.h"
#include "help.h"
#include "init.h"

cchd_error cchd_parse_args(int argc, char *argv[], cchd_config_t *config) {
  CHECK_NULL(argv, CCHD_ERROR_INVALID_ARG);
  CHECK_NULL(config, CCHD_ERROR_INVALID_ARG);

  // Check for init command first
  if (argc >= 2 && strcmp(argv[1], "init") == 0) {
    cchd_error err = cchd_handle_init(argc, argv);
    exit(err == CCHD_SUCCESS ? 0 : 1);
  }

  // Check for help flag first
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      cchd_print_verbose_usage(argv[0]);
      exit(0);
    }
  }

  // Check for version flag
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--version") == 0) {
      printf("cchd %s\n", CCHD_VERSION);
      printf("Copyright (c) 2025 Sam Joyce\n");
      printf("License: MIT\n");
      printf("Built with: Zig, C11, yyjson, libcurl\n");
      exit(0);
    }
  }

  // Check for ambiguous -v flag
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0) {
      fprintf(stderr, "Error: -v is ambiguous\n\n");
      fprintf(stderr, "Did you mean:\n");
      fprintf(stderr, "  • --version  Show version information\n");
      fprintf(stderr, "  • --debug    Enable debug output\n");
      return CCHD_ERROR_INVALID_ARG;
    }
  }

  // Parse remaining arguments through config module
  cchd_error err = cchd_config_load_args(config, argc, argv);
  if (err != CCHD_SUCCESS) {
    return err;
  }

  // Check for unknown options
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      // Skip known options and their arguments
      if (strcmp(argv[i], "--server") == 0 ||
          strcmp(argv[i], "--timeout") == 0 ||
          strcmp(argv[i], "--api-key") == 0) {
        i++;  // Skip the argument
        continue;
      }

      // Check if it's a known flag
      if (strcmp(argv[i], "--fail-open") != 0 && strcmp(argv[i], "-q") != 0 &&
          strcmp(argv[i], "--quiet") != 0 && strcmp(argv[i], "-d") != 0 &&
          strcmp(argv[i], "--debug") != 0 && strcmp(argv[i], "--json") != 0 &&
          strcmp(argv[i], "--plain") != 0 &&
          strcmp(argv[i], "--no-color") != 0 &&
          strcmp(argv[i], "--no-input") != 0 &&
          strcmp(argv[i], "--insecure") != 0) {
        fprintf(stderr, "Error: Unknown option '%s'\n\n", argv[i]);
        fprintf(stderr, "Run '%s --help' for usage information\n", argv[0]);
        return CCHD_ERROR_INVALID_ARG;
      }
    }
  }

  return CCHD_SUCCESS;
}