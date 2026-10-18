#include "syntax.h"
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool shell_expansion_close(struct shell_expansion *expansion)
{
  bool closed = true;
  for (size_t i = 0; expansion->captures && i < expansion->value_count; ++i) {
    if (!shell_capture_close(&expansion->captures[i])) {
      closed = false;
    }
  }
  for (size_t i = 0; expansion->values && i < expansion->value_count; ++i) {
    struct environment_value *value = &expansion->values[i];
    if (value->handle && handle_close(value->handle)) {
      closed = false;
    }
    free(value->text);
  }
  for (size_t i = 0; expansion->arguments && i < expansion->argument_count; ++i) {
    free(expansion->arguments[i]);
  }
  for (size_t i = 0; i < LAUNCH_BATCH_MAX; ++i) {
    /* Selected entries borrow the owned per-word values above. */
    free(expansion->selected[i].values);
  }
  free(expansion->arguments);
  free(expansion->values);
  free(expansion->captures);
  free(expansion->words);
  *expansion = (struct shell_expansion){0};
  return closed;
}

static const char *expansion_error(struct shell_expansion *expansion,
    enum call_status *status, const char *diagnostic)
{
  struct shell_outcome outcome = {0};
  for (size_t i = 0; expansion->captures && i < expansion->value_count; ++i) {
    struct shell_outcome captured = expansion->captures[i].outcome;
    if (captured.kind == TERMINAL_COMPLETION_TERMINATED ||
        captured.kind == TERMINAL_COMPLETION_FAULTED) {
      outcome = captured;
      break;
    }
  }
  bool closed = shell_expansion_close(expansion);
  expansion->failure_outcome = outcome;
  if (!closed) {
    *status = CALL_OUTCOME_UNKNOWN;
    return "Could not release the expansion's owned capabilities";
  }
  return diagnostic;
}

static enum call_status lookup_value(handle_t snapshot, const char *name,
    struct environment_value *value)
{
  struct environment_entry_reply entry;
  enum call_status status = environment_lookup(snapshot, name, &entry, NULL, 0);
  char *text = NULL;
  if (status == CALL_BUFFER_TOO_SMALL && entry.kind == ENVIRONMENT_TEXT) {
    if (entry.text_length >= SIZE_MAX) {
      return CALL_LIMIT;
    }
    text = malloc(entry.text_length + 1);
    if (!text) {
      return CALL_NO_MEMORY;
    }
    /* The snapshot is immutable: this retrieves the same captured publication,
     * and a CAP lookup above already returned its single owned reference. */
    status = environment_lookup(snapshot, name, &entry, text, entry.text_length + 1);
  }
  if (status != CALL_OK) {
    free(text);
    return status;
  }
  *value = (struct environment_value){
    .kind = entry.kind, .flags = entry.flags, .identity = entry.identity,
    .handle = entry.handle, .rights = entry.rights, .transport = entry.transport,
    .text = text, .length = entry.text_length,
  };
  memcpy(value->name, entry.name, sizeof(value->name));
  return CALL_OK;
}

static const char *append_text(char **text, size_t *length,
    const char *addition, size_t count)
{
  if (count > SIZE_MAX - *length - 1) {
    return "Expanded word is too large";
  }
  char *grown = realloc(*text, *length + count + 1);
  if (!grown) {
    return "Out of memory";
  }
  if (count) {
    memcpy(grown + *length, addition, count);
  }
  *length += count;
  grown[*length] = 0;
  *text = grown;
  return NULL;
}

/* ${N} digits were validated by the parser; an index beyond the arguments,
 * including one too large to represent, is missing. */
static const char *positional_argument(const struct shell *shell, const char *digits,
    const char **argument)
{
  size_t index = 0;
  for (const char *read = digits; *read; ++read) {
    size_t digit = (size_t)(*read - '0');
    if (index > (SIZE_MAX - digit) / 10) {
      return "Missing positional argument";
    }
    index = index * 10 + digit;
  }
  if (index > shell->argument_count) {
    return "Missing positional argument";
  }
  *argument = shell->arguments[index - 1];
  return NULL;
}

