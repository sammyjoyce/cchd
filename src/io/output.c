/*
 * Output handling implementation.
 */

#include "output.h"

#include <stdio.h>

#include "../core/config.h"
#include "../utils/logging.h"

void cchd_handle_output(bool suppress_output, const char *modified_output_json,
                        const char *input_json_string,
                        const cchd_config_t *config, int32_t exit_code) {
  if (input_json_string == nullptr || config == nullptr) {
    LOG_ERROR("Invalid parameters in handle_output");
    return;
  }

  if (!suppress_output) {
    if (cchd_config_is_json_output(config)) {
      // Output structured JSON response
      printf("{\"status\":\"%s\",\"exit_code\":%d,\"modified\":%s",
             exit_code == 0 ? "allowed"
                            : (exit_code == 1 ? "blocked" : "ask_user"),
             exit_code, modified_output_json ? "true" : "false");
      if (modified_output_json) {
        printf(",\"data\":%s", modified_output_json);
      }
      printf("}\n");
    } else if (cchd_config_is_plain_output(config)) {
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