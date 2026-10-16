#include "syntax.h"
#include <stdlib.h>
#include <string.h>

static bool separator(char character)
{
  return character == ' ' || character == '\t' || character == '\n' ||
      character == '\r' || character == '\v' || character == '\f';
}

static bool letter(char character)
{
  return (character >= 'a' && character <= 'z') ||
      (character >= 'A' && character <= 'Z') || character == '_';
}

static bool digit(char character)
{
  return character >= '0' && character <= '9';
}

static bool name_character(char character)
{
  return letter(character) || digit(character) || character == '.' ||
      character == '+' || character == '-';
}

static char *copy_text(const char *text, size_t length)
{
  char *copy = malloc(length + 1);
  if (copy) {
    memcpy(copy, text, length);
    copy[length] = 0;
  }
  return copy;
}

static void close_word(struct shell_word *word)
{
  if (!word) {
    return;
  }
  for (size_t i = 0; i < word->count; ++i) {
    free(word->pieces[i].text);
  }
  free(word->pieces);
  free(word->assignment_name);
  free(word);
}

void shell_syntax_close(struct shell_syntax *syntax)
{
  for (size_t i = 0; i < syntax->count; ++i) {
    struct shell_syntax_command *command = &syntax->commands[i];
    for (size_t j = 0; j < command->stage_count; ++j) {
      struct shell_syntax_stage *stage = &command->stages[j];
      for (size_t k = 0; k < stage->count; ++k) {
        close_word(stage->words[k]);
      }
      free(stage->words);
      for (size_t k = 0; k < stage->redirection_count; ++k) {
        close_word(stage->redirections[k].word);
      }
    }
  }
  free(syntax->commands);
  *syntax = (struct shell_syntax){0};
}

static const char *add_piece(struct shell_word *word, enum shell_piece_kind kind,
    const char *text, size_t length)
{
  if (kind == SHELL_LITERAL && word->count &&
      word->pieces[word->count - 1].kind == SHELL_LITERAL) {
    struct shell_piece *last = &word->pieces[word->count - 1];
    size_t previous = strlen(last->text);
    char *combined = realloc(last->text, previous + length + 1);
    if (!combined) {
      return "Out of memory";
    }
    memcpy(combined + previous, text, length);
    combined[previous + length] = 0;
    last->text = combined;
    return NULL;
  }
  char *copy = copy_text(text, length);
  if (!copy) {
    return "Out of memory";
  }
  struct shell_piece *pieces = realloc(word->pieces,
      (word->count + 1) * sizeof(*pieces));
  if (!pieces) {
    free(copy);
    return "Out of memory";
  }
  word->pieces = pieces;
  pieces[word->count++] = (struct shell_piece){.kind = kind, .text = copy};
  return NULL;
}

static const char *parse_sequence(const char **cursor, bool nested,
    struct shell_syntax *syntax);
static bool word_is(const struct shell_word *word, const char *text);

static const char *capture_source(const struct shell_syntax *syntax, bool *management)
{
  *management = false;
  if (!syntax->count) {
    return "Command substitution requires a command";
  }
  for (size_t i = 0; i < syntax->count; ++i) {
    const struct shell_syntax_command *command = &syntax->commands[i];
    if (command->background) {
      return "Command substitution cannot run a background command";
    }
    for (size_t j = 0; j < command->stage_count; ++j) {
      const struct shell_syntax_stage *stage = &command->stages[j];
      if (word_is(stage->words[0], "revocable") ||
          (stage->count > 1 && word_is(stage->words[0], "service") &&
           word_is(stage->words[1], "start"))) {
        *management = true;
      }
    }
  }
  if (*management && (syntax->count != 1 || syntax->commands[0].stage_count != 1 ||
      syntax->commands[0].stages[0].redirection_count)) {
    return "Management capture requires a single command without redirection";
  }
  return NULL;
}

