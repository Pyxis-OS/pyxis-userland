#include "syntax.h"
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A pattern word is the expanded text of the word with a mark on every byte that
 * was typed unquoted and literal: only those bytes can be wildcards, so text from
 * a variable or a quoted or escaped byte is always an ordinary character. `*` is
 * the only wildcard, and only the last path component may hold one. */

enum token_kind { TOKEN_BYTE, TOKEN_ANY };

struct token {
  enum token_kind kind;
  unsigned char byte;
};

struct pattern {
  char *text;
  bool *active;
  size_t length, capacity;
};

static void pattern_free(struct pattern *pattern)
{
  free(pattern->text);
  free(pattern->active);
  *pattern = (struct pattern){0};
}

static bool pattern_append(struct pattern *pattern, const char *text, size_t length,
    bool active)
{
  if (length > SIZE_MAX - pattern->length - 1) {
    return false;
  }
  size_t needed = pattern->length + length + 1;
  if (needed > pattern->capacity) {
    size_t capacity = pattern->capacity ? pattern->capacity : 64;
    while (capacity < needed) {
      if (capacity > SIZE_MAX / 2) {
        return false;
      }
      capacity *= 2;
    }
    char *larger = realloc(pattern->text, capacity);
    if (!larger) {
      return false;
    }
    pattern->text = larger;
    bool *marks = realloc(pattern->active, capacity * sizeof(*marks));
    if (!marks) {
      return false;
    }
    pattern->active = marks;
    pattern->capacity = capacity;
  }
  memcpy(pattern->text + pattern->length, text, length);
  for (size_t i = 0; i < length; ++i) {
    pattern->active[pattern->length + i] = active;
  }
  pattern->length += length;
  pattern->text[pattern->length] = 0;
  return true;
}

static const char *message(struct shell *shell, const char *format, const char *subject)
{
  snprintf(shell->glob_message, sizeof(shell->glob_message), format, subject);
  return shell->glob_message;
}

/* The word's text with its marks. A command substitution cannot take part: it
 * would run before the command is known to be a pattern. */
static const char *pattern_build(struct shell *shell, handle_t snapshot,
    const struct shell_word *word, struct pattern *pattern, enum call_status *status)
{
  for (size_t i = 0; i < word->count; ++i) {
    const struct shell_piece *piece = &word->pieces[i];
    const char *addition = piece->text;
    char count[24];
    char *owned = NULL;
    bool ok = true;
    if (piece->kind == SHELL_SUBSTITUTION) {
      return "A wildcard cannot share a word with a command substitution; quote one";
    } else if (piece->kind == SHELL_POSITIONAL) {
      const char *error = shell_positional_argument(shell, piece->text, &addition);
      if (error) {
        return error;
      }
    } else if (piece->kind == SHELL_ARGUMENT_COUNT) {
      snprintf(count, sizeof(count), "%zu", shell->argument_count);
      addition = count;
    } else if (piece->kind == SHELL_VARIABLE) {
      struct environment_value value = {0};
      *status = shell_lookup_value(snapshot, piece->text, &value);
      if (*status != CALL_OK) {
        return "Environment expansion lookup failed";
      }
      if (value.kind != ENVIRONMENT_TEXT) {
        if (value.kind == ENVIRONMENT_CAP && handle_close(value.handle)) {
          *status = CALL_OUTCOME_UNKNOWN;
        }
        free(value.text);
        return "A CAP expansion must occupy the entire word";
      }
      owned = value.text;
      addition = owned ? owned : "";
    }
    ok = pattern_append(pattern, addition, strlen(addition),
        piece->kind == SHELL_LITERAL && piece->unquoted);
    free(owned);
    if (!ok) {
      *status = CALL_NO_MEMORY;
      return message(shell, "Out of memory expanding %s", "a wildcard word");
    }
  }
  return NULL;
}

enum classification { NOT_PATTERN, IS_PATTERN, REFUSED };

/* Splits the pattern at its last '/', and turns the last component into tokens.
 * tokens must have room for the component's length. */
