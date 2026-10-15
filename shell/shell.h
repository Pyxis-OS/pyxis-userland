#ifndef USERSPACE_SHELL_H
#define USERSPACE_SHELL_H

#include <abi/launcher.h>
#include <abi/startup.h>
#include <abi/terminal.h>
#include <path.h>
#include <term.h>
#include <startup.h>
#include "../common/launch_environment.h"
#include <bundle_launch.h>

#define SHELL_LINE_CAPACITY 1024
/* Bytes before LF, including CR in CRLF; storage also needs a trailing NUL. */
#define SHELL_SCRIPT_LINE_MAX 1024
enum command_result {
  COMMAND_OK,
  COMMAND_FAILED,
  COMMAND_EXIT,
  COMMAND_FATAL, /* Terminal, wait or cleanup failed; input cannot resume. */
};

/* Reported completion, separate from command_result control flow. Kind is a
 * TERMINAL_COMPLETION value; status is the exit code for EXITED, 0 or 1 for
 * BUILTIN and zero otherwise. Undefined when the command result is FATAL. */
struct shell_outcome {
  uint64_t kind;
  int64_t status;
};

enum shell_launch_mode { SHELL_FOREGROUND, SHELL_BACKGROUND, SHELL_SESSION,
    SHELL_SERVICE };

struct bundle_program;
struct shell_capability_table;

struct shell_redirection {
  enum environment_stream_index stream;
  const char *path;
  bool capability;
  bool append;
  const struct environment_value *value;
};

struct shell_stage {
  char **arguments;
  size_t count;
  size_t redirection_count;
  bool http_worker;
  bool http_producer;
  const struct environment_capture *operands;
  struct shell_redirection redirections[ENVIRONMENT_STREAM_COUNT];
};

struct shell_command_line {
  size_t stage_count;
  bool background;
  const char *capture;
  struct shell_stage stages[LAUNCH_BATCH_MAX];
};

#define SHELL_FAT_MOUNT_LIMIT 4
struct shell_fat_mount {
  char *name, *write_name, *control_name;
  handle_t control, write_root;
  uint64_t domain;
  bool bound;
};

struct shell {
  struct bundle_jobs jobs;
  struct shell_capability_table *capabilities;
  const char *script_name; /* Borrowed diagnostic name, NULL for interactive input. */
  size_t script_line;
  /* Root-shell options only; never forwarded to children. */
  bool quiet_input;
  bool remote_prompt;
  /* Keep the supervised remote root alive through session successors. */
  bool session_wait;
  int exit_status;
  /* Input carries Ctrl+C arming and a clock bounds the waits. Only a root
   * shell or session successor is granted it; commands get READ alone. */
  bool interrupts;
  struct terminal terminal;
  /* Interactive lines for Up/Down recall; the quiet editor keeps none. */
  struct term_history history;
  /* Owned home:// capture holding .history; saving stops after one failure. */
  handle_t history_home;
  bool history_saving;
  handle_t profile, space, launcher, child_launcher, memory, display, clock, system_info, log,
      echo, udp, udp_beacons, tcp, pipe, service, environment, random, net_config,
      keyboard, pointer, terminal_service,
      boot, tmp, host_mount, native_mount, power, screen_capture, audio;
  const struct path_context *directory;
  /* Own the original startup chain after libc retains its independent cwd.
   * Clear released slots so handle-number reuse cannot revive stale owners. */
  handle_t *initial_directories;
  size_t initial_directory_count;
  struct path_workspace workspace;
  /* Writable FAT grants are selected sources, never inherited URI roots. */
  struct shell_fat_mount fat_mounts[SHELL_FAT_MOUNT_LIMIT];
};

/* Compacts in place. Arguments and redirection paths borrow line; capacity
 * includes a NULL after each stage's arguments. Redirections remain in source order.
 * An unquoted trailing & selects background launch.
 * Returns a static diagnostic on malformed input, NULL on success. */
const char *parse_line(char *line, char **arguments, size_t capacity,
    struct shell_command_line *command);
enum call_status shell_directory_init(struct shell *shell);
void shell_directory_close(struct shell *shell);
enum call_status shell_change_directory(struct shell *shell, const char *path);
/* Owned handle to a directory reached through the shell's own roots. */
enum call_status shell_open_directory(struct shell *shell, const char *path, uint64_t rights,
    handle_t *directory);
enum call_status shell_reserve_root(struct shell *shell, const char *name,
    size_t length, char **reserved);
/* Consumes name and root on every path. */
enum call_status shell_publish_root(struct shell *shell, char *name, handle_t root);
enum command_result shell_mount_fat32(struct shell *shell, char **arguments, size_t count);
enum command_result shell_sync_fat32(struct shell *shell, char **arguments, size_t count);
enum command_result shell_release_fat32(struct shell *shell, char **arguments, size_t count);
/* Close owned data/control grants without implicit retirement. */
void shell_fat_close(struct shell *shell);
/* Tab completion. The word under the cursor is a command name (the first word
 * of a pipeline stage: builtins and the programs in bin:// and boot://) or a
 * path (any argument or redirection target: see complete_path.c). */
bool shell_complete(void *context, const char *line, size_t cursor,
    struct term_candidates *result);
/* The word ending at the cursor, read with parse_line's quoting rules. value is
 * the unquoted text typed so far, heap-owned by the caller. */
struct completion_word {
  size_t start;  /* Offset of the word in the line. */
  bool command;  /* First word of a pipeline stage. */
  bool plain;    /* No quote or backslash in the word. */
  char quote;    /* Quote left open at the cursor: 0, ' or ". */
  char *value;
};
/* False where nothing is completed, such as after a background &. */
bool completion_scan(const char *line, size_t cursor, struct completion_word *word);
bool shell_complete_path(struct shell *shell, const struct completion_word *word,
    struct term_candidates *result);
/* IMAGE is owned for plain programs, borrowed from BUNDLE when it is present. */
enum call_status shell_open_image(struct shell *shell, const char *command, handle_t *image,
    struct bundle_program **bundle);
enum call_status shell_open_redirect(struct shell *shell, const char *path,
    bool input, handle_t *file);
enum command_result shell_error(struct shell *shell, const char *format, ...);
enum command_result shell_directory_error(struct shell *shell, const char *operation,
    const char *path, enum call_status status);
/* Interactive shells with history only. Load fills memory with the newest
 * saved entries; save merges one recorded line into home://.history unless it
 * starts with a space. Neither reports a missing or read-only home. */
void shell_history_load(struct shell *shell);
void shell_history_save(struct shell *shell, const char *line);
enum command_result shell_command(struct shell *shell, char *line, char **arguments,
    struct shell_outcome *outcome);
/* Outcome is NULL for the session builtin, which reports its own result. */
enum command_result shell_launch(struct shell *shell, char **arguments, size_t count,
    enum shell_launch_mode mode, const struct shell_redirection *redirections,
    size_t redirection_count, struct shell_outcome *outcome);
enum command_result shell_launch_pipeline(struct shell *shell,
    const struct shell_command_line *command, struct shell_outcome *outcome);
enum command_result shell_launch_service(struct shell *shell, const char *name,
    bool replace, bool optional, bool read_only, char **arguments, size_t count);
/* Borrows script; line has SHELL_SCRIPT_LINE_MAX + 1 bytes. */
int shell_script(struct shell *shell, handle_t script, char *line, char **arguments);

#endif
