/*
 * Secure memory management for CCHD.
 * 
 * Provides memory functions that prevent sensitive data from persisting in memory.
 * These functions ensure API keys, tokens, and user data are properly cleared
 * to prevent exposure through memory dumps, swap files, or use-after-free bugs.
 * Critical for maintaining security when handling authentication credentials.
 */

#pragma once

#include <stddef.h>

#include "../core/types.h"

// Securely zero memory to prevent sensitive data recovery.
// Uses volatile pointer to prevent compiler optimization from removing the zeroing.
// Standard memset() might be optimized away if compiler thinks memory is unused,
// leaving passwords and keys recoverable in memory dumps.
void cchd_secure_zero(void *ptr, size_t len);

// Secure memory allocation with automatic zeroing.
// Allocates memory and immediately zeros it to prevent information leakage
// from previous allocations. Use for any buffer that will hold sensitive data.
void *cchd_secure_malloc(size_t size);

// Secure memory reallocation that zeros old memory before freeing.
// When growing buffers containing sensitive data, this ensures the old
// smaller buffer is cleared before being returned to the heap, preventing
// data fragments from persisting in freed memory.
void *cchd_secure_realloc(void *ptr, size_t old_size, size_t new_size);

// Secure memory free that zeros before deallocation.
// The size parameter enables complete zeroing of the memory block.
// This prevents sensitive data from persisting in freed heap memory
// where it could be allocated to other parts of the program.
void cchd_secure_free(void *ptr, size_t size);

// Secure string duplication for sensitive strings like API keys.
// Duplicates string into secure memory that will be properly cleared on free.
// Use instead of strdup() for any authentication credentials or user secrets
// to maintain security throughout the string's lifecycle.
CCHD_NODISCARD char *cchd_secure_strdup(const char *s);