static enum classification classify(struct shell *shell, const struct pattern *pattern,
    struct token *tokens, size_t *token_count, size_t *directory_length,
    const char **error)
{
  size_t directory = 0;
  for (size_t i = 0; i < pattern->length; ++i) {
    if (pattern->text[i] == '/') {
      directory = i + 1;
    }
  }
  for (size_t i = 0; i < directory; ++i) {
    if (pattern->active[i] && pattern->text[i] == '*') {
      *error = message(shell, "Wildcards are supported only in the last path component: %s",
          pattern->text);
      return REFUSED;
    }
  }
  size_t count = 0;
  bool wildcard = false;
  for (size_t i = directory; i < pattern->length; ++i) {
    struct token token = {.kind = TOKEN_BYTE, .byte = (unsigned char)pattern->text[i]};
    if (pattern->active[i] && pattern->text[i] == '*') {
      wildcard = true;
      if (count && tokens[count - 1].kind == TOKEN_ANY) {
        continue;
      }
      token.kind = TOKEN_ANY;
    }
    tokens[count++] = token;
  }
  *token_count = count;
  *directory_length = directory;
  return wildcard ? IS_PATTERN : NOT_PATTERN;
}

static bool name_matches(const struct token *tokens, size_t count, const char *name)
{
  const unsigned char *text = (const unsigned char *)name;
  size_t token = 0, read = 0, star_token = SIZE_MAX, star_read = 0;
  while (text[read]) {
    if (token < count && tokens[token].kind == TOKEN_ANY) {
      star_token = token++;
      star_read = read;
    } else if (token < count && text[read] == tokens[token].byte) {
      ++token;
      ++read;
    } else if (star_token != SIZE_MAX) {
      /* The star takes one more byte and the rest is tried again. UTF-8 is
       * self-synchronizing, so a literal can never match inside a character. */
      read = ++star_read;
      token = star_token + 1;
    } else {
      return false;
    }
  }
  while (token < count && tokens[token].kind == TOKEN_ANY) {
    ++token;
  }
  return token == count;
}

