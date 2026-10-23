#include "../common/launch_environment.h"
#ifndef REMOTE_SESSION_H
#define REMOTE_SESSION_H

#include <abi/process.h>
#include <abi/launcher.h>
#include <terminal.h>
#include <startup.h>
#include <job_work.h>
#include <network_environment.h>
#include <environment.h>
#include "../common/pane_broker.h"

struct remote_shell {
  struct job_work *job_provisioner;
  handle_t attachment;
  handle_t group;
  handle_t process;
};

/* Root-only options: quiet input and byte records without presentation cleanup. */
enum call_status remote_shell_launch(size_t columns, size_t rows, unsigned tab_width,
    bool no_echo, bool records, struct remote_shell *shell);

/* Retained authority and initial environment belong to this connection. The
 * network environment borrows strings from inherited and its own DNS buffer. */
struct remote_context {
  handle_t job_provisioner;
  struct launch_grant *grants; /* Owned restricted CAP copies. */
  struct launch_environment_entry *entries; /* Owned names, indices into grants. */
  size_t grant_count, entry_count, capacity;
  size_t directory;
  bool allow_launch, multiplexer;
  struct environment_capture inherited;
  struct network_environment environment;
};

struct remote_pane {
  bool used, published, closing, group_ready;
  struct pane_broker_message request;
  handle_t reply, bootstrap;
  struct pyxis_working_snapshot cwd;
  size_t directories, bytes;
  struct remote_shell shell;
};

struct remote_broker {
  handle_t receiver;
  bool sealed;
  enum call_status error;
  uint64_t last_id;
  struct remote_context context;
  struct remote_pane panes[PANE_BROKER_PANES];
};

enum call_status remote_mux_launch(size_t columns, size_t rows, unsigned tab_width,
    struct remote_broker *broker, struct remote_shell *shell);
/* Service a bounded number of messages without awaiting another chunk. */
bool remote_broker_service(struct remote_broker *broker);
void remote_broker_seal(struct remote_broker *broker);
bool remote_broker_complete(const struct remote_broker *broker);
uint64_t remote_broker_deadline(const struct remote_broker *broker, uint64_t deadline);
/* Only after every pane group has completed. */
void remote_broker_release(struct remote_broker *broker);

#endif
