#include <abi/readiness_set.h>
#include <clock.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/select.h>
#include <syscall.h>
#include "descriptor.h"
#include "errors.h"
#include "signal_internal.h"
#include "thread_platform.h"

#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)

struct poll_row {
  struct descriptor_poll descriptor;
  struct readiness_set_registration registration;
  uint64_t events;
  enum call_status status;
  bool registered;
};

struct poll_duration {
  uint64_t nanoseconds;
  bool infinite;
};

struct poll_signal_source {
  struct readiness_set_registration registration;
  bool registered;
};

static int fail(int error)
{
  errno = error;
  return -1;
}

static enum call_status reply_status(struct syscall_result result, size_t size)
{
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? size : 0)) {
    return CALL_IO;
  }
  return result.status;
}

static enum call_status set_register(handle_t set, handle_t target, uint64_t interests,
    uint64_t observed_generation, size_t index,
    struct readiness_set_registration *registration)
{
  struct readiness_set_register_request request = {
    .header = {PROTOCOL_READINESS_SET, READINESS_SET_REGISTER},
    .target = target, .allowed_events = interests, .active_events = interests, .tag = index,
    .observed_generation = observed_generation,
  };
  return reply_status(syscall_call(set, &request, sizeof(request), registration,
      sizeof(*registration)), sizeof(*registration));
}

static enum call_status set_remove(handle_t set, struct readiness_set_registration registration)
{
  struct readiness_set_remove_request request = {
    .header = {PROTOCOL_READINESS_SET, READINESS_SET_REMOVE}, .registration = registration,
  };
  return reply_status(syscall_call(set, &request, sizeof(request), NULL, 0), 0);
}

static enum call_status set_wait(handle_t set, uint64_t deadline,
    struct readiness_set_event *events, struct readiness_set_wait_reply *reply)
{
  struct readiness_set_wait_request request = {
    .header = {PROTOCOL_READINESS_SET, READINESS_SET_WAIT},
    .deadline = deadline, .events_address = (uintptr_t)events, .capacity = READINESS_SET_BATCH_MAX,
  };
  enum call_status status = reply_status(syscall_call(set, &request, sizeof(request),
      reply, sizeof(*reply)), sizeof(*reply));
  if (status == CALL_OK && reply->count > READINESS_SET_BATCH_MAX) {
    return CALL_IO;
  }
  return status;
}

static int read_clock(uint64_t *now)
{
  enum call_status status = clock_now(libc_thread_clock(), now);
  return status == CALL_OK ? 0 : fail(libc_call_errno(status));
}

static int signal_sources_refresh(handle_t set, size_t tag_base,
    struct poll_signal_source sources[2], bool *pending)
{
  for (size_t index = 0; index < 2; ++index) {
    if (sources[index].registered) {
      enum call_status status = set_remove(set, sources[index].registration);
      if (status != CALL_OK) {
        return fail(libc_call_errno(status));
      }
      sources[index].registered = false;
    }
  }
  struct libc_signal_wait wait;
  if (libc_signal_prepare(&wait) < 0) {
    return -1;
  }
  *pending = wait.pending;
  for (size_t index = 0; index < wait.count; ++index) {
    struct wait_interest *interest = &wait.interests[index];
    enum call_status status = set_register(set, interest->handle, interest->events,
        interest->observed_generation, tag_base + index, &sources[index].registration);
    if (status != CALL_OK) {
      return fail(libc_call_errno(status));
    }
    sources[index].registered = true;
  }
  return 0;
}

