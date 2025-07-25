/*
 * CloudEvents transformation implementation.
 */

#include "cloudevents.h"

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../utils/logging.h"
#include "../utils/memory.h"
#include "json.h"

// Compile-time validations ensure our buffers can handle worst-case scenarios.
// These assertions catch configuration errors at compile time rather than runtime,
// preventing buffer overflows and data corruption in production.
static_assert(TIMESTAMP_BUFFER_SIZE >= 25,
              "Timestamp buffer too small for RFC3339: need 25 chars for 'YYYY-MM-DDTHH:MM:SS.sssZ'");
static_assert(ID_BUFFER_SIZE >= 32,
              "ID buffer too small: need space for 64-bit hex seconds + 64-bit hex nanoseconds");
static_assert(TYPE_BUFFER_SIZE >= 128,
              "Type buffer too small: CloudEvents types can be lengthy with reverse-DNS format");

static yyjson_mut_val *build_data_object(yyjson_mut_doc *output_doc,
                                         yyjson_val *input_root) {
  if (output_doc == nullptr || input_root == nullptr) {
    LOG_ERROR("Invalid parameters in build_data_object");
    return nullptr;
  }

  // Create a deep copy of the entire input root object. We need a deep copy because
  // the CloudEvents format requires the original hook data to be nested under a 'data'
  // field, while we add CloudEvents metadata at the root level. Without a deep copy,
  // we'd corrupt the original input or create invalid references across documents.
  yyjson_mut_val *data_copy = yyjson_val_mut_copy(output_doc, input_root);
  if (data_copy == NULL) {
    return nullptr;
  }

  return data_copy;
}

static bool add_required_cloudevents_attributes(yyjson_mut_doc *output_doc,
                                                yyjson_mut_val *output_root,
                                                yyjson_val *input_root) {
  if (output_doc == NULL || output_root == NULL || input_root == NULL ||
      !yyjson_is_obj(input_root)) {
    LOG_ERROR("Invalid parameters in add_required_cloudevents_attributes");
    return false;
  }

  // CloudEvents specification version. We use "1.0" because it's the current stable
  // version that's widely supported by event processing systems. This version field
  // allows consumers to handle format changes gracefully if the spec evolves.
  if (!yyjson_mut_obj_add_strcpy(output_doc, output_root, "specversion",
                                 "1.0")) {
    return false;
  }

  // Event type using CloudEvents reverse-DNS format. We use reverse-DNS naming
  // (com.claudecode.hook.*) to ensure globally unique event types without requiring
  // a central registry. This format also makes it clear these events originate from
  // Claude Code hooks, helping with event routing and filtering in larger systems.
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

  // Source URI identifies where events originate within the Claude Code system.
  // We use a URI path format rather than a full URL because hooks run locally
  // and don't have a stable network address. The '/claude-code/hooks' path
  // clearly indicates these events come from the hooks subsystem, enabling
  // consumers to filter or route events based on their source.
  if (!yyjson_mut_obj_add_strcpy(output_doc, output_root, "source",
                                 "/claude-code/hooks")) {
    return false;
  }

  // Generate unique event identifier. CloudEvents requires each event to have a
  // globally unique ID for deduplication and tracing. We combine high-resolution
  // timestamp (seconds + nanoseconds) in hex format because:
  // 1. It's guaranteed unique within this process (nanosecond precision)
  // 2. It's chronologically sortable for debugging
  // 3. It's compact and URL-safe (hex encoding)
  char id_buffer[ID_BUFFER_SIZE] = {0};
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    // Fallback to lower resolution time. If high-resolution clock fails (rare but
    // possible on some systems), we use regular time() plus an atomic counter.
    // This maintains uniqueness at the cost of less precise timestamps.
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

static bool add_optional_cloudevents_attributes(yyjson_mut_doc *output_doc,
                                                yyjson_mut_val *output_root,
                                                yyjson_val *input_root) {
  if (output_doc == NULL || output_root == NULL || input_root == NULL ||
      !yyjson_is_obj(input_root)) {
    LOG_ERROR("Invalid parameters in add_optional_cloudevents_attributes");
    return false;
  }

  // Event timestamp
  char *timestamp = cchd_generate_rfc3339_timestamp();
  if (timestamp != NULL) {
    if (!yyjson_mut_obj_add_strcpy(output_doc, output_root, "time",
                                   timestamp)) {
      cchd_secure_free(timestamp, TIMESTAMP_BUFFER_SIZE);
      return false;
    }
    cchd_secure_free(timestamp, TIMESTAMP_BUFFER_SIZE);
  }

  // Content type
  if (!yyjson_mut_obj_add_strcpy(output_doc, output_root, "datacontenttype",
                                 "application/json")) {
    return false;
  }

  // CloudEvents extensions
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

yyjson_mut_doc *cchd_transform_to_cloudevents(yyjson_doc *input_doc) {
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

  // Build CloudEvents structure
  if (!add_required_cloudevents_attributes(output_doc, output_root,
                                           input_root)) {
    yyjson_mut_doc_free(output_doc);
    return NULL;
  }

  if (!add_optional_cloudevents_attributes(output_doc, output_root,
                                           input_root)) {
    yyjson_mut_doc_free(output_doc);
    return NULL;
  }

  // Embed original hook data
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