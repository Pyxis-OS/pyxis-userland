#include <term.h>
#include <string.h>

/* Values above the byte range are local editor keys, not a physical-key API. */
enum edit_key {
  EDIT_LEFT = 256, EDIT_RIGHT, EDIT_HOME, EDIT_END, EDIT_DELETE,
  EDIT_UP, EDIT_DOWN, EDIT_PAGE_UP, EDIT_PAGE_DOWN,
};

struct line_editor {
  struct terminal *term;
  const char *prompt;
  char *buffer;
  size_t columns, prompt_length;
  size_t length, cursor, displayed_length, displayed_cursor;
};

static enum call_status read_key(struct terminal *term, unsigned *key)
{
  enum { TEXT, ESCAPE, CSI, IGNORE_CSI } state = TEXT;
  unsigned parameter = 0;

  for (;;) {
    unsigned char byte;
    size_t count;
    enum call_status status = term_read(term, &byte, 1, &count);
    if (status != CALL_OK) {
      return status;
    }
    if (byte == '\x1b') {
      state = ESCAPE;
      parameter = 0;
      continue;
    }
    if (byte == 3 || byte == '\n' || byte == '\r' || byte == '\b' || byte == 0x7f) {
      *key = byte;
      return CALL_OK;
    }
    if (state == ESCAPE) {
      if (byte == '[') {
        state = CSI;
        continue;
      }
      /* Escape has no standalone action; an unrelated following byte still
       * belongs to this line. No timer or read-ahead buffer is needed. */
      state = TEXT;
    }
    if (state == CSI || state == IGNORE_CSI) {
      if (byte >= 0x40 && byte <= 0x7e) {
        bool valid = state == CSI;
        state = TEXT;
        if (valid && (parameter == 0 || parameter == 1)) {
          switch (byte) {
          case 'A': *key = EDIT_UP; return CALL_OK;
          case 'B': *key = EDIT_DOWN; return CALL_OK;
          case 'C': *key = EDIT_RIGHT; return CALL_OK;
          case 'D': *key = EDIT_LEFT; return CALL_OK;
          case 'H': *key = EDIT_HOME; return CALL_OK;
          case 'F': *key = EDIT_END; return CALL_OK;
          }
        }
        if (valid && byte == '~') {
          switch (parameter) {
          case 3: *key = EDIT_DELETE; return CALL_OK;
          case 5: *key = EDIT_PAGE_UP; return CALL_OK;
          case 6: *key = EDIT_PAGE_DOWN; return CALL_OK;
          }
        }
      } else if (state == CSI) {
        if (byte >= '0' && byte <= '9' && parameter < 100) {
          parameter = parameter * 10 + byte - '0';
        } else {
          state = IGNORE_CSI;
        }
      }
      continue;
    }
    if (byte >= ' ' && byte <= '~') {
      *key = byte;
      return CALL_OK;
    }
  }
}

static enum call_status move_between(struct line_editor *editor, size_t from, size_t to)
{
  int rows = (int)(to / editor->columns) - (int)(from / editor->columns);
  enum call_status status = term_move(editor->term, rows, 0);
  if (status == CALL_OK) {
    status = term_print(editor->term, "\r");
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

static enum call_status draw_line(struct line_editor *editor, bool highlight, bool full)
{
  struct terminal *term = editor->term;
  enum call_status status = move_between(editor, editor->displayed_cursor, 0);
  if (status != CALL_OK) {
    return status;
  }
  status = term_print(term, editor->prompt);
  if (status != CALL_OK) {
    return status;
  }

  size_t prefix = highlight ? editor->cursor : editor->length;
  status = term_write_all(term, editor->buffer, prefix);
  if (status != CALL_OK) {
    return status;
  }
  if (highlight) {
    if (full) {
      status = term_colors(term, 9, -1);
      if (status != CALL_OK) {
        return status;
      }
    }
    status = term_reverse(term, true);
    if (status != CALL_OK) {
      return status;
    }
    char cursor = prefix < editor->length ? editor->buffer[prefix] : ' ';
    status = term_write_all(term, &cursor, 1);
    if (status != CALL_OK) {
      return status;
    }
    status = term_reset_style(term);
    if (status != CALL_OK) {
      return status;
    }
    if (prefix < editor->length) {
      status = term_write_all(term, editor->buffer + prefix + 1, editor->length - prefix - 1);
      if (status != CALL_OK) {
        return status;
      }
    }
  }

  /* Clear the old tail, including the previous highlighted end cell. Keeping
   * this span on screen lets row-relative movement survive TTY scrolling. */
  size_t span = editor->length > editor->displayed_length ? editor->length : editor->displayed_length;
  size_t trailing = span - editor->length + 1;
  if (highlight && prefix == editor->length) {
    --trailing; /* The highlighted blank already occupies the end cell. */
  }
  status = write_spaces(term, trailing);
  if (status != CALL_OK) {
    return status;
  }
  size_t end = editor->prompt_length + span + 1;
  size_t target = editor->prompt_length + (highlight ? editor->cursor : editor->length);
  status = move_between(editor, end, target);
  if (status == CALL_OK) {
    editor->displayed_length = editor->length;
    editor->displayed_cursor = target;
  }
  return status;
}

struct term_line_result term_read_line(struct terminal *term, const char *prompt,
                                      char *buffer, size_t capacity)
{
  struct term_line_result result = {.status = TERM_LINE_ERROR, .error = CALL_BAD_REQUEST};
  if (!buffer || !capacity) {
    return result;
  }
  buffer[0] = '\0';
  if (!term || !prompt) {
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
  if (cells < 2 || prompt_length > cells - 2) {
    result.error = CALL_LIMIT;
    return result;
  }
  size_t limit = cells - prompt_length - 2;
  if (limit > capacity - 1) {
    limit = capacity - 1;
  }
  struct line_editor editor = {
    .term = term, .prompt = prompt, .buffer = buffer,
    .columns = columns, .prompt_length = prompt_length,
  };
  result.error = term_print(term, "\x1b[0m\r\x1b[J");
  if (result.error != CALL_OK) {
    return result;
  }
  result.error = draw_line(&editor, true, false);

  while (result.error == CALL_OK) {
    unsigned key;
    result.error = read_key(term, &key);
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

    bool full = false;
    switch (key) {
    case EDIT_LEFT:
      if (editor.cursor) {
        --editor.cursor;
      }
      break;
    case EDIT_RIGHT:
      if (editor.cursor < editor.length) {
        ++editor.cursor;
      }
      break;
    case EDIT_HOME:
      editor.cursor = 0;
      break;
    case EDIT_END:
      editor.cursor = editor.length;
      break;
    case '\b':
    case 0x7f:
      if (!editor.cursor) {
        continue;
      }
      --editor.cursor;
      [[fallthrough]];
    case EDIT_DELETE:
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

  if (result.status != TERM_LINE_ERROR) {
    enum call_status status = draw_line(&editor, false, false);
    if (status == CALL_OK) {
      status = term_print(term, result.status == TERM_LINE_CANCELLED ? "^C\n" : "\n");
    }
    if (status != CALL_OK) {
      result.status = TERM_LINE_ERROR;
      result.error = status;
    }
  }
  if (result.status == TERM_LINE_OK) {
    result.length = editor.length;
  } else {
    buffer[0] = '\0';
  }
  return result;
}