static const char *expand_word(struct shell *shell, handle_t snapshot,
    const struct shell_word *word, struct environment_value *value,
    struct shell_capture_result *capture, char **argument, enum call_status *status)
{
  char *text = NULL;
  size_t length = 0;
  bool capability = false;
  const char *error = NULL;
  for (size_t i = 0; i < word->count; ++i) {
    const struct shell_piece *piece = &word->pieces[i];
    if (piece->kind == SHELL_LITERAL || piece->kind == SHELL_POSITIONAL ||
        piece->kind == SHELL_ARGUMENT_COUNT) {
      const char *addition = piece->text;
      char count[24];
      if (piece->kind == SHELL_POSITIONAL) {
        error = positional_argument(shell, piece->text, &addition);
      } else if (piece->kind == SHELL_ARGUMENT_COUNT) {
        snprintf(count, sizeof(count), "%zu", shell->argument_count);
        addition = count;
      }
      if (!error && capability && *addition) {
        error = "A CAP expansion must occupy the entire word";
      }
      if (!error) {
        error = append_text(&text, &length, addition, strlen(addition));
      }
      if (error) {
        break;
      }
    } else if (piece->kind == SHELL_ALL_ARGUMENTS) {
      error = "\"$@\" can only supply command arguments";
      break;
    } else {
      struct environment_value selected = {0};
      if (piece->kind == SHELL_SUBSTITUTION) {
        *status = shell_capture_evaluate(shell, piece->text,
            word->assignment_name, capture);
        if (*status != CALL_OK) {
          error = "Command substitution could not prepare its result";
          break;
        }
        *value = capture->value;
        capture->value = (struct environment_value){0};
        if (capture->command == COMMAND_FATAL) {
          *status = CALL_OUTCOME_UNKNOWN;
          error = "Command substitution could not complete cleanup";
          break;
        }
        if (!word->assignment_name && capture->command != COMMAND_OK) {
          if (capture->outcome.kind == TERMINAL_COMPLETION_TERMINATED) {
            *status = CALL_ABANDONED;
          }
          error = "Inline command substitution failed";
          break;
        }
        selected = *value;
        *value = (struct environment_value){0};
      } else {
        *status = lookup_value(snapshot, piece->text, &selected);
        if (*status != CALL_OK) {
          error = "Environment expansion lookup failed";
          break;
        }
      }
      if (selected.kind == ENVIRONMENT_CAP) {
        if (capability || length || word->count != 1) {
          if (handle_close(selected.handle)) {
            *status = CALL_OUTCOME_UNKNOWN;
          }
          error = "A CAP expansion must occupy the entire word";
          break;
        }
        *value = selected;
        capability = true;
      } else if (selected.kind == ENVIRONMENT_TEXT) {
        if (capability) {
          error = "A CAP expansion must occupy the entire word";
        } else {
          error = append_text(&text, &length, selected.text, selected.length);
        }
        free(selected.text);
      } else {
        *value = selected;
        error = "Unsupported environment value kind";
      }
    }
    if (error) {
      break;
    }
  }
  if (!error && !capability) {
    error = append_text(&text, &length, "", 0);
    if (!error) {
      *value = (struct environment_value){.kind = ENVIRONMENT_TEXT,
          .text = text, .length = length};
      text = NULL;
    }
  }
  if (!error) {
    size_t prefix = word->assignment_name ? strlen(word->assignment_name) + 1 : 0;
    size_t value_length = capability ? strlen(value->name) + 3 : value->length;
    if (prefix > SIZE_MAX - value_length - 1) {
      error = "Expanded word is too large";
    } else {
      *argument = malloc(prefix + value_length + 1);
      if (!*argument) {
        error = "Out of memory";
      } else {
        if (prefix) {
          memcpy(*argument, word->assignment_name, prefix - 1);
          (*argument)[prefix - 1] = '=';
        }
        if (capability) {
          size_t name_length = strlen(value->name);
          memcpy(*argument + prefix, value->name, name_length);
          memcpy(*argument + prefix + name_length, "://", 4);
        } else {
          memcpy(*argument + prefix, value->text, value_length + 1);
        }
      }
    }
  }
  free(text);
  return error;
}