static const char *parse_word(const char **cursor, struct shell_word **result)
{
  struct shell_word *word = calloc(1, sizeof(*word));
  if (!word) {
    return "Out of memory";
  }
  *result = word;
  word->plain = true;
  const char *read = *cursor;
  const char *name_end = read;
  while (name_character(*name_end)) {
    ++name_end;
  }
  if (name_end != read && *name_end == '=' && name_end[1] != '>') {
    if ((size_t)(name_end - read) > ENVIRONMENT_NAME_MAX) {
      return "Environment name is too long";
    }
    word->assignment_name = copy_text(read, name_end - read);
    if (!word->assignment_name) {
      return "Out of memory";
    }
    read = name_end + 1;
  }
  char quote = 0;
  const char *error = NULL;
  while (*read && (quote || (!separator(*read) && *read != '<' && *read != '>' &&
      *read != '&' && *read != '|' && *read != ';' && *read != ')' &&
      !(*read == '=' && read[1] == '>')))) {
    char character = *read++;
    if (character == '\\' && quote != '\'') {
      word->plain = false;
      if (!*read) {
        return "Unfinished escape";
      }
      error = add_piece(word, SHELL_LITERAL, read++, 1);
    } else if (quote && character == quote) {
      quote = 0;
    } else if (!quote && (character == '\'' || character == '"')) {
      quote = character;
      word->plain = false;
    } else if (character == '$' && quote != '\'') {
      word->plain = false;
      if (*read == '(') {
        ++read;
        const char *start = read;
        struct shell_syntax nested = {0};
        error = parse_sequence(&read, true, &nested);
        bool management = false;
        if (!error) {
          error = capture_source(&nested, &management);
        }
        shell_syntax_close(&nested);
        if (!error) {
          error = add_piece(word, SHELL_SUBSTITUTION, start, read - start - 1);
          if (!error) {
            word->pieces[word->count - 1].management = management;
          }
        }
      } else if (*read == '{') {
        const char *start = ++read;
        while (name_character(*read)) {
          ++read;
        }
        if (*read != '}' || read == start ||
            (size_t)(read - start) > ENVIRONMENT_NAME_MAX) {
          return "Invalid braced environment name";
        }
        error = add_piece(word, SHELL_VARIABLE, start, read - start);
        ++read;
      } else if (letter(*read)) {
        const char *start = read++;
        while (letter(*read) || digit(*read)) {
          ++read;
        }
        if ((size_t)(read - start) > ENVIRONMENT_NAME_MAX) {
          return "Environment name is too long";
        }
        error = add_piece(word, SHELL_VARIABLE, start, read - start);
      } else if (digit(*read)) {
        return "Positional expansion is unsupported";
      } else {
        error = add_piece(word, SHELL_LITERAL, "$", 1);
      }
    } else {
      error = add_piece(word, SHELL_LITERAL, &character, 1);
    }
    if (error) {
      return error;
    }
  }
  if (quote) {
    return "Unfinished quote";
  }
  *cursor = read;
  return NULL;
}

static bool word_is(const struct shell_word *word, const char *text)
{
  if (word->assignment_name) {
    return false;
  }
  size_t offset = 0;
  for (size_t i = 0; i < word->count; ++i) {
    const struct shell_piece *piece = &word->pieces[i];
    if (piece->kind != SHELL_LITERAL) {
      return false;
    }
    size_t length = strlen(piece->text);
    if (length > strlen(text) - offset ||
        strncmp(text + offset, piece->text, length)) {
      return false;
    }
    offset += length;
  }
  return !text[offset];
}

static const char *restore_assignment(struct shell_word *word)
{
  if (!word->assignment_name) {
    return NULL;
  }
  size_t length = strlen(word->assignment_name);
  char prefix[ENVIRONMENT_NAME_MAX + 2];
  memcpy(prefix, word->assignment_name, length);
  prefix[length++] = '=';
  prefix[length] = 0;
  char *text = copy_text(prefix, length);
  if (!text) {
    return "Out of memory";
  }
  struct shell_piece *pieces = realloc(word->pieces,
      (word->count + 1) * sizeof(*pieces));
  if (!pieces) {
    free(text);
    return "Out of memory";
  }
  word->pieces = pieces;
  memmove(pieces + 1, pieces, word->count * sizeof(*pieces));
  pieces[0] = (struct shell_piece){.kind = SHELL_LITERAL, .text = text};
  ++word->count;
  free(word->assignment_name);
  word->assignment_name = NULL;
  return NULL;
}

