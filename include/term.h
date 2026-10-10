#ifndef USERSPACE_TERM_H
#define USERSPACE_TERM_H

#include <abi/handle.h>
#include <abi/console.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>

/* Borrowed handles. No allocation, global terminal, buffering or implicit close.
 * Input/output implement CONSOLE for a framebuffer or independent terminal. */
struct terminal {
  handle_t input;
  handle_t output;
};

enum call_status term_read(struct terminal *term, void *bytes, size_t capacity, size_t *read);
enum call_status term_read_timeout(struct terminal *term, void *bytes, size_t capacity,
                                  uint32_t timeout_ms, size_t *read);
enum call_status term_write(struct terminal *term, const void *bytes, size_t size, size_t *written);
enum call_status term_write_all(struct terminal *term, const void *bytes, size_t size);
enum call_status term_print(struct terminal *term, const char *text);
/* Begin a fresh line without adding a blank row when already at column zero. */
enum call_status term_fresh_line(struct terminal *term);
enum call_status term_size(struct terminal *term, size_t *columns, size_t *rows);
enum call_status term_geometry(struct terminal *term, struct console_size_reply *size);
/* Set shared TTY tab spacing through output: 1..32 columns, default 8.
 * Affects future tabs only; preserves existing text, cursor and parser state. */
enum call_status term_set_tab_width(struct terminal *term, size_t columns);
/* READ authority on input. On success *passthrough is a new handle; while it
 * stays open, Ctrl+C reaches this application as data instead of interrupting
 * the foreground job. Close it with handle_close to withdraw. */
enum call_status term_passthrough(struct terminal *term, handle_t *passthrough);

/* Movement clamps at screen edges and never scrolls. Position is zero-based;
 * relative movement accepts -65535..65535. Clear-screen also moves to (0, 0),
 * while clear-line leaves the cursor in place. */
enum call_status term_move(struct terminal *term, int rows, int columns);
enum call_status term_position(struct terminal *term, size_t row, size_t column);
enum call_status term_clear(struct terminal *term);
enum call_status term_clear_line(struct terminal *term);
/* Palette indices 0..15, or -1 for the terminal's separate default colors. */
enum call_status term_colors(struct terminal *term, int foreground, int background);
enum call_status term_reverse(struct terminal *term, bool enabled);
enum call_status term_reset_style(struct terminal *term);
enum call_status term_cursor_visible(struct terminal *term, bool visible);
/* Enter the cleared alternate screen, saving the cursor, or leave it, restoring
 * the screen and cursor that entering saved. */
enum call_status term_alternate_screen(struct terminal *term, bool enabled);

/* Logical input keys, independent of physical keyboard events. Bytes retain
 * their byte values, including Escape (27), Tab and control characters. */
enum term_key {
  TERM_KEY_UNKNOWN = 256,
  TERM_KEY_LEFT, TERM_KEY_RIGHT, TERM_KEY_HOME, TERM_KEY_END, TERM_KEY_DELETE,
  TERM_KEY_UP, TERM_KEY_DOWN, TERM_KEY_PAGE_UP, TERM_KEY_PAGE_DOWN,
  TERM_KEY_EOF,
};

/* Block for a key, then allow 100 ms between bytes of an escape sequence.
 * Standalone Escape returns 27; incomplete/unsupported sequences return UNKNOWN.
 * Input EOF returns EOF, also when it interrupts an incomplete escape sequence;
 * the Ctrl+D byte remains 4.
 * Reads one byte at a time with no retained input or read-ahead. INPUT_LOST and
 * other native failures are returned to the caller; *key is UNKNOWN on error. */
enum call_status term_read_key(struct terminal *term, unsigned *key);
/* Bound the initial byte wait; zero polls. No byte returns TIMED_OUT with key
 * UNKNOWN. Once input begins, use the same escape-sequence rules as read_key,
 * even if decoding extends past the initial timeout. No partial key is lost
 * to the caller's timeout. This is not a total key-decoding deadline. */
enum call_status term_read_key_timeout(struct terminal *term, uint32_t timeout_ms,
                                      unsigned *key);