static bool all_arguments(const struct shell_word *word)
{
  return word->count == 1 && word->pieces[0].kind == SHELL_ALL_ARGUMENTS;
}

/* Each argument becomes its own TEXT word; arguments are never CAPs. */
static const char *expand_all_arguments(const struct shell *shell,
    struct environment_value *values, char **arguments)
{
  for (size_t i = 0; i < shell->argument_count; ++i) {
    size_t length = strlen(shell->arguments[i]);
    values[i] = (struct environment_value){.kind = ENVIRONMENT_TEXT,
        .text = malloc(length + 1), .length = length};
    arguments[i] = malloc(length + 1);
    if (!values[i].text || !arguments[i]) {
      return "Out of memory";
    }
    memcpy(values[i].text, shell->arguments[i], length + 1);
    memcpy(arguments[i], shell->arguments[i], length + 1);
  }
  return NULL;
}

static const char *select_capability(struct environment_capture *capture,
    const struct environment_value *value)
{
  for (size_t i = 0; i < capture->count; ++i) {
    if (!strcmp(capture->values[i].name, value->name)) {
      if (capture->values[i].identity != value->identity) {
        return "Conflicting captured environment publications";
      }
      return NULL;
    }
  }
  struct environment_value *values = realloc(capture->values,
      (capture->count + 1) * sizeof(*values));
  if (!values) {
    return "Out of memory";
  }
  capture->values = values;
  values[capture->count++] = *value;
  return NULL;
}

