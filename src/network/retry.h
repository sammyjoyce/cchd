/*
 * Retry logic for CCHD.
 * 
 * Implements intelligent retry delays that adapt based on error types to balance
 * reliability with server load. Network errors get quick retries assuming transient
 * issues, while server errors use exponential backoff to avoid overwhelming
 * struggling servers. This approach maximizes success rates while being a good citizen.
 */

#pragma once

#include <stdint.h>

#include "../core/types.h"

// Calculate adaptive retry delay based on error type and attempt number.
// Returns 0 for immediate retry (network errors), exponentially increasing delays
// for server errors, or -1 when retry limit exceeded. This strategy prevents
// both premature failure and excessive server load during outages.
CCHD_NODISCARD int32_t cchd_calculate_retry_delay(int32_t http_status,
                                                  int32_t base_delay_ms,
                                                  int32_t attempt);