static int compare_names(const void *left, const void *right)
{
  return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static struct shell_word *literal_word(const char *directory, size_t directory_length,
    const char *name)
{
  size_t name_length = strlen(name);
  struct shell_word *word = calloc(1, sizeof(*word));
  struct shell_piece *piece = calloc(1, sizeof(*piece));
  char *text = NULL;
  if (word && piece && directory_length <= SIZE_MAX - name_length - 1) {
    text = malloc(directory_length + name_length + 1);
  }
  if (!text) {
    free(word);
    free(piece);
    return NULL;
  }
  memcpy(text, directory, directory_length);
  memcpy(text + directory_length, name, name_length + 1);
  *piece = (struct shell_piece){.kind = SHELL_LITERAL, .text = text};
  *word = (struct shell_word){.pieces = piece, .count = 1};
  return word;
}

void shell_glob_close(struct shell_glob *glob)
{
  for (size_t i = 0; i < glob->word_count; ++i) {
    free(glob->words[i]->pieces->text);
    free(glob->words[i]->pieces);
    free(glob->words[i]);
  }
  for (size_t i = 0; i < glob->array_count; ++i) {
    free(glob->arrays[i]);
  }
  free(glob->words);
  free(glob->arrays);
  free(glob->command);
  *glob = (struct shell_glob){0};
}

/* One stage's words while patterns are replaced by their matches. */
struct words {
  struct shell_word **items;
  size_t count, capacity;
};

static bool words_reserve(struct words *words, size_t extra)
{
  if (extra > SIZE_MAX - words->count) {
    return false;
  }
  size_t needed = words->count + extra;
  if (needed <= words->capacity) {
    return true;
  }
  size_t capacity = words->capacity ? words->capacity : 8;
  while (capacity < needed) {
    if (capacity > SIZE_MAX / 2) {
      return false;
    }
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(*words->items)) {
    return false;
  }
  struct shell_word **larger = realloc(words->items, capacity * sizeof(*larger));
  if (!larger) {
    return false;
  }
  words->items = larger;
  words->capacity = capacity;
  return true;
}

static bool glob_own(struct shell_glob *glob, struct shell_word *word)
{
  if (glob->word_count == glob->word_capacity) {
    size_t capacity = glob->word_capacity ? glob->word_capacity : 16;
    if (glob->word_capacity && capacity > SIZE_MAX / 2) {
      return false;
    }
    capacity = glob->word_capacity ? capacity * 2 : capacity;
    if (capacity > SIZE_MAX / sizeof(*glob->words)) {
      return false;
    }
    struct shell_word **larger = realloc(glob->words, capacity * sizeof(*larger));
    if (!larger) {
      return false;
    }
    glob->words = larger;
    glob->word_capacity = capacity;
  }
  glob->words[glob->word_count++] = word;
  return true;
}

/* Appends one word per match of the pattern, sorted by name. */
static const char *expand_pattern(struct shell *shell, struct shell_glob *glob,
    const struct pattern *pattern, const struct token *tokens, size_t token_count,
    size_t directory_length, struct words *words, enum call_status *status)
{
  char *directory = malloc(directory_length + 2);
  if (!directory) {
    *status = CALL_NO_MEMORY;
    return message(shell, "Out of memory expanding %s", pattern->text);
  }
  memcpy(directory, pattern->text, directory_length);
  directory[directory_length] = 0;
  struct shell_listing listing;
  *status = shell_list_directory(shell, directory_length ? directory : ".", 0, &listing);
  if (*status != CALL_OK) {
    free(directory);
    return message(shell, "Cannot list the directory for %s", pattern->text);
  }
  const char **matches = calloc(listing.count ? listing.count : 1, sizeof(*matches));
  size_t matched = 0;
  const char *error = NULL;
  if (!matches) {
    *status = CALL_NO_MEMORY;
    error = message(shell, "Out of memory expanding %s", pattern->text);
  }
  /* A leading dot is matched only by a leading dot in the pattern. */
  bool dot = token_count && tokens[0].kind == TOKEN_BYTE && tokens[0].byte == '.';
  for (size_t i = 0; !error && i < listing.count; ++i) {
    const char *name = shell_listing_name(&listing, i);
    if ((name[0] != '.' || dot) && name_matches(tokens, token_count, name)) {
      matches[matched++] = name;
    }
  }
  if (!error && !matched) {
    error = message(shell, "No match: %s (quote it to pass it literally)", pattern->text);
  } else if (!error && !words_reserve(words, matched)) {
    *status = CALL_NO_MEMORY;
    error = message(shell, "Out of memory expanding %s", pattern->text);
  }
  if (!error) {
    qsort(matches, matched, sizeof(*matches), compare_names);
  }
  for (size_t i = 0; !error && i < matched; ++i) {
    struct shell_word *word = literal_word(directory, directory_length, matches[i]);
    if (word && !glob_own(glob, word)) {
      free(word->pieces->text);
      free(word->pieces);
      free(word);
      word = NULL;
    }
    if (!word) {
      *status = CALL_NO_MEMORY;
      error = message(shell, "Out of memory expanding %s", pattern->text);
      break;
    }
    words->items[words->count++] = word;
  }
  free(matches);
  shell_listing_close(&listing);
  free(directory);
  return error;
}

static bool has_glob(const struct shell_syntax_command *syntax, bool *variables)
{
  bool found = false;
  for (size_t i = 0; i < syntax->stage_count; ++i) {
    const struct shell_syntax_stage *stage = &syntax->stages[i];
    size_t total = stage->count + stage->redirection_count;
    for (size_t j = 0; j < total; ++j) {
      const struct shell_word *word = j < stage->count ? stage->words[j] :
          stage->redirections[j - stage->count].word;
      if (!word || !word->glob || word->assignment_name) {
        continue;
      }
      found = true;
      for (size_t k = 0; k < word->count; ++k) {
        *variables = *variables || word->pieces[k].kind == SHELL_VARIABLE;
      }
    }
  }
  return found;
}

/* Builds the word's pattern and classifies it. REFUSED sets *error. */
static enum classification examine(struct shell *shell, handle_t snapshot,
    const struct shell_word *word, struct pattern *pattern, struct token **tokens,
    size_t *token_count, size_t *directory_length, const char **error,
    enum call_status *status)
{
  *error = pattern_build(shell, snapshot, word, pattern, status);
  if (*error) {
    return REFUSED;
  }
  *tokens = calloc(pattern->length + 1, sizeof(**tokens));
  if (!*tokens) {
    *status = CALL_NO_MEMORY;
    *error = message(shell, "Out of memory expanding %s", pattern->text ? pattern->text : "");
    return REFUSED;
  }
  return classify(shell, pattern, *tokens, token_count, directory_length, error);
}

const char *shell_glob_prepare(struct shell *shell,
    const struct shell_syntax_command *syntax, struct shell_glob *glob,
    enum call_status *status)
{
  bool variables = false;
  if (!has_glob(syntax, &variables)) {
    return NULL;
  }
  handle_t snapshot = HANDLE_INVALID;
  if (variables) {
    struct environment_info_reply info;
    *status = environment_snapshot(shell->environment,
        ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &snapshot, &info);
    if (*status != CALL_OK) {
      return "Could not snapshot the environment for expansion";
    }
  }
  glob->command = malloc(sizeof(*glob->command));
  const char *error = NULL;
  if (!glob->command) {
    *status = CALL_NO_MEMORY;
    error = "Out of memory";
  } else {
    *glob->command = *syntax;
  }
  for (size_t i = 0; !error && i < syntax->stage_count; ++i) {
    const struct shell_syntax_stage *stage = &syntax->stages[i];
    struct words words = {0};
    bool replaced = false;
    for (size_t j = 0; !error && j < stage->count; ++j) {
      struct shell_word *word = stage->words[j];
      struct pattern pattern = {0};
      struct token *tokens = NULL;
      size_t token_count = 0, directory_length = 0;
      enum classification kind = NOT_PATTERN;
      if (word->glob && !word->assignment_name) {
        kind = examine(shell, snapshot, word, &pattern, &tokens, &token_count,
            &directory_length, &error, status);
      }
      if (kind == IS_PATTERN && !j) {
        error = message(shell, "A wildcard cannot select the command: %s", pattern.text);
      } else if (kind == IS_PATTERN) {
        error = expand_pattern(shell, glob, &pattern, tokens, token_count,
            directory_length, &words, status);
        replaced = true;
      } else if (!error && words_reserve(&words, 1)) {
        words.items[words.count++] = word;
      } else if (!error) {
        *status = CALL_NO_MEMORY;
        error = "Out of memory";
      }
      free(tokens);
      pattern_free(&pattern);
    }
    for (size_t j = 0; !error && j < stage->redirection_count; ++j) {
      const struct shell_word *word = stage->redirections[j].word;
      struct pattern pattern = {0};
      struct token *tokens = NULL;
      size_t token_count = 0, directory_length = 0;
      if (word && word->glob && !word->assignment_name &&
          examine(shell, snapshot, word, &pattern, &tokens, &token_count,
              &directory_length, &error, status) == IS_PATTERN) {
        error = message(shell, "A wildcard cannot select a redirection target: %s",
            pattern.text);
      }
      free(tokens);
      pattern_free(&pattern);
    }
    if (!error && replaced) {
      glob->command->stages[i].words = words.items;
      glob->command->stages[i].count = words.count;
      if (glob->array_count > SIZE_MAX / sizeof(*glob->arrays) - 1) {
        error = "Out of memory";
      } else {
        struct shell_word ***larger = realloc(glob->arrays,
            (glob->array_count + 1) * sizeof(*larger));
        if (larger) {
          glob->arrays = larger;
          larger[glob->array_count++] = words.items;
          words.items = NULL;
        } else {
          *status = CALL_NO_MEMORY;
          error = "Out of memory";
        }
      }
    }
    free(words.items);
  }
  if (snapshot != HANDLE_INVALID && handle_close(snapshot)) {
    *status = CALL_OUTCOME_UNKNOWN;
    error = "Could not release the expansion snapshot";
  }
  return error;
}
