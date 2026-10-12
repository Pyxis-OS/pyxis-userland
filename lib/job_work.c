#include <job_work.h>
#include <clock.h>
#include <directory.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>

struct job_work {
  handle_t parent, directory;
  char name[64];
};

struct cleanup_frame {
  struct cleanup_frame *parent;
  handle_t directory;
  char *name;
};

static enum call_status clear_directory(handle_t directory)
{
  struct cleanup_frame root = {.directory = directory};
  struct cleanup_frame *frame = &root;
  char *name = NULL;
  size_t capacity = 0;
  enum call_status status = CALL_OK;
  while (frame) {
    struct directory_cursor cursor = {0};
    struct directory_enumerate_reply entry;
    status = directory_enumerate(frame->directory, &cursor, name, capacity, &entry);
    if (status != CALL_OK) {
      break;
    }
    if (entry.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      if (!entry.name_size || entry.name_size > SIZE_MAX) {
        status = CALL_LIMIT;
        break;
      }
      char *grown = realloc(name, entry.name_size);
      if (!grown) {
        status = CALL_NO_MEMORY;
        break;
      }
      name = grown;
      capacity = entry.name_size;
      continue;
    }
    if (entry.outcome == DIRECTORY_END) {
      if (frame == &root) {
        break;
      }
      struct cleanup_frame *parent = frame->parent;
      status = directory_remove(parent->directory, frame->name, DIRECTORY_KIND_DIRECTORY);
      handle_close(frame->directory);
      free(frame->name);
      free(frame);
      frame = parent;
      if (status != CALL_OK) {
        break;
      }
      continue;
    }
    if (entry.outcome != DIRECTORY_ENTRY) {
      status = CALL_BAD_REQUEST;
      break;
    }
    if (entry.kind == DIRECTORY_KIND_DIRECTORY) {
      struct cleanup_frame *child = calloc(1, sizeof(*child));
      if (!child) {
        status = CALL_NO_MEMORY;
        break;
      }
      child->name = name;
      name = NULL;
      capacity = 0;
      status = directory_lookup(frame->directory, child->name, DIRECTORY_KIND_DIRECTORY,
          JOB_PROVISIONER_RIGHTS, &child->directory);
      if (status != CALL_OK) {
        free(child->name);
        free(child);
        break;
      }
      child->parent = frame;
      frame = child;
    } else {
      status = directory_remove(frame->directory, name, entry.kind);
      if (status != CALL_OK) {
        break;
      }
    }
  }
  while (frame && frame != &root) {
    struct cleanup_frame *parent = frame->parent;
    handle_close(frame->directory);
    free(frame->name);
    free(frame);
    frame = parent;
  }
  free(name);
  return status;
}

enum call_status job_work_create(handle_t parent, handle_t clock, struct job_work **result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = NULL;
  struct handle_info info;
  enum call_status status = handle_query(parent, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_DIRECTORY) {
    return CALL_WRONG_TYPE;
  }
  if ((info.rights & JOB_PROVISIONER_RIGHTS) != JOB_PROVISIONER_RIGHTS || info.transport) {
    return CALL_DENIED;
  }
  uint64_t now;
  status = clock_now(clock, &now);
  if (status != CALL_OK) {
    return status;
  }
  struct job_work *work = calloc(1, sizeof(*work));
  if (!work) {
    return CALL_NO_MEMORY;
  }
  status = handle_copy_restricted(parent, JOB_PROVISIONER_RIGHTS, 0, &work->parent);
  if (status != CALL_OK) {
    free(work);
    return status;
  }
  /* Exclusive creation arbitrates identical timestamps across launchers. */
  for (uint64_t suffix = 0; suffix < 64; ++suffix) {
    snprintf(work->name, sizeof(work->name), ".jobs-%jx-%jx", (uintmax_t)now,
        (uintmax_t)suffix);
    status = directory_create(parent, work->name, DIRECTORY_KIND_DIRECTORY,
        JOB_PROVISIONER_RIGHTS, &work->directory);
    if (status != CALL_ALREADY_EXISTS) {
      break;
    }
  }
  if (status != CALL_OK) {
    handle_close(work->parent);
    free(work);
    return status;
  }
  *result = work;
  return CALL_OK;
}

handle_t job_work_directory(const struct job_work *work)
{
  return work ? work->directory : HANDLE_INVALID;
}

enum call_status job_work_close(struct job_work *work, bool retired)
{
  if (!work) {
    return CALL_OK;
  }
  enum call_status status = CALL_OK;
  if (retired) {
    status = clear_directory(work->directory);
    if (status == CALL_OK) {
      status = directory_remove(work->parent, work->name, DIRECTORY_KIND_DIRECTORY);
    }
  }
  if (handle_close(work->directory) && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  if (handle_close(work->parent) && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  free(work);
  return status;
}
