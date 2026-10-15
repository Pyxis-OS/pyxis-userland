#include "../common/launch_environment.h"
#include <network_environment.h>
#include <net_config.h>
#include <clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DNS_SETUP_WAIT_NS UINT64_C(10000000000)
#define DNS_SETUP_POLL_NS UINT64_C(100000000)

enum call_status network_environment_wait(handle_t authority, handle_t clock)
{
  if (authority == HANDLE_INVALID) {
    return CALL_OK;
  }
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK || now > UINT64_MAX - DNS_SETUP_WAIT_NS) {
    return status == CALL_OK ? CALL_LIMIT : status;
  }
  uint64_t end = now + DNS_SETUP_WAIT_NS;
  for (;;) {
    struct net_config_reply snapshot;
    status = net_config_query(authority, &snapshot);
    if (status != CALL_OK || snapshot.dns_server) {
      return status;
    }
    status = clock_now(clock, &now);
    if (status != CALL_OK || now >= end) {
      return status == CALL_OK ? CALL_TIMED_OUT : status;
    }
    uint64_t delay = DNS_SETUP_POLL_NS;
    status = clock_sleep_until(clock, delay < end - now ? now + delay : end);
    if (status != CALL_OK) {
      return status;
    }
  }
}

enum call_status network_environment_read(struct network_environment *environment,
    handle_t authority, const struct launch_environment_entry *source, size_t count,
    const char *fallback)
{
  *environment = (struct network_environment){0};
  if (count > SIZE_MAX / sizeof(*environment->variables) - 1) {
    return CALL_LIMIT;
  }
  uint32_t chosen = 0;
  if (authority != HANDLE_INVALID) {
    struct net_config_reply snapshot;
    enum call_status status = net_config_query(authority, &snapshot);
    if (status != CALL_OK) {
      return status;
    }
    chosen = snapshot.dns_server;
  }
  environment->variables = malloc((count + 1) * sizeof(*environment->variables));
  if (!environment->variables) {
    return CALL_NO_MEMORY;
  }
  const char *dns_server = fallback;
  for (size_t i = 0; i < count; ++i) {
    if (!strcmp((const char *)source[i].name, "DNS_SERVER")) {
      dns_server = (const char *)source[i].value;
    } else {
      environment->variables[environment->count++] = source[i];
    }
  }
  if (chosen) {
    snprintf(environment->dns_server, sizeof(environment->dns_server), "%u.%u.%u.%u",
        chosen >> 24, (chosen >> 16) & 255, (chosen >> 8) & 255, chosen & 255);
    dns_server = environment->dns_server;
  }
  if (dns_server) {
    environment->variables[environment->count++] = (struct launch_environment_entry)PROFILE_TEXT("DNS_SERVER", dns_server);
  }
  return CALL_OK;
}

enum call_status network_environment_from_capture(struct network_environment *environment,
    handle_t authority, const struct environment_capture *source, const char *fallback)
{
  if (source->count > SIZE_MAX / sizeof(struct launch_environment_entry)) {
    return CALL_LIMIT;
  }
  struct launch_environment_entry *entries = malloc(source->count * sizeof(*entries));
  if (source->count && !entries) {
    return CALL_NO_MEMORY;
  }
  size_t count = 0;
  for (size_t i = 0; i < source->count; ++i) {
    const struct environment_value *value = &source->values[i];
    if (value->kind == ENVIRONMENT_TEXT &&
        strcmp(value->name, "stdin.append") && strcmp(value->name, "stdout.append") &&
        strcmp(value->name, "stderr.append")) {
      entries[count++] = (struct launch_environment_entry)PROFILE_TEXT(value->name, value->text);
    }
  }
  enum call_status status = network_environment_read(environment, authority, entries, count, fallback);
  free(entries);
  return status;
}

void network_environment_free(struct network_environment *environment)
{
  free(environment->variables);
  *environment = (struct network_environment){0};
}
