#include <script_scope.h>
#include <launcher.h>
#include <handle.h>

enum call_status script_scope_open(bool positional, handle_t launcher,
    handle_t child_launcher, struct execution_group_create_reply *group)
{
  handle_t selected = positional ? launcher : child_launcher;
  if (!group) {
    return CALL_BAD_REQUEST;
  }
  if (selected == HANDLE_INVALID) {
    return positional ? CALL_DENIED : CALL_OK;
  }
  return launcher_create_group(selected, group);
}

handle_t script_scope_launch_launcher(bool positional, bool foreground,
    handle_t own_launcher, const struct execution_group_create_reply *group,
    handle_t command_launcher)
{
  if (command_launcher != HANDLE_INVALID) {
    return command_launcher;
  }
  if ((foreground || positional) && group && group->launcher != HANDLE_INVALID) {
    return group->launcher;
  }
  return own_launcher;
}

handle_t script_scope_child_launcher(bool positional, bool foreground,
    handle_t child_launcher, const struct execution_group_create_reply *group,
    handle_t command_launcher)
{
  if (command_launcher != HANDLE_INVALID && child_launcher != HANDLE_INVALID) {
    return command_launcher;
  }
  if ((foreground || positional) && group && group->launcher != HANDLE_INVALID) {
    return group->launcher;
  }
  return child_launcher;
}

struct script_scope_close_result script_scope_close(
    struct execution_group_create_reply *group)
{
  struct script_scope_close_result result = {.status = CALL_OK};
  if (!group) {
    result.status = CALL_BAD_REQUEST;
    return result;
  }
  handle_t launcher = group->launcher;
  handle_t supervision = group->supervision;
  if (supervision != HANDLE_INVALID) {
    result.status = execution_group_terminate(supervision);
    if (result.status == CALL_OK) {
      result.status = execution_group_wait(supervision);
    }
  }
  *group = (struct execution_group_create_reply){0};
  if (launcher != HANDLE_INVALID && handle_close(launcher) != 0) {
    result.close_failed = true;
  }
  if (supervision != HANDLE_INVALID && handle_close(supervision) != 0) {
    result.close_failed = true;
  }
  return result;
}
