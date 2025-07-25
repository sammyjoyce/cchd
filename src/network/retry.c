/*
 * Retry logic implementation.
 */

#include "retry.h"

#include <stdlib.h>

#include "../core/error.h"

int32_t cchd_calculate_retry_delay(int32_t http_status, int32_t base_delay_ms,
                                   int32_t attempt) {
  int32_t delay_ms = base_delay_ms;

  if (http_status < 0) {
    // Negative values are CCHD error codes. We use negative values to distinguish
    // our internal error codes from HTTP status codes, allowing this single function
    // to handle both network-level failures and HTTP-level failures with appropriate
    // retry strategies for each type.
    int32_t error_code = -http_status;
    switch (error_code) {
    case CCHD_ERROR_CONNECTION:
    case CCHD_ERROR_DNS:
      // Connection/DNS errors: shorter initial delay because these failures are often
      // transient (network hiccup, DNS cache miss). Quick retries have a high success
      // rate without overloading the network. We use exponential backoff (2^attempt)
      // to handle persistent failures gracefully while still recovering quickly from
      // brief outages.
      delay_ms = 250 + (rand() % 250);  // 250-500ms initial with jitter
      if (attempt > 0) {
        delay_ms *= (1 << attempt);  // 2^attempt multiplier
      }
      if (delay_ms > 3000)
        delay_ms = 3000;  // Cap at 3 seconds to maintain responsiveness
      break;
    case CCHD_ERROR_TIMEOUT:
      // Timeout: longer delay as server might be overloaded. Timeouts often indicate
      // the server is processing slowly due to high load. Immediate retries would
      // worsen the situation, so we start with a full second delay. Linear backoff
      // (2x) is gentler than exponential, giving the server time to recover without
      // excessive waiting.
      delay_ms = 1000 + (rand() % 500);  // 1-1.5s initial with jitter
      if (attempt > 0) {
        delay_ms *= 2;
      }
      if (delay_ms > 5000)
        delay_ms = 5000;  // Cap at 5 seconds balances recovery time vs user experience
      break;
    default:
      // Other network errors: moderate delay for unknown failure modes. We don't know
      // the specific cause, so we use middle-ground timing that works reasonably well
      // for various scenarios (proxy errors, TLS issues, etc). The randomization helps
      // prevent synchronized retries from multiple clients (thundering herd problem).
      delay_ms = 500 + (rand() % 500);  // 500ms-1s initial with jitter
      if (attempt > 0) {
        delay_ms *= 2;
      }
      if (delay_ms > 3000)
        delay_ms = 3000;  // Cap at 3 seconds for reasonable worst-case
      break;
    }
  } else if (http_status >= 500 && http_status < 600) {
    // Server errors: longer delays because 5xx errors indicate server-side problems
    // that need time to resolve (crashes, deploys, database issues). We use aggressive
    // backoff (2+attempt multiplier) because repeated requests during server recovery
    // can delay that recovery. The 10-second cap prevents excessive waiting while
    // still giving servers breathing room.
    delay_ms = 1000 + (rand() % 500);  // 1-1.5s initial with jitter
    if (attempt > 0) {
      delay_ms *= (2 + attempt);  // More aggressive backoff for server protection
    }
    if (delay_ms > 10000)
      delay_ms = 10000;  // Cap at 10 seconds - long enough for most recoveries
  } else if (http_status == 429) {
    // Rate limiting: respect server's rate limits with substantial delays. 429 status
    // explicitly means "you're sending too many requests", so we must back off
    // significantly. Starting at 5-7 seconds shows we respect the server's limits.
    // The 30-second cap is high because rate limits often have minute-level windows,
    // and waiting 30 seconds gives us the best chance of success on the next attempt.
    delay_ms = 5000 + (rand() % 2000);  // 5-7s initial with wide jitter
    if (attempt > 0) {
      delay_ms *= 2;
    }
    if (delay_ms > 30000)
      delay_ms = 30000;  // Cap at 30 seconds - respects typical rate limit windows
  }

  return delay_ms;
}