static const char *export_name(const struct shell_word *word)
{
  if (word->assignment_name) {
    return word->assignment_name;
  }
  if (word->count == 1 && word->pieces[0].kind == SHELL_LITERAL) {
    return word->pieces[0].text;
  }
  return NULL;
}

static bool management_word(const struct shell_word *word)
{
  return word->count == 1 && word->pieces[0].management;
}

static bool control_name(const char *holder, const char *candidate)
{
  size_t length = strlen(holder);
  return strlen(candidate) == length + strlen(".control") &&
      !strncmp(holder, candidate, length) && !strcmp(candidate + length, ".control");
}

static const char *finish_command(struct shell_syntax_command *command)
{
  if (!command->stage_count ||
      !command->stages[command->stage_count - 1].count) {
    return "Pipeline stage requires a command";
  }
  struct shell_syntax_stage *first = &command->stages[0];
  command->assignment = first->words[0]->assignment_name != NULL;
  bool exporting = word_is(first->words[0], "export");
  if (command->assignment && (command->stage_count != 1 || first->count != 1 ||
      first->redirection_count || command->background)) {
    return "Assignments must be standalone commands";
  }
  if (exporting && (command->stage_count != 1 || first->redirection_count ||
      command->background)) {
    return "export must be a standalone foreground command";
  }
  if (!command->assignment && !exporting) {
    for (size_t i = 0; i < command->stage_count; ++i) {
      struct shell_syntax_stage *stage = &command->stages[i];
      if (stage->words[0]->assignment_name) {
        return "Assignments must be standalone commands";
      }
      for (size_t j = 1; j < stage->count; ++j) {
        const char *error = restore_assignment(stage->words[j]);
        if (error) {
          return error;
        }
      }
    }
  }
  for (size_t i = 0; i < command->stage_count; ++i) {
    const struct shell_syntax_stage *stage = &command->stages[i];
    size_t total = stage->count + stage->redirection_count;
    for (size_t j = 0; j < total; ++j) {
      const struct shell_word *word = j < stage->count ? stage->words[j] :
          stage->redirections[j - stage->count].word;
      for (size_t k = 0; k < word->count; ++k) {
        if (word->pieces[k].kind == SHELL_SUBSTITUTION && word->count != 1) {
          return "A captured CAP must occupy the entire word";
        }
        if (word->pieces[k].management &&
            (!word->assignment_name || word->count != 1)) {
          return "Management capture requires a named whole-word assignment";
        }
      }
    }
  }
  if (exporting) {
    for (size_t i = 1; i < first->count; ++i) {
      const struct shell_word *left = first->words[i];
      const char *left_name = export_name(left);
      if (!left_name) {
        continue;
      }
      for (size_t j = i + 1; j < first->count; ++j) {
        const struct shell_word *right = first->words[j];
        const char *right_name = export_name(right);
        if (!right_name) {
          continue;
        }
        if (((left->assignment_name || right->assignment_name) &&
             !strcmp(left_name, right_name)) ||
            (management_word(left) && control_name(left_name, right_name)) ||
            (management_word(right) && control_name(right_name, left_name))) {
          return "export destinations and management companions must not overlap";
        }
      }
    }
  }
  return NULL;
}

