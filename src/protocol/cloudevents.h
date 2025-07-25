/*
 * CloudEvents transformation for CCHD.
 * 
 * Converts hook events to CloudEvents format for standardized event handling.
 * CloudEvents provides a vendor-neutral specification for event data, enabling
 * interoperability between different systems. This transformation ensures hooks
 * can be processed by any CloudEvents-compliant system.
 */

#pragma once

#include <yyjson.h>

#include "../core/types.h"

// Transform input to CloudEvents format with required metadata.
// Adds CloudEvents attributes (specversion, type, source, id) while preserving
// original data. Returns new document that caller must free. This standardization
// enables reliable event routing and processing across diverse systems.
CCHD_NODISCARD yyjson_mut_doc *cchd_transform_to_cloudevents(
    yyjson_doc *input_doc);