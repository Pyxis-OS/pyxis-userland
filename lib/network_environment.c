#include <network_environment.h>
#include <net_config.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum call_status network_environment_read(struct network_environment *environment,
    handle_t authority, const struct startup_variable *source, size_t count,
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
    environment->variables[environment->count++] = (struct startup_variable){
      (uintptr_t)"DNS_SERVER", (uintptr_t)dns_server,
    };
  }
  return CALL_OK;
}

void network_environment_free(struct network_environment *environment)
{
  free(environment->variables);
  *environment = (struct network_environment){0};
}