static int poll_wait(struct pollfd *descriptors, nfds_t count, struct poll_duration duration,
    const sigset_t *temporary_mask, bool selecting)
{
  if (count > INT_MAX) {
    return fail(EINVAL);
  }
  if (count && !descriptors) {
    return fail(EFAULT);
  }
  if (count > SIZE_MAX / sizeof(struct poll_row)) {
    return fail(EOVERFLOW);
  }
  bool timed = duration.infinite || duration.nanoseconds != 0;
  uint64_t deadline = 0;
  if (timed) {
    uint64_t now;
    if (read_clock(&now) < 0) {
      return -1;
    }
    if (!duration.infinite && duration.nanoseconds > UINT64_MAX - now) {
      return fail(EOVERFLOW);
    }
    deadline = duration.infinite ? UINT64_MAX : now + duration.nanoseconds;
  }
  struct poll_row *rows = count ? calloc(count, sizeof(*rows)) : NULL;
  if (count && !rows) {
    return -1;
  }
  int error = 0;
  bool immediate = false;
  bool signal_scope = false;
  sigset_t previous_mask;
  struct poll_signal_source signal_sources[2] = {0};
  handle_t set = HANDLE_INVALID;
  for (size_t index = 0; index < count; ++index) {
    if (descriptors[index].fd < 0) {
      continue;
    }
    unsigned requested = (unsigned short)descriptors[index].events;
    if (requested & ~(POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP | POLLNVAL |
        POLLRDNORM | POLLRDBAND | POLLWRNORM | POLLWRBAND)) {
      error = EINVAL;
      break;
    }
    if (requested & (POLLPRI | POLLRDBAND | POLLWRBAND)) {
      error = ENOTSUP;
      break;
    }
    if (descriptor_poll_begin(descriptors[index].fd, descriptors[index].events,
        &rows[index].descriptor) < 0) {
      error = errno;
      break;
    }
    if (selecting && (rows[index].descriptor.immediate & POLLNVAL)) {
      error = EBADF;
      break;
    }
    immediate |= rows[index].descriptor.immediate != 0;
  }
  if (!error) {
    if (libc_signal_mask_install(temporary_mask, &previous_mask) < 0) {
      error = errno;
    } else {
      signal_scope = true;
    }
  }
  if (!error) {
    struct readiness_set_create_reply reply = {0};
    enum call_status status = reply_status(syscall_readiness_set_create(&reply, sizeof(reply)),
        sizeof(reply));
    if (status != CALL_OK) {
      error = libc_call_errno(status);
    } else {
      set = reply.handle;
    }
  }
  for (size_t index = 0; !error && index < count; ++index) {
    struct descriptor_poll *descriptor = &rows[index].descriptor;
    if (!descriptor->interests) {
      continue;
    }
    enum call_status status = set_register(set, descriptor->handle, descriptor->interests,
        0, index, &rows[index].registration);
    if (status != CALL_OK) {
      error = libc_call_errno(status);
    } else {
      rows[index].registered = true;
    }
  }
  if (!error) {
    bool pending;
    if (signal_sources_refresh(set, count, signal_sources, &pending) < 0) {
      error = errno;
    } else {
      immediate |= pending;
    }
  }
  /* All sources attach before waiting. Remove returned registrations so a
   * permanently ready source cannot repeat while collecting the next batch. */
  while (!error) {
    uint64_t until = 0;
    if (timed && !immediate) {
      uint64_t now;
      if (read_clock(&now) < 0) {
        error = errno;
        break;
      }
      if (duration.infinite && now == UINT64_MAX) {
        error = EOVERFLOW;
        break;
      }
      if (duration.infinite || now < deadline) {
        until = now > UINT64_MAX - WAIT_MAX_WAIT_NS ? UINT64_MAX : now + WAIT_MAX_WAIT_NS;
        if (!duration.infinite && until > deadline) {
          until = deadline;
        }
      }
    }
    struct readiness_set_event events[READINESS_SET_BATCH_MAX];
    struct readiness_set_wait_reply reply = {0};
    enum call_status status = set_wait(set, until, events, &reply);
    if (status == CALL_TIMED_OUT) {
      continue; /* Recompute a slice against the original deadline. */
    }
    if (status != CALL_OK) {
      error = libc_call_errno(status);
      break;
    }
    if (!reply.count) {
      if (!until) {
        break;
      }
      continue;
    }
    bool refresh_signals = false;
    for (size_t index = 0; index < reply.count; ++index) {
      struct readiness_set_event *event = &events[index];
      if (event->status >= CALL_STATUS_COUNT) {
        error = EIO;
        break;
      }
      if (event->tag >= count) {
        size_t source_index = event->tag - count;
        if (source_index >= 2 || !signal_sources[source_index].registered ||
            event->registration.identity != signal_sources[source_index].registration.identity ||
            event->registration.generation != signal_sources[source_index].registration.generation) {
          error = EIO;
          break;
        }
        refresh_signals = true;
        continue;
      }
      struct poll_row *row = &rows[event->tag];
      if (!row->registered || event->registration.identity != row->registration.identity ||
          event->registration.generation != row->registration.generation) {
        error = EIO;
        break;
      }
      row->events = event->events;
      row->status = event->status;
      status = set_remove(set, row->registration);
      if (status != CALL_OK) {
        error = libc_call_errno(status);
        break;
      }
      row->registered = false;
      immediate = true;
    }
    if (!error && refresh_signals) {
      bool pending;
      if (signal_sources_refresh(set, count, signal_sources, &pending) < 0) {
        error = errno;
      } else {
        immediate |= pending;
      }
    }
  }
  if (set != HANDLE_INVALID && descriptor_release_handle(set) < 0 && !error) {
    error = errno;
  }
  int ready = 0;
  if (!error) {
    for (size_t index = 0; index < count; ++index) {
      if (descriptors[index].fd < 0) {
        continue;
      }
      rows[index].descriptor.immediate = descriptor_poll_result(descriptors[index].fd,
          &rows[index].descriptor, rows[index].events, rows[index].status);
      ready += rows[index].descriptor.immediate != 0;
    }
  }
  for (size_t index = 0; index < count; ++index) {
    descriptor_drop(rows[index].descriptor.object);
  }
  /* Only scalar staged results remain. No set, descriptor or FILE operation
   * survives a handler; returning handlers may close or replace those fds. */
  if (signal_scope) {
    struct libc_signal_delivery delivery = libc_signal_checkpoint();
    if (!error && delivery.caught) {
      error = EINTR;
    }
    libc_signal_mask_restore(&previous_mask);
    /* Delivery made eligible only by restoring the caller's mask follows the
     * completed wait result, rather than retroactively interrupting it. */
    libc_signal_checkpoint();
  }
  if (!error) {
    for (size_t index = 0; index < count; ++index) {
      descriptors[index].revents = rows[index].descriptor.immediate;
    }
  }
  free(rows);
  return error ? fail(error) : ready;
}

