#include "capture.h"
#include "syntax.h"
#include "launch_capture.h"
#include <clock.h>
#include <forwarder.h>
#include <handle.h>
#include <output.h>
#include <process.h>
#include <retention.h>
#include <wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool close_value(struct environment_value *value)
{
  bool closed = !value->handle || handle_close(value->handle) == 0;
  free(value->text);
  *value = (struct environment_value){0};
  return closed;
}

bool shell_capture_close(struct shell_capture_result *result)
{
  bool closed = true;
  if (result->service) {
    if (!result->published) {
      struct process_result completion;
      if (process_terminate(result->service) != CALL_OK ||
          process_wait(result->service, &completion) != CALL_OK) {
        closed = false;
      }
    }
    if (handle_close(result->service)) {
      closed = false;
    }
  }
  closed = close_value(&result->value) && closed;
  closed = close_value(&result->observation) && closed;
  closed = close_value(&result->control) && closed;
  *result = (struct shell_capture_result){0};
  return closed;
}

static enum call_status absent(handle_t table, const char *name)
{
  struct environment_entry_reply entry;
  enum call_status status = environment_lookup(table, name, &entry, NULL, 0);
  if (status == CALL_OK && entry.kind == ENVIRONMENT_CAP && handle_close(entry.handle)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return status == CALL_NOT_FOUND ? CALL_OK :
      status == CALL_OK || status == CALL_BUFFER_TOO_SMALL ? CALL_ALREADY_EXISTS : status;
}

enum call_status shell_capture_destination(struct shell *shell, const char *name,
    bool management)
{
  char checked[ENVIRONMENT_NAME_MAX + 1];
  if (!environment_name(checked, name)) {
    return CALL_BAD_REQUEST;
  }
  if (!shell->retention_account) {
    return CALL_DENIED;
  }
  if (!management) {
    return CALL_OK;
  }
  size_t length = strlen(name);
  if (length + sizeof(".control") > sizeof(checked)) {
    return CALL_LIMIT;
  }
  memcpy(checked + length, ".control", sizeof(".control"));
  return absent(shell->environment, checked);
}

static enum call_status adopt_value(handle_t handle, const char *name,
    struct environment_value *value)
{
  if (!handle) {
    return CALL_BAD_HANDLE;
  }
  /* Own the reference even if metadata is unavailable, so cleanup never loses
   * an already collected grant. Metadata-only describe never calls its issuer. */
  *value = (struct environment_value){.kind = ENVIRONMENT_CAP, .handle = handle};
  struct handle_info info;
  enum call_status status = handle_query(handle, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (!environment_name(value->name, name)) {
    return CALL_LIMIT;
  }
  value->rights = info.rights;
  value->transport = info.transport;
  return CALL_OK;
}

static uint64_t failure_outcome(enum call_status status)
{
  switch (status) {
  case CALL_NOT_FOUND: return RESOURCE_NOT_FOUND;
  case CALL_DENIED: return RESOURCE_DENIED;
  case CALL_FILE_TOO_LARGE: return RESOURCE_TOO_LARGE;
  case CALL_ABANDONED: case CALL_TIMED_OUT: return RESOURCE_ABORTED;
  case CALL_OUTCOME_UNKNOWN: return RESOURCE_UNCERTAIN;
  default: return RESOURCE_IO_ERROR;
  }
}

static enum call_status failed_value(struct shell *shell, const char *name,
    enum call_status cause, struct shell_capture_result *result)
{
  char detail[128];
  int length = snprintf(detail, sizeof(detail), "capture failed (status %u)", cause);
  if (length < 0 || (size_t)length >= sizeof(detail)) {
    return CALL_BAD_REQUEST;
  }
  handle_t value;
  enum call_status status = retention_create_failure(shell->retention_account,
      failure_outcome(cause), detail, (size_t)length, &value);
  if (status == CALL_OK) {
    status = adopt_value(value, name, &result->value);
  }
  result->command = COMMAND_FAILED;
  return status;
}

static enum call_status result_name(struct shell *shell, const char *destination,
    char name[ENVIRONMENT_NAME_MAX + 1])
{
  if (destination) {
    return environment_name(name, destination) ? CALL_OK : CALL_BAD_REQUEST;
  }
  static uint64_t sequence;
  for (;;) {
    if (sequence == UINT64_MAX) {
      return CALL_LIMIT;
    }
    snprintf(name, ENVIRONMENT_NAME_MAX + 1, "capture_%ju", (uintmax_t)++sequence);
    enum call_status status = absent(shell->environment, name);
    if (status != CALL_ALREADY_EXISTS) {
      return status;
    }
  }
}

static enum call_status paired_value(struct shell *shell, const struct shell_stage *stage,
    const char *destination, struct shell_capture_result *result)
{
  char control_name[ENVIRONMENT_NAME_MAX + 1];
  size_t length = strlen(destination);
  memcpy(control_name, destination, length);
  memcpy(control_name + length, ".control", sizeof(".control"));
  struct forwarder_reply pair = {0};
  enum call_status status = CALL_OK;
  bool started = false;
  if (!strcmp(stage->arguments[0], "revocable")) {
    if (stage->count != 2 || !stage->operands || stage->operands->count != 1) {
      return CALL_BAD_REQUEST;
    }
    const struct environment_value *target = &stage->operands->values[0];
    if (target->kind != ENVIRONMENT_CAP) {
      return CALL_WRONG_TYPE;
    }
    result->evaluated = true;
    status = forwarder_revocable(target->handle, 0, 0,
        FORWARDER_CREATE_SAME_RIGHTS, &pair);
  } else {
    if (stage->count < 3 || strcmp(stage->arguments[1], "start")) {
      return CALL_BAD_REQUEST;
    }
    struct shell_stage provider = *stage;
    provider.arguments += 2;
    provider.count -= 2;
    struct shell_service_value service = {0};
    enum call_status error = CALL_OK;
    result->command = shell_launch_service_value(shell, &provider, false,
        &service, &started, &error);
    if (result->command == COMMAND_FATAL) {
      return CALL_OUTCOME_UNKNOWN;
    }
    result->evaluated = service.evaluated;
    if (result->command != COMMAND_OK) {
      /* A definite preparation refusal precedes evaluation and leaves the
       * destination unchanged; attempted startup instead binds a failed value. */
      return service.evaluated ? failed_value(shell, destination, error, result) : error;
    }
    pair = service.slot;
    result->service = service.process;
  }
  if (status != CALL_OK) {
    return failed_value(shell, destination, status, result);
  }
  status = adopt_value(pair.holder, destination, &result->value);
  enum call_status control = adopt_value(pair.controller, control_name, &result->control);
  if (status == CALL_OK) {
    status = control;
  }
  result->command = COMMAND_OK;
  return status;
}

static enum call_status take_value(struct shell *shell, handle_t collector,
    const char *name, struct shell_capture_result *result)
{
  struct output_take_reply taken = {0};
  enum call_status status = output_seal(collector);
  while (status == CALL_OK) {
    status = output_take(collector, &taken);
    if (status != CALL_WOULD_BLOCK) {
      break;
    }
    uint64_t now;
    status = clock_now(shell->clock, &now);
    if (status != CALL_OK) {
      break;
    }
    struct wait_interest interest = {.handle = collector, .events = WAIT_COMPLETE};
    uint64_t events = 0;
    uint64_t until = now > UINT64_MAX - WAIT_MAX_WAIT_NS ? UINT64_MAX : now + WAIT_MAX_WAIT_NS;
    status = wait_many(&interest, 1, until, &events);
    if (status == CALL_TIMED_OUT) {
      status = CALL_OK;
    }
  }
  if (status != CALL_OK) {
    if (status == CALL_OUTCOME_UNKNOWN) {
      return status;
    }
    return failed_value(shell, name, status, result);
  }
  status = adopt_value(taken.value, name, &result->value);
  if (taken.observation) {
    char observation_name[ENVIRONMENT_NAME_MAX + 1];
    int length = snprintf(observation_name, sizeof(observation_name), "%s.status", name);
    enum call_status observation = adopt_value(taken.observation,
        length >= 0 && (size_t)length < sizeof(observation_name) ? observation_name : "",
        &result->observation);
    if (status == CALL_OK) {
      status = observation;
    }
  }
  if (status != CALL_OK) {
    return status;
  }
  struct describe_reply description;
  status = handle_describe(result->value.handle, 0, &description);
  if (status == CALL_OK && description.description.outcome != RESOURCE_OK) {
    result->command = COMMAND_FAILED;
    if (description.description.outcome == RESOURCE_TOO_LARGE) {
      struct retention_status_reply limits;
      if (retention_status(shell->retention_account, &limits) == CALL_OK) {
        shell_error(shell, "%s: too large to retain (limit %ju bytes); write it to a file instead\n",
            name, (uintmax_t)limits.value_limit);
      }
    }
  }
  return status;
}

enum call_status shell_capture_evaluate(struct shell *shell, const char *source,
    const char *destination, struct shell_capture_result *result)
{
  *result = (struct shell_capture_result){.command = COMMAND_OK};
  if (!shell->retention_account) {
    return CALL_DENIED;
  }
  struct shell_syntax syntax = {0};
  const char *diagnostic = shell_syntax_parse(source, &syntax);
  if (diagnostic) {
    return CALL_BAD_REQUEST;
  }
  if (!syntax.count) {
    shell_syntax_close(&syntax);
    return CALL_BAD_REQUEST;
  }
  char name[ENVIRONMENT_NAME_MAX + 1];
  enum call_status status = result_name(shell, destination, name);
  if (status != CALL_OK) {
    shell_syntax_close(&syntax);
    return status;
  }
  struct output_create_reply output = {0};
  for (size_t i = 0; i < syntax.count && status == CALL_OK; ++i) {
    struct shell_syntax_command *command = &syntax.commands[i];
    struct shell_expansion expansion = {0};
    enum call_status expanded;
    diagnostic = shell_syntax_expand(shell, command, &expansion, &expanded);
    if (diagnostic) {
      status = expanded == CALL_OK ? CALL_BAD_REQUEST : expanded;
      if (result->evaluated && status != CALL_OUTCOME_UNKNOWN) {
        shell_error(shell, "capture: %s (status %u)\n", diagnostic, status);
        result->command = COMMAND_FAILED;
        status = CALL_OK;
      }
      break;
    }
    const struct shell_stage *stage = &expansion.command.stages[0];
    bool management = stage->count && (!strcmp(stage->arguments[0], "revocable") ||
        !strcmp(stage->arguments[0], "service"));
    if (management) {
      if (!destination || syntax.count != 1 || expansion.command.stage_count != 1 ||
          command->assignment || command->background || command->capture || stage->redirection_count) {
        status = CALL_BAD_REQUEST;
      } else {
        status = shell_capture_destination(shell, destination, true);
        if (status == CALL_OK) {
          status = paired_value(shell, stage, destination, result);
        }
      }
    } else if (command->assignment || command->background || command->capture) {
      status = CALL_BAD_OPERATION;
    } else {
      for (size_t j = 0; j < expansion.command.stage_count; ++j) {
        const struct shell_stage *part = &expansion.command.stages[j];
        if (!part->count || shell_builtin(part->arguments, part->count)) {
          status = CALL_BAD_OPERATION;
          break;
        }
      }
      if (status == CALL_OK && !output.collector) {
        status = output_create(shell->retention_account, &output);
      }
      if (status == CALL_OK) {
        bool started = false, evaluated = false;
        bool earlier = result->evaluated;
        enum call_status error = CALL_OK;
        enum command_result command_result = shell_launch_capture(shell, &expansion.command,
            output.producer, &result->outcome, &evaluated, &started, &error);
        result->evaluated |= evaluated;
        if (command_result == COMMAND_FATAL) {
          status = CALL_OUTCOME_UNKNOWN;
        } else if (!started) {
          if (earlier) {
            result->command = COMMAND_FAILED;
          } else if (evaluated) {
            status = failed_value(shell, name, error, result);
          } else {
            status = error == CALL_OK ? CALL_BAD_REQUEST : error;
          }
          /* Keep earlier output; only an attempted OPEN/launch with no prior
           * result needs an immutable failure instead of successful emptiness. */
          if (!shell_expansion_close(&expansion)) {
            status = CALL_OUTCOME_UNKNOWN;
          }
          break;
        } else if (command_result != COMMAND_OK) {
          result->command = COMMAND_FAILED;
        }
      }
    }
    if (!shell_expansion_close(&expansion)) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    if (status != CALL_OK && status != CALL_OUTCOME_UNKNOWN && result->evaluated &&
        !result->value.handle) {
      result->command = COMMAND_FAILED;
      status = CALL_OK;
      break;
    }
    if (management || status != CALL_OK ||
        result->outcome.kind == TERMINAL_COMPLETION_TERMINATED ||
        result->outcome.kind == TERMINAL_COMPLETION_FAULTED ||
        (shell->script_name && result->command != COMMAND_OK)) {
      break;
    }
  }
  shell_syntax_close(&syntax);
  if (output.producer && handle_close(output.producer)) {
    status = CALL_OUTCOME_UNKNOWN;
  }
  if (status == CALL_OK && !result->value.handle && output.collector) {
    if (result->outcome.kind == TERMINAL_COMPLETION_TERMINATED ||
        result->outcome.kind == TERMINAL_COMPLETION_FAULTED) {
      status = failed_value(shell, name, CALL_ABANDONED, result);
    } else {
      status = take_value(shell, output.collector, name, result);
    }
  }
  if (output.collector && handle_close(output.collector)) {
    status = CALL_OUTCOME_UNKNOWN;
  }
  if (status != CALL_OK && !shell_capture_close(result)) {
    status = CALL_OUTCOME_UNKNOWN;
  }
  return status;
}
