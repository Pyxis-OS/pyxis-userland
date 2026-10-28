#include <program_runtime.h>
#include <script_scope.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
  bool begun, active, finished, reported;
  enum program_runtime_mode mode;
  enum call_status cleanup_status;
  struct program_sources sources;
  struct program_interpreters interpreters;
  struct execution_group_create_reply group;
  handle_t launcher, child_launcher;
} runtime;

static handle_t source_handle(const char *name)
{
  for (size_t i = 0; i < runtime.sources.ordinary_count; ++i) {
    const struct bundle_authority *source = &runtime.sources.authority[i];
    if (!strcmp(name, source->resource)) {
      return source->source;
    }
  }
  return HANDLE_INVALID;
}

static void close_runtime(void)
{
  if (!runtime.begun || runtime.finished) {
    return;
  }
  /* Mark first: cleanup failure must not replay termination on another exit
   * path. Group close clears references even when termination/wait fails. */
  runtime.finished = true;
  runtime.active = false;
  struct script_scope_close_result result = script_scope_close(&runtime.group);
  runtime.cleanup_status = result.status != CALL_OK ? result.status :
      result.close_failed ? CALL_BAD_HANDLE : CALL_OK;
  runtime.launcher = runtime.child_launcher = HANDLE_INVALID;
  program_interpreters_close(&runtime.interpreters);
  program_sources_close(&runtime.sources);
}

static void report_cleanup(void)
{
  if (runtime.cleanup_status != CALL_OK && !runtime.reported) {
    runtime.reported = true;
    fprintf(stderr, "script runtime: subtree cleanup failed (status %u)\n",
        (unsigned)runtime.cleanup_status);
  }
}

static void runtime_at_exit(void)
{
  close_runtime();
  report_cleanup();
}

enum call_status program_runtime_begin(enum program_interpreter_kind kind,
    enum program_runtime_mode mode, handle_t self_image, handle_t archive_root,
    handle_t package_root)
{
  if (runtime.begun) {
    return CALL_ALREADY_EXISTS;
  }
  if (kind != PROGRAM_INTERPRETER_SHELL && kind != PROGRAM_INTERPRETER_LUA &&
      kind != PROGRAM_INTERPRETER_LUA51) {
    return CALL_BAD_REQUEST;
  }
  if ((unsigned)mode > PROGRAM_RUNTIME_INTERACTIVE) {
    return CALL_BAD_REQUEST;
  }
  handle_t environment = startup_environment_handle();
  enum call_status status = program_sources_capture(environment, &runtime.sources);
  if (status != CALL_OK) {
    return status;
  }
  if (archive_root == HANDLE_INVALID) {
    archive_root = source_handle("interpreter_root");
  }
  status = program_interpreters_capture(environment, archive_root, package_root,
      self_image == HANDLE_INVALID ? PROGRAM_INTERPRETER_NONE : kind,
      self_image, &runtime.interpreters);
  if (status != CALL_OK) {
    program_sources_close(&runtime.sources);
    return status;
  }
  runtime.mode = mode;
  runtime.launcher = source_handle("launcher");
  runtime.child_launcher = source_handle("child_launcher");
  if (mode != PROGRAM_RUNTIME_INTERACTIVE && runtime.launcher != HANDLE_INVALID) {
    status = script_scope_open(mode == PROGRAM_RUNTIME_POSITIONAL,
        runtime.launcher, runtime.child_launcher, &runtime.group);
    if (status != CALL_OK) {
      goto fail;
    }
    /* Shell makes its positional subgroup the delegated child launcher before
     * command scopes are selected. Handed scripts retain the original one. */
    if (mode == PROGRAM_RUNTIME_POSITIONAL) {
      runtime.child_launcher = runtime.group.launcher;
    }
  }
  if (atexit(runtime_at_exit) != 0) {
    status = CALL_NO_MEMORY;
    goto fail;
  }
  runtime.begun = runtime.active = true;
  return CALL_OK;
fail:
  {
    struct script_scope_close_result closed = script_scope_close(&runtime.group);
    program_interpreters_close(&runtime.interpreters);
    program_sources_close(&runtime.sources);
    runtime.launcher = runtime.child_launcher = HANDLE_INVALID;
    return closed.status != CALL_OK ? closed.status : closed.close_failed ? CALL_BAD_HANDLE : status;
  }
}

bool program_runtime_active(void)
{
  return runtime.active;
}

bool program_runtime_interactive(void)
{
  return runtime.active && runtime.mode == PROGRAM_RUNTIME_INTERACTIVE;
}

const struct program_sources *program_runtime_sources(void)
{
  return runtime.active ? &runtime.sources : NULL;
}

const struct program_interpreters *program_runtime_interpreters(void)
{
  return runtime.active ? &runtime.interpreters : NULL;
}

enum call_status program_runtime_route(bool foreground, handle_t command_launcher,
    handle_t *admission, handle_t *child)
{
  if (admission) {
    *admission = HANDLE_INVALID;
  }
  if (child) {
    *child = HANDLE_INVALID;
  }
  if (!admission || !child) {
    return CALL_BAD_REQUEST;
  }
  if (!runtime.active) {
    return CALL_UNAVAILABLE;
  }
  if (runtime.launcher == HANDLE_INVALID) {
    return CALL_DENIED;
  }
  bool positional = runtime.mode == PROGRAM_RUNTIME_POSITIONAL;
  *admission = script_scope_launch_launcher(positional, foreground, runtime.launcher,
      &runtime.group, command_launcher);
  *child = script_scope_child_launcher(positional, foreground, runtime.child_launcher,
      &runtime.group, command_launcher);
  return CALL_OK;
}

int program_runtime_finish(int status)
{
  close_runtime();
  report_cleanup();
  return runtime.cleanup_status != CALL_OK && status == EXIT_SUCCESS ? EXIT_FAILURE : status;
}

enum call_status program_runtime_cleanup_status(void)
{
  return runtime.cleanup_status;
}
