#include <handle.h>
#include <term.h>
#include <string.h>

struct line_editor {
  struct terminal *term;
  const char *prompt;
  char *buffer;
  size_t columns, prompt_length;
  size_t length, cursor, displayed_length, displayed_cursor;
  bool quiet;
};

static enum call_status move_between(struct line_editor *editor, size_t from, size_t to)
{
  int rows = (int)(to / editor->columns) - (int)(from / editor->columns);
  enum call_status status = term_print(editor->term, "\r");
  if (status == CALL_OK) {
    status = term_move(editor->term, rows, 0);
  }
  return status == CALL_OK ? term_move(editor->term, 0, to % editor->columns) : status;
}

static enum call_status write_spaces(struct terminal *term, size_t count)
{
  static const char spaces[] = "                                ";
  while (count) {
    size_t chunk = count < sizeof(spaces) - 1 ? count : sizeof(spaces) - 1;
    enum call_status status = term_write_all(term, spaces, chunk);
    if (status != CALL_OK) {
      return status;
    }
    count -= chunk;
  }
  return CALL_OK;
}

static enum call_status draw_line(struct line_editor *editor, bool editing, bool full)
{
  if (editor->quiet) {
    return CALL_OK;
  }
  struct terminal *term = editor->term;
  enum call_status status = term_cursor_visible(term, false);
  if (status != CALL_OK) {
    return status;
  }
  status = move_between(editor, editor->displayed_cursor, 0);
  if (status != CALL_OK) {
    return status;
  }
  status = term_print(term, editor->prompt);
  if (status != CALL_OK) {
    return status;
  }

  status = term_write_all(term, editor->buffer, editor->length);
  if (status != CALL_OK) {
    return status;
  }

  /* Clear the old tail, including the previous end cell. Keeping
   * this span on screen lets row-relative movement survive TTY scrolling. */
  size_t span = editor->length > editor->displayed_length ? editor->length : editor->displayed_length;
  size_t trailing = span - editor->length + 1;
  status = write_spaces(term, trailing);
  if (status != CALL_OK) {
    return status;
  }
  /* The last byte leaves the cursor at the margin until another is printed.
   * CR below cancels that pending wrap before relative movement. */
  size_t end = editor->prompt_length + span + 1;
  if (end % editor->columns == 0) {
    --end;
  }
  size_t target = editor->prompt_length + (editing ? editor->cursor : editor->length);
  status = move_between(editor, end, target);
  if (status == CALL_OK && full) {
    /* Keep the existing overflow cue at the editing position. */
    status = term_colors(term, -1, 9);
    if (status == CALL_OK) {
      char c = editor->cursor < editor->length ? editor->buffer[editor->cursor] : ' ';
      status = term_write_all(term, &c, 1);
    }
    if (status == CALL_OK) {
      status = term_reset_style(term);
    }
    if (status == CALL_OK) {
      size_t after = target % editor->columns == editor->columns - 1 ? target : target + 1;
      status = move_between(editor, after, target);
    }
  }
  if (status == CALL_OK) {
    status = term_cursor_visible(term, true);
  }
  if (status == CALL_OK) {
    editor->displayed_length = editor->length;
    editor->displayed_cursor = target;
  }
  return status;
}