enum term_event_kind {
  TERM_EVENT_KEY, TERM_EVENT_RESIZED,
  TERM_EVENT_PASTE_BEGIN, TERM_EVENT_PASTE_DATA, TERM_EVENT_PASTE_END,
  TERM_EVENT_PASTE_CANCEL,
};

struct term_event {
  enum term_event_kind kind;
  unsigned key;
  struct console_size_reply size; /* Valid only for RESIZED. */
  /* Paste fields are produced only for an explicitly registered reader. */
  uint64_t transaction_id;
  enum call_status paste_status;
  size_t length;
  unsigned char bytes[256]; /* Owned DATA, never ordinary input read-ahead. */
};

/* One reader exclusively owns key decoding for its borrowed terminal. Resize
 * events retain partial Escape/CSI state and the original 100 ms byte deadline.
 * Clock READ authority is explicit; no handles are acquired or closed here. */
struct term_event_reader {
  struct terminal *term;
  handle_t clock;
  struct console_size_reply size;
  uint64_t byte_deadline;
  unsigned state, parameter;
  uint64_t receiver_epoch; /* Only stock editing opts in; zero keeps raw reads. */
};

enum call_status term_event_reader_init(struct term_event_reader *reader,
    struct terminal *term, handle_t clock);
enum call_status term_read_event(struct term_event_reader *reader, struct term_event *event);
/* Bounds only the initial-byte wait, just like term_read_key_timeout. */
enum call_status term_read_event_timeout(struct term_event_reader *reader,
    uint32_t timeout_ms, struct term_event *event);

enum term_line_status {
  TERM_LINE_OK,
  TERM_LINE_CANCELLED,
  TERM_LINE_EOF,
  TERM_LINE_INPUT_LOST,
  TERM_LINE_ERROR,
};

struct term_line_result {
  enum term_line_status status;
  enum call_status error; /* Native failure for ERROR/INPUT_LOST, otherwise OK. */
  size_t length;          /* Excludes newline and NUL; zero unless LINE_OK. */
  bool limit_reached;     /* An insertion was rejected; editing still continued. */
  bool recorded;          /* LINE_OK appended the line to the caller's history. */
  bool paste_cancelled;   /* An admitted paste was cancelled; editing continued. */
  size_t pasted;          /* Paste bytes inserted, including LF/Tab as spaces. */
  enum call_status paste_status; /* Most recent admitted paste completion. */
};

/* Own input/output exclusively for the call. Prompt must be printable ASCII;
 * the buffer must be separate writable storage, with room for at least NUL.
 * Starts on a fresh line, advancing only if not already at column zero, then
 * clears from there to screen end.
 * Uses default colors and the visible terminal cursor; both are restored on
 * normal completion. Cursor visibility is enabled on return.
 *
 * Printable ASCII only, one byte per cell. Insert, Backspace/Delete, Left/Right,
 * Home/End, Enter and Ctrl+C are supported. Ctrl+D returns EOF only on an empty
 * line; otherwise it is ignored. Actual input EOF always discards a partial line
 * and returns EOF. Up/Down/Page keys are decoded but ignored without a history;
 * no Unicode editing; Tab is ignored unless term_read_line_completing is given a
 * completion. Standalone Escape is decoded with a timeout and
 * ignored by this line editor.
 *
 * The buffer's capacity minus NUL is the editing limit. A line larger than the
 * screen uses a visible window around its cursor; text and prompt are retained.
 * With a startup clock READ grant, resize wakes and redraws the editor. Without
 * it, existing key reads remain usable with the initially queried dimensions.
 * Redraws account for delayed wrapping at the right margin.
 * Buffer exhaustion rejects insertion, colors its cell red and sets
 * limit_reached. Deletion, movement, submission and cancellation remain usable.
 * CANCELLED/EOF/INPUT_LOST/ERROR clear buffer[0] when possible.
 * Reads one byte at a time: no unread bytes are retained for a future caller.
 * Exclusively registers a process-owned native paste receiver while editing,
 * when matching input/output authority is available. Busy/denied registration
 * preserves ordinary editing. Paste inserts ASCII with LF/Tab changed to spaces
 * and consumes complete native framing even at the line limit; it never submits.
 * Physical Enter freshness and suppression belong to the kernel. Cancellation
 * retains the editable prefix, reports its inserted length and sets the result's
 * paste fields. Quiet readers report through the result only. The receiver epoch
 * is released before every return, so a child cannot inherit a transaction.
 * Holds passthrough for the call, so Ctrl+C cancels the line rather than
 * interrupting the program; if the request fails, editing continues without
 * it. Failing to withdraw passthrough returns ERROR with CALL_BAD_HANDLE. */
