/*
 * Input handling implementation.
 */

#include "input.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../utils/logging.h"
#include "../utils/memory.h"

// Compile-time assertions
static_assert(INPUT_MAX_SIZE >= 512 * 1024, "Input max size too small");
static_assert(INPUT_BUFFER_INITIAL_SIZE >= 8192,
              "Initial input buffer too small");

char *cchd_read_input_from_stdin(void) {
  if (stdin == NULL) {
    LOG_ERROR("stdin is NULL");
    return nullptr;
  }

  // Optimize initial allocation by checking if stdin is a regular file
  size_t capacity = INPUT_BUFFER_INITIAL_SIZE;
  struct stat st;
  if (fstat(fileno(stdin), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
    capacity = st.st_size < INPUT_MAX_SIZE ? st.st_size + 1 : INPUT_MAX_SIZE;
  }

  if (capacity == 0 || capacity > INPUT_MAX_SIZE) {
    LOG_ERROR("Invalid initial buffer capacity");
    return nullptr;
  }

  char *buffer = cchd_secure_malloc(capacity);
  if (buffer == NULL) {
    errno = ENOMEM;
    return nullptr;
  }

  size_t total_size = 0;
  while (1) {
    size_t remaining_capacity = capacity - total_size;
    if (remaining_capacity < INPUT_BUFFER_READ_CHUNK_SIZE) {
      if (capacity >= INPUT_MAX_SIZE) {
        LOG_ERROR("Input exceeds maximum size limit (%d bytes)",
                  INPUT_MAX_SIZE);
        cchd_secure_free(buffer, capacity);
        errno = E2BIG;
        return nullptr;
      }
      size_t new_capacity = capacity * 2;
      if (new_capacity > INPUT_MAX_SIZE) {
        new_capacity = INPUT_MAX_SIZE;
      }
      char *new_buffer = cchd_secure_realloc(buffer, capacity, new_capacity);
      if (new_buffer == NULL) {
        cchd_secure_free(buffer, capacity);
        errno = ENOMEM;
        return NULL;
      }
      buffer = new_buffer;
      capacity = new_capacity;
      remaining_capacity = capacity - total_size;
    }

    size_t bytes_to_read = remaining_capacity < INPUT_BUFFER_READ_CHUNK_SIZE
                               ? remaining_capacity
                               : INPUT_BUFFER_READ_CHUNK_SIZE;

    size_t bytes_read = fread(buffer + total_size, 1, bytes_to_read, stdin);
    total_size += bytes_read;

    if (bytes_read < bytes_to_read) {
      if (ferror(stdin)) {
        cchd_secure_free(buffer, capacity);
        return nullptr;
      }
      break;
    }
  }

  buffer[total_size] = '\0';
  if (strlen(buffer) != total_size || total_size > INPUT_MAX_SIZE) {
    LOG_ERROR("Buffer size validation failed");
    cchd_secure_free(buffer, capacity);
    return NULL;
  }

  // Shrink buffer to exact size
  size_t exact_size = total_size + 1;
  if (capacity > exact_size) {
    char *shrunk = cchd_secure_realloc(buffer, capacity, exact_size);
    if (shrunk) {
      buffer = shrunk;
      capacity = exact_size;
    }
  }

  return buffer;
}

#if __STDC_VERSION__ >= 202311L
static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

char *cchd_read_input_from_stdin_async(void) {
  if (stdin == NULL) {
    LOG_ERROR("stdin is NULL");
    return nullptr;
  }

  int stdin_fd = fileno(stdin);

  // Check if stdin is a regular file (not async-capable)
  struct stat st;
  if (fstat(stdin_fd, &st) == 0 && S_ISREG(st.st_mode)) {
    return cchd_read_input_from_stdin();
  }

  // Set stdin to non-blocking mode
  int original_flags = fcntl(stdin_fd, F_GETFL, 0);
  if (original_flags == -1 || set_nonblocking(stdin_fd) == -1) {
    LOG_WARNING("Failed to set non-blocking mode, falling back to sync read");
    return cchd_read_input_from_stdin();
  }

  size_t capacity = INPUT_BUFFER_INITIAL_SIZE;
  char *buffer = cchd_secure_malloc(capacity);
  if (buffer == NULL) {
    fcntl(stdin_fd, F_SETFL, original_flags);
    errno = ENOMEM;
    return nullptr;
  }

  size_t total_size = 0;
  struct pollfd pfd = {.fd = stdin_fd, .events = POLLIN};

  while (1) {
    int poll_result = poll(&pfd, 1, 100);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG_ERROR("poll() failed: %s", strerror(errno));
      cchd_secure_free(buffer, capacity);
      fcntl(stdin_fd, F_SETFL, original_flags);
      return nullptr;
    }

    if (poll_result == 0) {
      continue;
    }

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      if (total_size > 0) {
        break;
      }
      cchd_secure_free(buffer, capacity);
      fcntl(stdin_fd, F_SETFL, original_flags);
      return nullptr;
    }

    if (pfd.revents & POLLIN) {
      size_t remaining_capacity = capacity - total_size;
      if (remaining_capacity < INPUT_BUFFER_READ_CHUNK_SIZE) {
        if (capacity >= INPUT_MAX_SIZE) {
          LOG_ERROR("Input exceeds maximum size limit (%d bytes)",
                    INPUT_MAX_SIZE);
          cchd_secure_free(buffer, capacity);
          fcntl(stdin_fd, F_SETFL, original_flags);
          errno = E2BIG;
          return nullptr;
        }
        size_t new_capacity = capacity * 2;
        if (new_capacity > INPUT_MAX_SIZE) {
          new_capacity = INPUT_MAX_SIZE;
        }
        char *new_buffer = cchd_secure_realloc(buffer, capacity, new_capacity);
        if (new_buffer == NULL) {
          cchd_secure_free(buffer, capacity);
          fcntl(stdin_fd, F_SETFL, original_flags);
          errno = ENOMEM;
          return nullptr;
        }
        buffer = new_buffer;
        capacity = new_capacity;
        remaining_capacity = capacity - total_size;
      }

      size_t bytes_to_read = remaining_capacity < INPUT_BUFFER_READ_CHUNK_SIZE
                                 ? remaining_capacity
                                 : INPUT_BUFFER_READ_CHUNK_SIZE;

      ssize_t bytes_read = read(stdin_fd, buffer + total_size, bytes_to_read);
      if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        LOG_ERROR("read() failed: %s", strerror(errno));
        cchd_secure_free(buffer, capacity);
        fcntl(stdin_fd, F_SETFL, original_flags);
        return nullptr;
      }

      if (bytes_read == 0) {
        break;
      }

      total_size += bytes_read;
    }
  }

  fcntl(stdin_fd, F_SETFL, original_flags);

  buffer[total_size] = '\0';
  if (strlen(buffer) != total_size || total_size > INPUT_MAX_SIZE) {
    LOG_ERROR("Buffer size validation failed");
    cchd_secure_free(buffer, capacity);
    return NULL;
  }

  // Shrink buffer to exact size
  size_t exact_size = total_size + 1;
  if (capacity > exact_size) {
    char *shrunk = cchd_secure_realloc(buffer, capacity, exact_size);
    if (shrunk) {
      buffer = shrunk;
      capacity = exact_size;
    }
  }

  return buffer;
}
#endif