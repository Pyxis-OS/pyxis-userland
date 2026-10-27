#ifndef USERSPACE_SHELL_SYNTAX_H
#define USERSPACE_SHELL_SYNTAX_H

#include "shell.h"
#include <environment.h>
#include "capture.h"

/* Script arguments: $N or ${N} is one argument as TEXT, $# their count, and
 * "$@", only as an entire quoted word, every argument as a separate word. */
enum shell_piece_kind {
  SHELL_LITERAL, SHELL_VARIABLE, SHELL_SUBSTITUTION,
  SHELL_POSITIONAL, SHELL_ARGUMENT_COUNT, SHELL_ALL_ARGUMENTS,
};

struct shell_piece {
  enum shell_piece_kind kind;
  char *text;
  bool management;
  /* Literal text typed outside quotes and escapes: the only text in which `*`
   * is a wildcard. */
  bool unquoted;
};

struct shell_word {
  struct shell_piece *pieces;
  size_t count;
  bool plain;
  /* An unquoted `*` appears: the expanded word may be a pattern. */
  bool glob;
  char *assignment_name;
};

struct shell_syntax_redirection {
  enum environment_stream_index stream;
  bool append, duplicate_stdout;
  struct shell_word *word;
};

struct shell_syntax_stage {
  struct shell_word **words;
  size_t count;
  struct shell_syntax_redirection redirections[ENVIRONMENT_STREAM_COUNT];
  size_t redirection_count;
};

enum shell_condition { SHELL_ALWAYS, SHELL_ON_SUCCESS, SHELL_ON_FAILURE };

struct shell_syntax_command {
  /* ALWAYS starts a new list; other connectors bind to its accumulated result. */
  enum shell_condition condition;
  struct shell_syntax_stage stages[LAUNCH_BATCH_MAX];
  size_t stage_count;
  bool background;
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
  struct shell_capture_result *capture;
};

/* Wildcard expansion (glob.c). A pattern word becomes one word per match, so
 * the expansion works on a copy of the command whose pattern words are replaced
 * by literal words owned here. NULL command means no word was a pattern. */
struct shell_glob {
  struct shell_syntax_command *command;
  struct shell_word ***arrays;
  size_t array_count;
  struct shell_word **words;
  size_t word_count, word_capacity;
};
const char *shell_glob_prepare(struct shell *shell,
    const struct shell_syntax_command *syntax, struct shell_glob *glob,
    enum call_status *status);
void shell_glob_close(struct shell_glob *glob);
enum call_status shell_lookup_value(handle_t snapshot, const char *name,
    struct environment_value *value);
const char *shell_positional_argument(const struct shell *shell, const char *digits,
    const char **argument);

struct shell_expansion {
  struct shell_command_line command;
  /* A failed inline capture's stop reason survives expansion cleanup. */
  struct shell_outcome failure_outcome;
  /* Borrowed records with separately allocated arrays; close only through
   * shell_expansion_close, which owns their per-word CAP references. */
  struct environment_capture selected[LAUNCH_BATCH_MAX];
  struct shell_expanded_word *words;
  size_t word_count;
  /* words and values are parallel in stage/argv order, excluding redirections. */
  struct environment_value *values;
  struct shell_capture_result *captures;
  char **arguments;
  size_t argument_count;
  size_t value_count;
};

/* Parse the whole submitted line before expansion or command side effects.
 * Substitutions are recursively validated; paired management producers require
 * a named whole-word destination. Errors leave output empty; diagnostics are static. */
const char *shell_syntax_parse(const struct shell *shell, const char *line,
    struct shell_syntax *syntax);
void shell_syntax_close(struct shell_syntax *syntax);
bool shell_condition_selected(enum shell_condition condition, enum command_result result);
/* Environment calls report their CALL status; cleanup failure reports
 * CALL_OUTCOME_UNKNOWN. Syntax/type diagnostics leave CALL_OK. Expansion owns
 * every argv string and selected CAP until close. */
const char *shell_syntax_expand(struct shell *shell,
    const struct shell_syntax_command *syntax, struct shell_expansion *expansion,
    enum call_status *status);
/* Release each owned reference once and always free all storage. False reports
 * at least one failed CAP close; callers must stop input and must not retry. */
bool shell_expansion_close(struct shell_expansion *expansion);

#endif
