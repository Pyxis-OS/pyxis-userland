#ifndef USERSPACE_TERM_H
#define USERSPACE_TERM_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <stdbool.h>
#include <stddef.h>

/* Borrowed handles. No allocation, global terminal, buffering or implicit close.
 * Input and output may be separate grants to the same console. */
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

/* Logical input keys, independent of physical keyboard events. Bytes retain
 * their byte values, including Escape (27), Tab and control characters. */
enum term_key {
  TERM_KEY_UNKNOWN = 256,
  TERM_KEY_LEFT, TERM_KEY_RIGHT, TERM_KEY_HOME, TERM_KEY_END, TERM_KEY_DELETE,
  TERM_KEY_UP, TERM_KEY_DOWN, TERM_KEY_PAGE_UP, TERM_KEY_PAGE_DOWN,
};

/* Block for a key, then allow 100 ms between bytes of an escape sequence.
 * Standalone Escape returns 27; incomplete/unsupported sequences return UNKNOWN.
 * Reads one byte at a time with no retained input or read-ahead. INPUT_LOST and
 * other native failures are returned to the caller; *key is UNKNOWN on error. */
enum call_status term_read_key(struct terminal *term, unsigned *key);

enum term_line_status {
  TERM_LINE_OK,
  TERM_LINE_CANCELLED,
  TERM_LINE_INPUT_LOST,
  TERM_LINE_ERROR,
};

struct term_line_result {
  enum term_line_status status;
  enum call_status error; /* Native failure for ERROR/INPUT_LOST, otherwise OK. */
  size_t length;          /* Excludes newline and NUL; zero unless LINE_OK. */
  bool limit_reached;     /* An insertion was rejected; editing still continued. */
};

/* Own input/output exclusively for the call. Prompt must be printable ASCII;
 * the buffer must be separate writable storage, with room for at least NUL.
 * Starts on a fresh line, advancing only if not already at column zero, then
 * clears from there to screen end.
 * Uses default colors and the visible terminal cursor; both are restored on
 * normal completion. Cursor visibility is enabled on return.
 *
 * Printable ASCII only, one byte per cell. Insert, Backspace/Delete, Left/Right,
 * Home/End, Enter and Ctrl+C are supported. Up/Down/Page keys are decoded but
 * ignored; no history, tabs, Unicode editing or EOF interpretation. Standalone
 * Escape is decoded with a timeout and ignored by this line editor.
 *
 * The prompt, line and one cursor cell must fit in the visible terminal.
 * Redraws account for delayed wrapping at the right margin.
 * Buffer/display exhaustion rejects insertion, colors its cell red and sets
 * limit_reached. Deletion, movement, submission and cancellation remain usable.
 * CANCELLED/INPUT_LOST/ERROR discard the line and clear buffer[0] when possible.
 * Reads one byte at a time: no unread bytes are retained for a future caller. */
struct term_line_result term_read_line(struct terminal *term, const char *prompt,
                                      char *buffer, size_t capacity);

#endif
