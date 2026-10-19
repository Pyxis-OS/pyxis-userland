#ifndef USERSPACE_COMMAND_SEARCH_H
#define USERSPACE_COMMAND_SEARCH_H

#include <abi/command_search.h>
#include <abi/launcher.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Attest native list kind, READ authority and zero transport. */
enum call_status command_list_validate(handle_t list);
enum call_status command_list_info(handle_t list, struct command_list_info_reply *info);
/* A successful GET returns an independently owned directory grant. */
enum call_status command_list_get(handle_t list, size_t index,
    struct command_list_entry_reply *entry);
/* The writable environment is a factory, not a publication target here. Inputs
 * stay borrowed and the returned list is owned. Select/reorder/attenuate held
 * members by supplying their handles and exact rights in the desired order. */
enum call_status command_list_create(handle_t environment,
    const struct command_list_create_entry *entries, size_t count, handle_t *list);
/* Atomically replace this environment's commands CAP, retaining the list. */
enum call_status command_list_publish(handle_t environment, handle_t list, bool exported);

struct command_search_capture {
  handle_t list;
  struct command_list_entry_reply *entries;
  size_t count;
  uint64_t capacity;
};

/* One owned immutable list and independently owned member grants; close once.
 * Init captures commands with one lookup (NOT_FOUND means absent). List capture
 * copies an already held list. Empty lists succeed and retain a nonzero list.
 * Neither helper resolves commands or adds any default search entry. */
enum call_status command_search_capture_init(handle_t environment,
    struct command_search_capture *capture);
enum call_status command_search_capture_list(handle_t list,
    struct command_search_capture *capture);
void command_search_capture_close(struct command_search_capture *capture);
/* Stage the captured list as exported commands. Arrays borrow it through
 * synchronous launch capture. Failure preserves both counts. */
enum call_status command_search_stage(const struct command_search_capture *capture,
    struct launch_grant *grants, size_t grant_capacity, size_t *grant_count,
    struct launch_environment_entry *entries, size_t entry_capacity, size_t *entry_count);

#ifdef __cplusplus
}
#endif

#endif