static struct term_line_result read_line(struct terminal *term, const char *prompt,
    const char *initial, char *buffer, size_t capacity, bool quiet)
{
  struct term_line_result result = {.status = TERM_LINE_ERROR, .error = CALL_BAD_REQUEST};
  if (!buffer || !capacity) {
    return result;
  }
  buffer[0] = '\0';
  if (!term || !prompt || !initial) {
    return result;
  }

  size_t columns, rows;
  result.error = term_size(term, &columns, &rows);
  if (result.error != CALL_OK) {
    return result;
  }
  if (!columns || !rows || columns > UINT16_MAX || rows > UINT16_MAX) {
    result.error = CALL_BAD_REQUEST;
    return result;
  }
  size_t prompt_length = strlen(prompt);
  for (size_t i = 0; i < prompt_length; ++i) {
    if ((unsigned char)prompt[i] < ' ' || (unsigned char)prompt[i] > '~') {
      result.error = CALL_BAD_REQUEST;
      return result;
    }
  }

  size_t cells = columns * rows;
  if (cells < 1 || prompt_length > cells - 1) {
    result.error = CALL_LIMIT;
    return result;
  }
  size_t limit = cells - prompt_length - 1;
  if (limit > capacity - 1) {
    limit = capacity - 1;
  }
  size_t initial_length = strlen(initial);
  if (initial_length > limit) {
    result.error = CALL_LIMIT;
    return result;
  }
  for (size_t i = 0; i < initial_length; ++i) {
    if ((unsigned char)initial[i] < ' ' || (unsigned char)initial[i] > '~') {
      result.error = CALL_BAD_REQUEST;
      return result;
    }
  }
  memcpy(buffer, initial, initial_length + 1);
  struct line_editor editor = {
    .term = term, .prompt = prompt, .buffer = buffer,
    .columns = columns, .prompt_length = prompt_length, .quiet = quiet,
    .length = initial_length, .cursor = initial_length,
  };
  if (!quiet) {
    result.error = term_fresh_line(term);
    if (result.error != CALL_OK) {
      return result;
    }
    result.error = term_print(term, "\x1b[0m\r\x1b[J");
    if (result.error != CALL_OK) {
      return result;
    }
  }
  result.error = draw_line(&editor, true, false);

  while (result.error == CALL_OK) {
    unsigned key;
    result.error = term_read_key(term, &key);
    if (result.error == CALL_INPUT_LOST) {
      result.status = TERM_LINE_INPUT_LOST;
      break;
    }
    if (result.error != CALL_OK) {
      break;
    }
    if (key == '\n' || key == '\r' || key == 3) {
      result.status = key == 3 ? TERM_LINE_CANCELLED : TERM_LINE_OK;
      break;
    }
    if (key == TERM_KEY_EOF || (key == 4 && editor.length == 0)) {
      result.status = TERM_LINE_EOF;
      break;
    }

    bool full = false;
    switch (key) {
    case TERM_KEY_LEFT:
      if (editor.cursor) {
        --editor.cursor;
      }
      break;
    case TERM_KEY_RIGHT:
      if (editor.cursor < editor.length) {
        ++editor.cursor;
      }
      break;
    case TERM_KEY_HOME:
      editor.cursor = 0;
      break;
    case TERM_KEY_END:
      editor.cursor = editor.length;
      break;
    case '\b':
    case 0x7f:
      if (!editor.cursor) {
        continue;
      }
      --editor.cursor;
      [[fallthrough]];
    case TERM_KEY_DELETE:
      if (editor.cursor == editor.length) {
        continue;
      }
      memmove(buffer + editor.cursor, buffer + editor.cursor + 1, editor.length - editor.cursor);
      --editor.length;
      break;
    default:
      if (key < ' ' || key > '~') {
        continue;
      }
      if (editor.length == limit) {
        full = true;
        result.limit_reached = true;
      } else {
        memmove(buffer + editor.cursor + 1, buffer + editor.cursor, editor.length - editor.cursor + 1);
        buffer[editor.cursor++] = key;
        ++editor.length;
      }
      break;
    }
    result.error = draw_line(&editor, true, full);
  }

  if (result.status != TERM_LINE_ERROR && !quiet) {
    enum call_status status = draw_line(&editor, false, false);
    if (status == CALL_OK) {
      status = term_print(term, result.status == TERM_LINE_CANCELLED ? "^C\n" : "\n");
    }
    if (status != CALL_OK) {
      result.status = TERM_LINE_ERROR;
      result.error = status;
    }
  }
  if (result.status == TERM_LINE_ERROR && !quiet) {
    /* An output failure can leave a partial redraw; still attempt restoration. */
    term_reset_style(term);
    term_cursor_visible(term, true);
  }
  if (result.status == TERM_LINE_OK) {
    result.length = editor.length;
  } else {
    buffer[0] = '\0';
  }
  return result;
}

/* Ctrl+C belongs to the editor only while a line is being read; code the
 * application runs between lines stays interruptible. */
static struct term_line_result read_line_passthrough(struct terminal *term,
    const char *prompt, const char *initial, char *buffer, size_t capacity, bool quiet)
{
  /* Invalid arguments keep read_line's handled BAD_REQUEST result. */
  if (!term || !prompt || !buffer || !capacity) {
    return read_line(term, prompt, initial, buffer, capacity, quiet);
  }
  handle_t passthrough;
  bool held = term_passthrough(term, &passthrough) == CALL_OK;
  struct term_line_result result = read_line(term, prompt, initial, buffer, capacity, quiet);
  if (held && handle_close(passthrough) != 0) {
    /* A retained grant would keep Ctrl+C from interrupting this program. */
    result = (struct term_line_result){.status = TERM_LINE_ERROR, .error = CALL_BAD_HANDLE};
    if (capacity) {
      buffer[0] = '\0';
    }
  }
  return result;
}

struct term_line_result term_read_line(struct terminal *term, const char *prompt,
                                      char *buffer, size_t capacity)
{
  return read_line_passthrough(term, prompt, "", buffer, capacity, false);
}

struct term_line_result term_read_line_quiet(struct terminal *term, const char *prompt,
                                            char *buffer, size_t capacity)
{
  return read_line_passthrough(term, prompt, "", buffer, capacity, true);
}

struct term_line_result term_read_line_initial(struct terminal *term, const char *prompt,
    const char *initial, char *buffer, size_t capacity)
{
  return read_line_passthrough(term, prompt, initial, buffer, capacity, false);
}