struct term_line_result term_read_line(struct terminal *term, const char *prompt,
                                      char *buffer, size_t capacity);
/* Starts with editable printable ASCII and the cursor at its end. Initial
 * text is borrowed, NUL-terminated and disjoint from buffer. It must fit the
 * same buffer limit; cancellation and errors discard it. */
struct term_line_result term_read_line_initial(struct terminal *term, const char *prompt,
    const char *initial, char *buffer, size_t capacity);
/* Same editing, cancellation, EOF and buffer limit as term_read_line, but
 * writes nothing: no prompt, fresh line, redraw, cursor or
 * style control, overflow color, submission newline or ^C. The prompt is
 * validated and measured only. Output state is left as the caller had it. */
struct term_line_result term_read_line_quiet(struct terminal *term, const char *prompt,
                                            char *buffer, size_t capacity);

#define TERM_HISTORY_ENTRIES 100

/* Submitted lines kept by the caller across line reads, oldest first. Start
 * zero-initialized; entries are heap copies released by term_history_free. */
struct term_history {
  char *entries[TERM_HISTORY_ENTRIES];
  size_t count;
};

/* Releases every entry and leaves the history empty and reusable. */
void term_history_free(struct term_history *history);

/* Appends a heap copy of line unless it is empty or all spaces, or the same as
 * the newest entry; a full history drops its oldest entry. Returns whether the
 * line was appended, false also when memory for the copy is unavailable. */
bool term_history_add(struct term_history *history, const char *line);

/* term_read_line with recall. Up loads the previous entry into the editor and
 * Down the next; past the newest entry Down restores the line typed before
 * recall began. Edits to a recalled entry last until another entry is loaded.
 * A recalled entry longer than the buffer limit is shortened and treated as a
 * rejected insertion: its cell turns red and limit_reached is set.
 *
 * LINE_OK appends the submitted line with term_history_add and sets recorded
 * when it did. If memory for the unfinished line is unavailable, recall is
 * ignored; the read itself is unaffected. NULL history behaves as
 * term_read_line. */
struct term_line_result term_read_line_history(struct terminal *term, const char *prompt,
    struct term_history *history, char *buffer, size_t capacity);

/* Opt-in shell integration: emit OSC 133;B after the initial empty prompt is
 * drawn and before reading input. Otherwise identical to
 * term_read_line_history. */
struct term_line_result term_read_line_marked(struct terminal *term, const char *prompt,
    struct term_history *history, char *buffer, size_t capacity);


/* Tab completion for one word of the line. The editor calls candidates() with
 * the text left of the cursor (line[0..cursor), NUL-terminated at cursor) and
 * the caller finds the word to complete. On success it fills names with heap
 * strings, ascending, unique and printable ASCII without spaces, each a full
 * replacement for line[start..cursor); the editor frees the strings and the
 * array. Returning false, or no names, leaves the line unchanged.
 *
 * One name replaces the word and adds a space unless one follows. Several
 * names extend the word to their common prefix when that is longer. Otherwise
 * they are listed below the line in columns and the prompt and line are drawn
 * again. A replacement that does not fit the buffer changes nothing and sets
 * limit_reached. Tab is ignored when no completion is given, as before. */
struct term_candidates {
  char **names;
  size_t count;
  size_t start;
};

struct term_completion {
  bool (*candidates)(void *context, const char *line, size_t cursor,
      struct term_candidates *result);
  void *context;
};

/* term_read_line_history or term_read_line_marked, with Tab completion.
 * History and completion may each be NULL. */
struct term_line_result term_read_line_completing(struct terminal *term, const char *prompt,
    struct term_history *history, const struct term_completion *completion, bool marked,
    char *buffer, size_t capacity);

#endif