static const char *parse_sequence(const char **cursor, bool nested,
    struct shell_syntax *syntax)
{
  const char *read = *cursor;
  struct shell_syntax_command *command = NULL;
  struct shell_syntax_redirection *pending = NULL;
  for (;;) {
    while (separator(*read)) {
      ++read;
    }
    if (!*read || (nested && *read == ')')) {
      if (pending) {
        return "Redirection requires a filename";
      }
      if (command) {
        const char *error = finish_command(command);
        if (error) {
          return error;
        }
      }
      if (nested && !*read) {
        return "Unfinished command substitution";
      }
      *cursor = read + (nested ? 1 : 0);
      return NULL;
    }
    if (*read == ')') {
      return "Unexpected closing parenthesis";
    }
    if (*read == ';') {
      if (!command) {
        return "Semicolon requires a preceding command";
      }
      if (pending) {
        return "Redirection requires a filename";
      }
      const char *error = finish_command(command);
      if (error) {
        return error;
      }
      command = NULL;
      ++read;
      continue;
    }
    if (!command) {
      struct shell_syntax_command *commands = realloc(syntax->commands,
          (syntax->count + 1) * sizeof(*commands));
      if (!commands) {
        return "Out of memory";
      }
      syntax->commands = commands;
      command = &commands[syntax->count++];
      *command = (struct shell_syntax_command){.stage_count = 1};
    }
    struct shell_syntax_stage *stage = &command->stages[command->stage_count - 1];
    if (command->background) {
      return "& must end the line";
    }
    if (*read == '&') {
      if (read[1] == '&') {
        return "Unsupported operator";
      }
      if (pending || !stage->count || command->stage_count > 1) {
        return "& requires one completed command";
      }
      command->background = true;
      ++read;
      while (separator(*read)) {
        ++read;
      }
      if (*read && !(nested && *read == ')')) {
        return "& must end the line";
      }
      continue;
    }
    if (*read == '|') {
      if (read[1] == '|' || read[1] == '&') {
        return "Unsupported operator";
      }
      if (pending || !stage->count) {
        return "Pipeline stage requires a command";
      }
      if (command->stage_count == LAUNCH_BATCH_MAX) {
        return "Too many pipeline stages";
      }
      ++command->stage_count;
      ++read;
      continue;
    }
    if (*read == '=' && read[1] == '>') {
      return "=> is retired; use name=$(command > $api)";
    }
    const char *operator = read;
    while (digit(*operator)) {
      ++operator;
    }
    if (*operator == '<' || *operator == '>') {
      if (pending || !stage->count) {
        return "Redirection requires a preceding command and filename";
      }
      enum environment_stream_index stream = *operator == '<' ?
          ENVIRONMENT_STDIN : ENVIRONMENT_STDOUT;
      if (operator != read) {
        if (operator != read + 1 || *read != '2' || *operator != '>') {
          return "Unsupported descriptor redirection";
        }
        stream = ENVIRONMENT_STDERR;
      }
      bool append = *operator == '>' && operator[1] == '>';
      read = operator + (append ? 2 : 1);
      if (*read == '<' || *read == '>' || *read == '&' || *read == '|') {
        return "Unsupported operator";
      }
      for (size_t i = 0; i < stage->redirection_count; ++i) {
        if (stage->redirections[i].stream == stream) {
          return "Duplicate redirection";
        }
      }
      pending = &stage->redirections[stage->redirection_count++];
      *pending = (struct shell_syntax_redirection){.stream = stream, .append = append};
      continue;
    }
    struct shell_word *word = NULL;
    const char *error = parse_word(&read, &word);
    if (error) {
      close_word(word);
      return error;
    }
    if (pending) {
      if (word->plain && word->count && word->pieces[0].kind == SHELL_LITERAL &&
          word->pieces[0].text[0] == '@') {
        close_word(word);
        return "@name redirects are retired; use $name";
      }
      error = restore_assignment(word);
      if (error) {
        close_word(word);
        return error;
      }
      pending->word = word;
      pending = NULL;
    } else {
      struct shell_word **words = realloc(stage->words,
          (stage->count + 1) * sizeof(*words));
      if (!words) {
        close_word(word);
        return "Out of memory";
      }
      stage->words = words;
      stage->words[stage->count++] = word;
    }
  }
}

const char *shell_syntax_parse(const char *line, struct shell_syntax *syntax)
{
  *syntax = (struct shell_syntax){0};
  const char *cursor = line;
  const char *error = parse_sequence(&cursor, false, syntax);
  if (error) {
    shell_syntax_close(syntax);
  }
  return error;
}
