/*
 * Secure memory management implementation.
 * 
 * This module provides security-hardened memory functions that prevent sensitive
 * data from persisting in memory after use. Standard memory functions can leave
 * data remnants that attackers could recover through memory dumps, swap files,
 * or side-channel attacks. Our secure functions ensure complete data erasure.
 */

#include "memory.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "logging.h"

void cchd_secure_zero(void *ptr, size_t len) {
  if (ptr == nullptr || len == 0)
    return;

#if defined(__STDC_LIB_EXT1__)
  // C11 Annex K provides memset_s for secure zeroing. Unlike regular memset(),
  // memset_s is guaranteed not to be optimized away by the compiler, even if
  // the memory is about to be freed. This prevents sensitive data from lingering
  // in memory where it could be recovered by an attacker.
  memset_s(ptr, len, 0, len);
#elif defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__)
  // BSD systems provide explicit_bzero for the same security purpose as memset_s.
  // It's specifically designed to resist compiler optimizations that would skip
  // zeroing memory that appears to be unused. BSD developers created this after
  // discovering regular bzero() calls were being optimized away in security code.
  explicit_bzero(ptr, len);
#else
  // Fallback implementation using volatile pointer. The volatile qualifier forces
  // the compiler to perform every write operation, preventing optimization that
  // would skip zeroing "dead" memory. This is crucial for clearing passwords,
  // keys, and other secrets before freeing memory.
  volatile unsigned char *p = ptr;
  while (len--) {
    *p++ = 0;
  }
  // Compiler barrier to prevent dead store elimination. This assembly statement
  // acts as a memory fence, ensuring the compiler completes all memory writes
  // before proceeding. Without this, aggressive optimizers might still remove
  // our zeroing loop if they determine the memory won't be read again.
  __asm__ __volatile__("" : : "r"(ptr) : "memory");
#endif
}

void *cchd_secure_malloc(size_t size) {
  if (size == 0)
    return NULL;

  void *ptr = malloc(size);
  if (ptr == NULL) {
    return NULL;
  }

  // Attempt to lock memory to prevent swapping. When the OS swaps memory to disk,
  // sensitive data gets written to the swap file where it can persist indefinitely,
  // even after the program exits. mlock() prevents this by pinning the memory in RAM.
  // This is critical for handling passwords, API keys, and cryptographic material.
  if (mlock(ptr, size) != 0) {
    LOG_WARNING("Failed to mlock() %zu bytes. Check user limits (ulimit -l).",
                size);
  }

  // Zero the memory immediately after allocation. Freshly allocated memory often
  // contains data from previously freed allocations, which could include sensitive
  // information from our own process or even other processes. Zeroing prevents
  // accidental information leakage if the memory is read before being initialized.
  memset(ptr, 0, size);
  return ptr;
}

void *cchd_secure_realloc(void *ptr, size_t old_size, size_t new_size) {
  if (new_size == 0) {
    if (ptr != nullptr) {
      cchd_secure_zero(ptr, old_size);
      munlock(ptr, old_size);
      free(ptr);
    }
    return nullptr;
  }

  void *new_ptr = cchd_secure_malloc(new_size);
  if (new_ptr == nullptr) {
    return nullptr;
  }

  if (ptr != NULL) {
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_size);
    cchd_secure_zero(ptr, old_size);
    munlock(ptr, old_size);
    free(ptr);
  }

  return new_ptr;
}

void cchd_secure_free(void *ptr, size_t size) {
  if (ptr == nullptr)
    return;

  cchd_secure_zero(ptr, size);
  munlock(ptr, size);
  free(ptr);
}

char *cchd_secure_strdup(const char *s) {
  if (s == nullptr) {
    return nullptr;
  }
  size_t len = strlen(s);
  char *new_str = cchd_secure_malloc(len + 1);
  if (new_str == nullptr) {
    return nullptr;
  }
  memcpy(new_str, s, len + 1);
  return new_str;
}