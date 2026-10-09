#include <handle.h>
#include <term.h>
#include <stdlib.h>
#include <string.h>
#include <startup.h>

struct line_editor {
  struct terminal *term;
  const char *prompt;
  char *buffer;
  size_t columns, cells, prompt_length;
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
  size_t total = editor->prompt_length + editor->length;
  size_t caret = editor->prompt_length + (editing ? editor->cursor : editor->length);
  size_t visible = total < editor->cells ? total : editor->cells - 1;
  size_t start = 0;
  if (total >= editor->cells) {
    start = caret > visible / 2 ? caret - visible / 2 : 0;
    if (start > total - visible) {
      start = total - visible;
    }
  }
  size_t prompt_count = start < editor->prompt_length ? editor->prompt_length - start : 0;
  if (prompt_count > visible) {
    prompt_count = visible;
  }
  status = term_write_all(term, editor->prompt + (prompt_count ? start : 0), prompt_count);
  if (status == CALL_OK && visible > prompt_count) {
    size_t input_start = start + prompt_count - editor->prompt_length;
    status = term_write_all(term, editor->buffer + input_start, visible - prompt_count);
  }
  if (status != CALL_OK) {
    return status;
  }

  /* The visible span, including its cursor cell, fits even after scrolling.
   * The retained prompt and input are independent of this temporary window. */
  size_t span = visible > editor->displayed_length ? visible : editor->displayed_length;
  status = write_spaces(term, span - visible + 1);
  if (status != CALL_OK) {
    return status;
  }
  size_t end = span + 1;
  if (end % editor->columns == 0) {
    --end; /* CR cancels the delayed wrap at the right margin. */
  }
  size_t target = caret - start;
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
    editor->displayed_length = visible;
    editor->displayed_cursor = target;
  }
  return status;
}

void term_history_free(struct term_history *history)
{
  if (!history) {
    return;
  }
  for (size_t i = 0; i < history->count; ++i) {
    free(history->entries[i]);
    history->entries[i] = NULL;
  }
  history->count = 0;
}

static void history_add(struct term_history *history, const char *line, size_t length)
{
  size_t spaces = 0;
  while (spaces < length && line[spaces] == ' ') {
    ++spaces;
  }
  if (spaces == length) {
    return;
  }
  if (history->count && !strcmp(history->entries[history->count - 1], line)) {
    return;
  }
  char *copy = malloc(length + 1);
  if (!copy) {
    return;
  }
  memcpy(copy, line, length + 1);
  if (history->count == TERM_HISTORY_ENTRIES) {
    free(history->entries[0]);
    memmove(history->entries, history->entries + 1,
        (TERM_HISTORY_ENTRIES - 1) * sizeof(history->entries[0]));
    --history->count;
  }
  history->entries[history->count++] = copy;
}

/* Replaces the edited text, shortening it to the buffer limit. Returns whether
 * it had to be shortened. */
static bool load_line(struct line_editor *editor, const char *text, size_t limit)
{
  size_t length = strlen(text);
  bool shortened = length > limit;
  if (shortened) {
    length = limit;
  }
  memcpy(editor->buffer, text, length);
  editor->buffer[length] = '\0';
  editor->length = editor->cursor = length;
  return shortened;
}

