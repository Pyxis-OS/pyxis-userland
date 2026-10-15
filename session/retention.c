#include "retention.h"
#include <environment.h>
#include <handle.h>
#include <startup.h>

static enum call_status lookup_private(const char *name, uint64_t protocol,
    uint64_t rights, struct environment_entry_reply *entry)
{
  enum call_status status = environment_lookup(startup_environment_handle(), name, entry, NULL, 0);
  if (status != CALL_OK) {
    return status;
  }
  struct handle_info info;
  status = entry->kind != ENVIRONMENT_CAP || entry->flags ? CALL_DENIED :
      handle_query(entry->handle, &info);
  if (status == CALL_OK && (info.kind != HANDLE_KIND_NATIVE || info.protocol != protocol ||
      info.transport || (info.rights & rights) != rights)) {
    status = CALL_DENIED;
  }
  if (status != CALL_OK && entry->handle) {
    handle_close(entry->handle);
    entry->handle = HANDLE_INVALID;
  }
  return status;
}

static enum call_status consume_private(const char *name, const struct environment_entry_reply *entry)
{
  struct environment_unset_entry removed = {.identity = entry->identity};
  environment_name(removed.name, name);
  return environment_unset_batch(startup_environment_handle(), &removed, 1);
}

enum call_status session_retention_init(const struct session_config *config, handle_t *account)
{
  if (!config || !account) {
    return CALL_BAD_REQUEST;
  }
  *account = HANDLE_INVALID;
  struct environment_entry_reply inherited = {0}, factory = {0};
  enum call_status status = lookup_private("retention_account", PROTOCOL_RETENTION_ACCOUNT,
      RETENTION_RIGHT_CAPTURE, &inherited);
  if (status != CALL_OK && status != CALL_NOT_FOUND) {
    return status;
  }
  enum call_status found_factory = lookup_private("retention_factory", PROTOCOL_RETENTION_FACTORY,
      RETENTION_FACTORY_RIGHT_CREATE, &factory);
  if (status == CALL_OK) {
    if (found_factory != CALL_NOT_FOUND) {
      handle_close(inherited.handle);
      if (found_factory == CALL_OK) {
        handle_close(factory.handle);
      }
      return found_factory == CALL_OK ? CALL_DENIED : found_factory;
    }
    status = consume_private("retention_account", &inherited);
    if (status == CALL_OK) {
      *account = inherited.handle;
    } else {
      handle_close(inherited.handle);
    }
    return status;
  }
  if (found_factory != CALL_OK) {
    return found_factory;
  }
  status = consume_private("retention_factory", &factory);
  if (status == CALL_OK) {
    struct retention_limits limits = {config->value_limit_bytes, config->session_limit_bytes};
    status = retention_create_account(factory.handle, &limits, account);
  }
  if (handle_close(factory.handle) != 0 && status == CALL_OK) {
    handle_close(*account);
    *account = HANDLE_INVALID;
    status = CALL_IO;
  }
  return status;
}
