#include "context.h"
#include "../xfer/hash.h"
#include "../installer/progress.h"
#include <startup.h>
#include <term.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool line(const char *prompt, char *bytes, size_t capacity)
{
  if (isatty(fileno(stdin)) && isatty(fileno(stderr))) {
    struct terminal terminal = {
      startup_stream(STARTUP_STDIN).handle,
      startup_stream(STARTUP_STDERR).handle,
    };
    struct term_line_result result = term_read_line(&terminal, prompt, bytes, capacity);
    return result.status == TERM_LINE_OK && !result.limit_reached;
  }
  fputs(prompt, stderr);
  if (!fgets(bytes, capacity, stdin)) {
    return false;
  }
  size_t length = strlen(bytes);
  if (!length || bytes[length - 1] != '\n') {
    return false;
  }
  bytes[length - 1] = '\0';
  return true;
}

static bool confirm(void)
{
  char answer[64];
  return line("Type config to publish this reviewed set, or anything else to cancel: ",
      answer, sizeof(answer)) && !strcmp(answer, "config");
}

static void review(const struct config_delta_plan *plan)
{
  for (size_t i = 0; i < plan->count; ++i) {
    config_delta_plan_review(plan, i, stderr);
  }
  for (size_t i = 0; i < CONFIG_FILE_COUNT; ++i) {
    if (plan->files[i].print_only) {
      fprintf(stderr, "config: %s untouched: %s\n", config_file_name(i), plan->files[i].reason);
    }
  }
}

static bool print_plan(struct config_editor_context *context)
{
  if (!context->target.valid) {
    review(&context->plan);
    fputs("config: no issuer-bound target; cannot print a replayable plan\n", stderr);
    return false;
  }
  struct config_replay_input inputs[CONFIG_REPLAY_INPUTS];
  config_editor_hashes(context, inputs);
  return config_replay_write(stdout, context->target_text, inputs, &context->plan);
}

static bool edit(struct config_editor_context *context)
{
  if (!context->writer || !context->target.valid) {
    fputs("config: edit refused: declared writer and stable ESP facts are required\n", stderr);
    return false;
  }
  for (size_t i = 0; i < context->plan.count; ++i) {
    config_delta_plan_review(&context->plan, i, stderr);
    char answer[64];
    for (;;) {
      if (!line("Choice [Enter keeps shown choice, keep/apply/adopt/remove, cancel]: ",
          answer, sizeof(answer)) || !strcmp(answer, "cancel")) {
        return false;
      }
      if (!answer[0]) {
        break;
      }
      bool chosen = false;
      for (unsigned choice = CONFIG_CHOICE_KEEP; choice <= CONFIG_CHOICE_REMOVE; ++choice) {
        if (!strcmp(answer, config_plan_choice_name(choice)) &&
            config_delta_plan_choose(&context->plan, i, choice)) {
          chosen = true;
          break;
        }
      }
      if (chosen) {
        break;
      }
      fputs("config: choice unavailable for this change\n", stderr);
    }
  }
  review(&context->plan);
  char action[64];
  if (!line("Final action [apply/print/cancel]: ", action, sizeof(action))) {
    return false;
  }
  if (!strcmp(action, "print")) {
    return print_plan(context);
  }
  if (!strcmp(action, "apply")) {
    return confirm() && config_editor_publish(context);
  }
  return false;
}

int main(int argc, char **argv)
{
  bool noninteractive = argc == 3 && !strcmp(argv[2], "--confirm=config");
  if (argc < 2 || argc > 3 || (argc == 3 && (!noninteractive || strcmp(argv[1], "apply"))) ||
      (strcmp(argv[1], "validate") && strcmp(argv[1], "plan") && strcmp(argv[1], "edit") && strcmp(argv[1], "apply"))) {
    fputs("usage: config validate | plan | edit | apply [--confirm=config]\n", stderr);
    return EXIT_FAILURE;
  }
  if (!hash_init(startup_resource("clock"), startup_resource("random"))) {
    fputs("config: declared clock/random unavailable for source hashing\n", stderr);
    hash_close();
    return EXIT_FAILURE;
  }
  struct config_editor_context context;
  install_progress_plain_stream(startup_resource("clock"), stderr);
  bool ok = config_editor_load(&context);
  if (ok && !strcmp(argv[1], "validate")) {
    ok = config_editor_validate(&context, false);
  } else if (ok && config_editor_plan(&context)) {
    if (!strcmp(argv[1], "plan")) {
      review(&context.plan);
      ok = print_plan(&context);
    } else if (!strcmp(argv[1], "edit")) {
      ok = edit(&context);
    } else {
      struct config_replay replay;
      struct config_replay_input inputs[CONFIG_REPLAY_INPUTS];
      config_editor_hashes(&context, inputs);
      ok = context.writer && context.target.valid && config_replay_read(stdin, &replay) &&
          config_replay_matches(&replay, context.target_text, inputs, &context.plan);
      if (ok) {
        review(&context.plan);
        ok = (noninteractive || confirm()) && config_editor_publish(&context);
      } else {
        fputs("config: apply refused: absent authority/evidence, stale inputs or unavailable choices\n", stderr);
      }
    }
  } else {
    ok = false;
  }
  config_editor_close(&context);
  install_progress_finish();
  hash_close();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
