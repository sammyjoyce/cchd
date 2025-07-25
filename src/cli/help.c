/*
 * Help text display implementation.
 */

#include "help.h"

#include <stdio.h>
#include <string.h>

#include "../core/types.h"
#include "../utils/colors.h"
#include "../utils/logging.h"

void cchd_print_concise_help(const char *program_name) {
  if (program_name == nullptr || strlen(program_name) == 0) {
    LOG_ERROR("Invalid program name");
    program_name = "cchd";
  }

  printf("cchd - Claude Code hooks dispatcher [version %s]\n\n", CCHD_VERSION);

  printf("Usage: %s [options]\n", program_name);
  printf("       %s init <template> [filename]\n\n", program_name);

  printf("cchd processes Claude Code hook events through custom servers to\n");
  printf("allow, block, or modify operations before they execute.\n\n");

  printf("Commands:\n");
  printf("  init      Initialize a new hook server from a template\n\n");

  printf("Example:\n");
  printf(
      "  $ echo '{\"hook_event_name\": \"PreToolUse\", \"session_id\": "
      "\"abc123\"}' | %s\n",
      program_name);
  printf("  Connecting to http://localhost:8080/hook...\n");
  printf(
      "  {\"hook_event_name\": \"PreToolUse\", \"session_id\": "
      "\"abc123\"}\n\n");

  printf("For more options, use %s --help\n", program_name);
}

void cchd_print_verbose_usage(const char *program_name) {
  if (program_name == nullptr || strlen(program_name) == 0) {
    LOG_ERROR("Invalid program name");
    program_name = "cchd";
  }

  const char *bold = cchd_use_colors(nullptr) ? COLOR_BOLD : "";
  const char *reset = cchd_use_colors(NULL) ? COLOR_RESET : "";

  printf("cchd - commandline Claude Code hooks dispatcher [version %s]\n\n",
         CCHD_VERSION);

  printf("%sUSAGE%s\n", bold, reset);
  printf("  %s [options]\n", program_name);
  printf("  %s init <template> [filename]\n\n", program_name);

  printf("%sDESCRIPTION%s\n", bold, reset);
  printf("  Processes Claude Code hook events through custom servers.\n\n");

  printf("%sCOMMANDS%s\n", bold, reset);
  printf(
      "  init                  Initialize a new hook server from a template\n");
  printf("                        Use '%s init --help' for more info\n\n",
         program_name);

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
  printf("  --plain               Plain output for scripts\n");
  printf("  --no-color            Disable colors\n");
  printf("  --version             Show version information\n\n");

  printf("%sQUICK START%s\n", bold, reset);
  printf("  1. Initialize a template server:\n");
  printf("     $ %s init python\n", program_name);
  printf("     $ %s init typescript\n", program_name);
  printf("     $ %s init go\n\n", program_name);

  printf("  2. Start the server:\n");
  printf("     $ uv run %s      # Python\n", CCHD_TEMPLATE_PYTHON);
  printf("     $ bun %s     # TypeScript\n", CCHD_TEMPLATE_TYPESCRIPT);
  printf("     $ go run %s          # Go\n\n", CCHD_TEMPLATE_GO);

  printf("  3. Configure Claude to use http://localhost:8080/hook\n\n");

  printf("  4. Test: echo '{\"hook_event_name\":\"PreToolUse\"}' | %s\n\n",
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