static struct term_line_result read_line(struct terminal *term, const char *prompt,
    const char *initial, struct term_history *history, char *buffer, size_t capacity,
    bool quiet, bool marked)
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
  if (prompt_length > SIZE_MAX - capacity) {
    result.error = CALL_LIMIT;
    return result;
  }
  size_t limit = capacity - 1;
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
  struct line_editor editor = {
    .term = term, .prompt = prompt, .buffer = buffer,
    .columns = columns, .cells = cells, .prompt_length = prompt_length, .quiet = quiet,
    .length = initial_length, .cursor = initial_length,
  };
  struct term_event_reader reader;
  handle_t clock = startup_resource("clock");
  bool adaptive = false;
  if (clock != HANDLE_INVALID) {
    result.error = term_event_reader_init(&reader, term, clock);
    if (result.error == CALL_OK) {
      if (reader.size.columns > UINT16_MAX || reader.size.rows > UINT16_MAX) {
        result.error = CALL_BAD_REQUEST;
        return result;
      }
      adaptive = true;
      editor.columns = reader.size.columns;
      editor.cells = reader.size.columns * reader.size.rows;
    } else if (result.error != CALL_DENIED && result.error != CALL_BAD_HANDLE) {
      return result;
    }
  }
  result.error = CALL_OK;
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
  memcpy(buffer, initial, initial_length + 1);

  /* position == count is the unfinished line, saved in draft while an entry
   * is shown. */
  size_t position = history ? history->count : 0;
  char *draft = NULL;
  result.error = draw_line(&editor, true, false);
  if (result.error == CALL_OK && marked) {
    result.error = term_print(term, "\x1b]133;B\a");
  }

  while (result.error == CALL_OK) {
    unsigned key;
    struct term_event event;
    result.error = adaptive ? term_read_event(&reader, &event) : term_read_key(term, &key);
    if (result.error == CALL_OK && adaptive) {
      if (event.kind == TERM_EVENT_RESIZED) {
        if (event.size.columns > UINT16_MAX || event.size.rows > UINT16_MAX) {
          result.error = CALL_BAD_REQUEST;
          break;
        }
        if (!quiet) {
          /* Old raster rows retain their order across resize. Recover the old
           * line origin before adopting the new width; upward movement clamps
           * at row zero if part of the old span has disappeared. */
          result.error = move_between(&editor, editor.displayed_cursor, 0);
          if (result.error == CALL_OK) {
            result.error = term_print(term, "\x1b[J");
          }
        }
        editor.columns = event.size.columns;
        editor.cells = event.size.columns * event.size.rows;
        editor.displayed_length = editor.displayed_cursor = 0;
        if (result.error == CALL_OK) {
          result.error = draw_line(&editor, true, false);
        }
        continue;
      }
      key = event.key;
    }
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
    case TERM_KEY_UP:
    case TERM_KEY_DOWN:
      if (!history || (key == TERM_KEY_UP ? !position : position == history->count)) {
        continue;
      }
      if (position == history->count) {
        if (!draft) {
          draft = malloc(capacity);
          if (!draft) {
            continue;
          }
        }
        memcpy(draft, buffer, editor.length + 1);
      }
      if (key == TERM_KEY_UP) {
        --position;
      } else {
        ++position;
      }
      full = load_line(&editor, position == history->count ? draft : history->entries[position],
          limit);
      if (full) {
        result.limit_reached = true;
      }
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
    if (history) {
      history_add(history, buffer, editor.length);
    }
  } else {
    buffer[0] = '\0';
  }
  free(draft);
  return result;
}

/* Ctrl+C belongs to the editor only while a line is being read; code the
 * application runs between lines stays interruptible. */
static struct term_line_result read_line_passthrough(struct terminal *term,
    const char *prompt, const char *initial, struct term_history *history, char *buffer,
    size_t capacity, bool quiet, bool marked)
{
  /* Invalid arguments keep read_line's handled BAD_REQUEST result. */
  if (!term || !prompt || !buffer || !capacity) {
    return read_line(term, prompt, initial, history, buffer, capacity, quiet, marked);
  }
  handle_t passthrough;
  bool held = term_passthrough(term, &passthrough) == CALL_OK;
  struct term_line_result result = read_line(term, prompt, initial, history, buffer, capacity,
      quiet, marked);
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
  return read_line_passthrough(term, prompt, "", NULL, buffer, capacity, false, false);
}

struct term_line_result term_read_line_history(struct terminal *term, const char *prompt,
    struct term_history *history, char *buffer, size_t capacity)
{
  return read_line_passthrough(term, prompt, "", history, buffer, capacity, false, false);
}

struct term_line_result term_read_line_quiet(struct terminal *term, const char *prompt,
                                            char *buffer, size_t capacity)
{
  return read_line_passthrough(term, prompt, "", NULL, buffer, capacity, true, false);
}

struct term_line_result term_read_line_initial(struct terminal *term, const char *prompt,
    const char *initial, char *buffer, size_t capacity)
{
  return read_line_passthrough(term, prompt, initial, NULL, buffer, capacity, false, false);
}

struct term_line_result term_read_line_marked(struct terminal *term, const char *prompt,
    struct term_history *history, char *buffer, size_t capacity)
{
  return read_line_passthrough(term, prompt, "", history, buffer, capacity, false, true);
}
