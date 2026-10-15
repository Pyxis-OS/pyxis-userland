#ifndef USERSPACE_SHELL_SYNTAX_H
#define USERSPACE_SHELL_SYNTAX_H

#include "shell.h"
#include <environment.h>

enum shell_piece_kind { SHELL_LITERAL, SHELL_VARIABLE, SHELL_SUBSTITUTION };

struct shell_piece {
  enum shell_piece_kind kind;
  char *text;
};

struct shell_word {
  struct shell_piece *pieces;
  size_t count;
  bool plain;
  char *assignment_name;
};

struct shell_syntax_redirection {
  enum environment_stream_index stream;
  bool append;
  struct shell_word *word;
};

struct shell_syntax_stage {
  struct shell_word **words;
  size_t count;
  struct shell_syntax_redirection redirections[ENVIRONMENT_STREAM_COUNT];
  size_t redirection_count;
};

struct shell_syntax_command {
  struct shell_syntax_stage stages[LAUNCH_BATCH_MAX];
  size_t stage_count;
  bool background;
  char *capture;
  bool assignment;
};

struct shell_syntax {
  struct shell_syntax_command *commands;
  size_t count;
};

/* Each argv metadata record borrows its text/CAP from this expansion. A named
 * record is an assignment operand with the name= prefix omitted from value.
 * CAP argument strings use the selected name:// spelling. */
struct shell_expanded_word {
  const char *assignment_name;
  const struct environment_value *value;
};

struct shell_expansion {
  struct shell_command_line command;
  /* Borrowed records with separately allocated arrays; close only through
   * shell_expansion_close, which owns their per-word CAP references. */
  struct environment_capture selected[LAUNCH_BATCH_MAX];
  struct shell_expanded_word *words;
  size_t word_count;
  /* words and values are parallel in stage/argv order, excluding redirections. */
  struct environment_value *values;
  char **arguments;
  size_t argument_count;
  size_t value_count;
};

/* Parse the whole submitted line before expansion or command side effects.
 * Substitutions are recursively validated and then refused until the capture
 * prerequisites exist. On error, output is empty and the diagnostic is static. */
const char *shell_syntax_parse(const char *line, struct shell_syntax *syntax);
void shell_syntax_close(struct shell_syntax *syntax);
/* Lookup failure stores its CALL status in status; other diagnostics leave
 * CALL_OK. Expansion owns every argv string and selected CAP until close. */
const char *shell_syntax_expand(struct shell *shell,
    const struct shell_syntax_command *syntax, struct shell_expansion *expansion,
    enum call_status *status);
void shell_expansion_close(struct shell_expansion *expansion);

#endif