const char *shell_syntax_expand(struct shell *shell,
    const struct shell_syntax_command *syntax, struct shell_expansion *expansion,
    enum call_status *status)
{
  *expansion = (struct shell_expansion){0};
  *status = CALL_OK;
  size_t redirections = 0;
  bool needs_environment = false;
  for (size_t i = 0; i < syntax->stage_count; ++i) {
    const struct shell_syntax_stage *stage = &syntax->stages[i];
    size_t stage_words = 0;
    for (size_t j = 0; j < stage->count; ++j) {
      stage_words += all_arguments(stage->words[j]) ? shell->argument_count : 1;
    }
    if (!stage_words) {
      return "Command expanded to no words";
    }
    expansion->word_count += stage_words;
    for (size_t j = 0; j < stage->redirection_count; ++j) {
      redirections += !stage->redirections[j].duplicate_stdout;
    }
    size_t total = stage->count + stage->redirection_count;
    for (size_t j = 0; j < total && !needs_environment; ++j) {
      const struct shell_word *word = j < stage->count ? stage->words[j] :
          stage->redirections[j - stage->count].word;
      for (size_t k = 0; word && k < word->count; ++k) {
        if (word->pieces[k].kind == SHELL_VARIABLE) {
          needs_environment = true;
          break;
        }
      }
    }
  }
  expansion->value_count = expansion->word_count + redirections;
  expansion->argument_count = expansion->value_count + syntax->stage_count;
  expansion->words = calloc(expansion->word_count, sizeof(*expansion->words));
  expansion->values = calloc(expansion->value_count, sizeof(*expansion->values));
  expansion->captures = calloc(expansion->value_count, sizeof(*expansion->captures));
  expansion->arguments = calloc(expansion->argument_count, sizeof(*expansion->arguments));
  if (!expansion->words || !expansion->values || !expansion->captures ||
      !expansion->arguments) {
    return expansion_error(expansion, status, "Out of memory");
  }
  for (size_t i = 0; i < syntax->stage_count; ++i) {
    const struct shell_syntax_stage *stage = &syntax->stages[i];
    for (size_t j = 0; j < stage->count; ++j) {
      const struct shell_word *word = stage->words[j];
      if (word->assignment_name && word->count == 1 &&
          word->pieces[0].kind == SHELL_SUBSTITUTION) {
        *status = shell_capture_destination(shell, word->assignment_name,
            word->pieces[0].management);
        if (*status != CALL_OK) {
          return expansion_error(expansion, status,
              "Command substitution destination admission failed");
        }
      }
    }
  }
  handle_t snapshot = HANDLE_INVALID;
  struct environment_info_reply info = {0};
  if (needs_environment) {
    *status = environment_snapshot(shell->environment,
        ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &snapshot, &info);
    if (*status != CALL_OK) {
      return expansion_error(expansion, status,
          "Could not snapshot the environment for expansion");
    }
  }
  expansion->command.stage_count = syntax->stage_count;
  expansion->command.background = syntax->background;
  size_t word_index = 0, argument_index = 0;
  const char *error = NULL;
  for (size_t i = 0; i < syntax->stage_count && !error; ++i) {
    const struct shell_syntax_stage *source = &syntax->stages[i];
    struct shell_stage *stage = &expansion->command.stages[i];
    stage->arguments = expansion->arguments + argument_index;
    stage->count = 0;
    stage->operands = &expansion->selected[i];
    expansion->selected[i].generation = info.generation;
    for (size_t j = 0; j < source->count && !error; ++j) {
      const struct shell_word *word = source->words[j];
      if (all_arguments(word)) {
        error = expand_all_arguments(shell, &expansion->values[word_index],
            &expansion->arguments[argument_index]);
        for (size_t k = 0; k < shell->argument_count; ++k) {
          expansion->words[word_index + k] = (struct shell_expanded_word){
              .value = &expansion->values[word_index + k]};
        }
        word_index += shell->argument_count;
        argument_index += shell->argument_count;
        stage->count += shell->argument_count;
        continue;
      }
      ++stage->count;
      struct environment_value *value = &expansion->values[word_index];
      struct shell_capture_result *capture = &expansion->captures[word_index];
      bool substitution = word->count == 1 &&
          word->pieces[0].kind == SHELL_SUBSTITUTION;
      expansion->words[word_index] = (struct shell_expanded_word){
          .assignment_name = word->assignment_name, .value = value,
          .capture = substitution ? capture : NULL};
      error = expand_word(shell, snapshot, word, value, capture,
          &expansion->arguments[argument_index++], status);
      if (!error && value->kind == ENVIRONMENT_CAP && !word->assignment_name) {
        error = select_capability(&expansion->selected[i], value);
        if (!error && substitution && capture->observation.kind == ENVIRONMENT_CAP) {
          error = select_capability(&expansion->selected[i], &capture->observation);
        }
      }
      ++word_index;
    }
    ++argument_index; /* calloc supplied the stage's argv terminator. */
  }
  size_t value_index = expansion->word_count;
  for (size_t i = 0; i < syntax->stage_count && !error; ++i) {
    const struct shell_syntax_stage *source = &syntax->stages[i];
    struct shell_stage *stage = &expansion->command.stages[i];
    stage->redirection_count = source->redirection_count;
    for (size_t j = 0; j < source->redirection_count && !error; ++j) {
      const struct shell_syntax_redirection *redirection = &source->redirections[j];
      if (redirection->duplicate_stdout) {
        stage->redirections[j] = (struct shell_redirection){
          .stream = ENVIRONMENT_STDERR, .duplicate_stdout = true,
        };
        continue;
      }
      struct environment_value *value = &expansion->values[value_index];
      struct shell_capture_result *capture = &expansion->captures[value_index++];
      char **path = &expansion->arguments[argument_index++];
      error = expand_word(shell, snapshot, redirection->word, value, capture, path, status);
      if (!error && !**path) {
        error = "Redirection requires a filename";
      }
      stage->redirections[j] = (struct shell_redirection){
        .stream = redirection->stream, .path = *path, .append = redirection->append,
        .value = value->kind == ENVIRONMENT_CAP ? value : NULL,
      };
    }
  }
  if (snapshot != HANDLE_INVALID && handle_close(snapshot)) {
    *status = CALL_OUTCOME_UNKNOWN;
    error = "Could not release the expansion snapshot";
  }
  return error ? expansion_error(expansion, status, error) : NULL;
}
