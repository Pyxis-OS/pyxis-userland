#include "syntax.h"
#include <handle.h>
#include <stdlib.h>
#include <string.h>

void shell_expansion_close(struct shell_expansion *expansion)
{
  for (size_t i = 0; expansion->values && i < expansion->value_count; ++i) {
    struct environment_value *value = &expansion->values[i];
    if (value->handle) {
      handle_close(value->handle);
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
  free(expansion->words);
  *expansion = (struct shell_expansion){0};
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

static const char *expand_word(handle_t snapshot, const struct shell_word *word,
    struct environment_value *value, char **argument, enum call_status *status)
{
  char *text = NULL;
  size_t length = 0;
  bool capability = false;
  const char *error = NULL;
  for (size_t i = 0; i < word->count; ++i) {
    const struct shell_piece *piece = &word->pieces[i];
    if (piece->kind == SHELL_SUBSTITUTION) {
      error = "Command substitution requires the pending capture prerequisites";
      break;
    }
    if (piece->kind == SHELL_LITERAL) {
      if (capability && *piece->text) {
        error = "A CAP expansion must occupy the entire word";
        break;
      }
      error = append_text(&text, &length, piece->text, strlen(piece->text));
    } else {
      struct environment_value selected = {0};
      *status = lookup_value(snapshot, piece->text, &selected);
      if (*status != CALL_OK) {
        error = "Environment expansion lookup failed";
        break;
      }
      if (selected.kind == ENVIRONMENT_CAP) {
        if (capability || length || word->count != 1) {
          handle_close(selected.handle);
          error = "A CAP expansion must occupy the entire word";
          break;
        }
        capability = true;
        *value = selected;
      } else if (selected.kind == ENVIRONMENT_TEXT) {
        if (capability) {
          error = "A CAP expansion must occupy the entire word";
        } else {
          error = append_text(&text, &length, selected.text, selected.length);
        }
        free(selected.text);
      } else {
        if (selected.handle) {
          handle_close(selected.handle);
        }
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
    expansion->word_count += stage->count;
    redirections += stage->redirection_count;
    size_t total = stage->count + stage->redirection_count;
    for (size_t j = 0; j < total && !needs_environment; ++j) {
      const struct shell_word *word = j < stage->count ? stage->words[j] :
          stage->redirections[j - stage->count].word;
      for (size_t k = 0; k < word->count; ++k) {
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
  expansion->arguments = calloc(expansion->argument_count, sizeof(*expansion->arguments));
  if (!expansion->words || !expansion->values || !expansion->arguments) {
    shell_expansion_close(expansion);
    return "Out of memory";
  }
  handle_t snapshot = HANDLE_INVALID;
  struct environment_info_reply info = {0};
  if (needs_environment) {
    *status = environment_snapshot(shell->environment,
        ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &snapshot, &info);
    if (*status != CALL_OK) {
      shell_expansion_close(expansion);
      return "Could not snapshot the environment for expansion";
    }
  }
  expansion->command.stage_count = syntax->stage_count;
  expansion->command.background = syntax->background;
  expansion->command.capture = syntax->capture;
  size_t word_index = 0, argument_index = 0;
  const char *error = NULL;
  for (size_t i = 0; i < syntax->stage_count && !error; ++i) {
    const struct shell_syntax_stage *source = &syntax->stages[i];
    struct shell_stage *stage = &expansion->command.stages[i];
    stage->arguments = expansion->arguments + argument_index;
    stage->count = source->count;
    stage->operands = &expansion->selected[i];
    expansion->selected[i].generation = info.generation;
    for (size_t j = 0; j < source->count && !error; ++j) {
      const struct shell_word *word = source->words[j];
      struct environment_value *value = &expansion->values[word_index];
      expansion->words[word_index] = (struct shell_expanded_word){
          .assignment_name = word->assignment_name, .value = value};
      error = expand_word(snapshot, word, value,
          &expansion->arguments[argument_index++], status);
      if (!error && value->kind == ENVIRONMENT_CAP && !word->assignment_name) {
        error = select_capability(&expansion->selected[i], value);
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
      struct environment_value *value = &expansion->values[value_index++];
      char **path = &expansion->arguments[argument_index++];
      error = expand_word(snapshot, redirection->word, value, path, status);
      if (!error && !**path) {
        error = "Redirection requires a filename";
      }
      stage->redirections[j] = (struct shell_redirection){
        .stream = redirection->stream, .path = *path, .append = redirection->append,
        .value = value,
        .capability = redirection->word->plain && *path && **path == '@',
      };
    }
  }
  if (snapshot != HANDLE_INVALID && handle_close(snapshot) && !error) {
    *status = CALL_OUTCOME_UNKNOWN;
    error = "Could not release the expansion snapshot";
  }
  if (error) {
    shell_expansion_close(expansion);
  }
  return error;
}