static int timespec_duration(const struct timespec *timeout, struct poll_duration *duration)
{
  *duration = (struct poll_duration){.infinite = timeout == NULL};
  if (!timeout) {
    return 0;
  }
  if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
      timeout->tv_nsec >= (long)NANOSECONDS_PER_SECOND) {
    return fail(EINVAL);
  }
  if ((uint64_t)timeout->tv_sec >
      (UINT64_MAX - (uint64_t)timeout->tv_nsec) / NANOSECONDS_PER_SECOND) {
    return fail(EOVERFLOW);
  }
  duration->nanoseconds = (uint64_t)timeout->tv_sec * NANOSECONDS_PER_SECOND + timeout->tv_nsec;
  return 0;
}

int poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
  struct poll_duration duration = {
    .infinite = timeout < 0,
    .nanoseconds = timeout < 0 ? 0 : (uint64_t)timeout * UINT64_C(1000000),
  };
  return poll_wait(descriptors, count, duration, NULL, false);
}

int ppoll(struct pollfd *descriptors, nfds_t count, const struct timespec *timeout,
    const sigset_t *sigmask)
{
  struct poll_duration duration;
  return timespec_duration(timeout, &duration) < 0 ? -1 :
      poll_wait(descriptors, count, duration, sigmask, false);
}

static int select_wait(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
    struct poll_duration duration, const sigset_t *temporary_mask)
{
  if (nfds < 0 || nfds > FD_SETSIZE) {
    return fail(EINVAL);
  }
  struct pollfd descriptors[FD_SETSIZE];
  nfds_t count = 0;
  for (int descriptor = 0; descriptor < nfds; ++descriptor) {
    if (exceptfds && FD_ISSET(descriptor, exceptfds)) {
      return fail(ENOTSUP);
    }
    short events = (readfds && FD_ISSET(descriptor, readfds) ? POLLIN : 0) |
        (writefds && FD_ISSET(descriptor, writefds) ? POLLOUT : 0);
    if (events) {
      descriptors[count++] = (struct pollfd){.fd = descriptor, .events = events};
    }
  }
  if (poll_wait(descriptors, count, duration, temporary_mask, true) < 0) {
    return -1;
  }
  fd_set reading = {0}, writing = {0};
  int ready = 0;
  for (size_t index = 0; index < count; ++index) {
    struct pollfd *descriptor = &descriptors[index];
    if (descriptor->revents & POLLNVAL) {
      return fail(EBADF);
    }
    if ((descriptor->events & POLLIN) && (descriptor->revents & (POLLIN | POLLHUP | POLLERR))) {
      FD_SET(descriptor->fd, &reading);
      ++ready;
    }
    if ((descriptor->events & POLLOUT) && (descriptor->revents & (POLLOUT | POLLERR))) {
      FD_SET(descriptor->fd, &writing);
      ++ready;
    }
  }
  if (readfds) {
    *readfds = reading;
  }
  if (writefds) {
    *writefds = writing;
  }
  if (exceptfds) {
    FD_ZERO(exceptfds);
  }
  return ready;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
    struct timeval *timeout)
{
  struct poll_duration duration = {.infinite = timeout == NULL};
  if (timeout) {
    if (timeout->tv_usec < 0 || timeout->tv_usec >= 1000000) {
      return fail(EINVAL);
    }
    struct timespec value = {timeout->tv_sec, timeout->tv_usec * 1000};
    if (timespec_duration(&value, &duration) < 0) {
      return -1;
    }
  }
  return select_wait(nfds, readfds, writefds, exceptfds, duration, NULL);
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
    const struct timespec *timeout, const sigset_t *sigmask)
{
  struct poll_duration duration;
  return timespec_duration(timeout, &duration) < 0 ? -1 :
      select_wait(nfds, readfds, writefds, exceptfds, duration, sigmask);
}
