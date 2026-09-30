#include <term.h>

#define ESCAPE_TIMEOUT_MS 100

static enum call_status read_key(struct terminal *term, unsigned *key,
                                 bool timed, uint32_t timeout_ms)
{
  if (!term || !key) {
    return CALL_BAD_REQUEST;
  }
  *key = TERM_KEY_UNKNOWN;
  enum { TEXT, ESCAPE, CSI, IGNORE_CSI } state = TEXT;
  unsigned parameter = 0;

  for (;;) {
    unsigned char byte;
    size_t count;
    enum call_status status;
    if (state == TEXT) {
      status = timed ? term_read_timeout(term, &byte, 1, timeout_ms, &count) :
                       term_read(term, &byte, 1, &count);
    } else {
      status = term_read_timeout(term, &byte, 1, ESCAPE_TIMEOUT_MS, &count);
    }
    if (status == CALL_TIMED_OUT) {
      if (state == TEXT) {
        return CALL_TIMED_OUT;
      }
      *key = state == ESCAPE ? 27 : TERM_KEY_UNKNOWN;
      return CALL_OK;
    }
    if (status != CALL_OK) {
      return status;
    }
    if (!count) {
      *key = TERM_KEY_EOF;
      return CALL_OK;
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
      /* Alt-modified text is not emitted by the native console. Preserve an
       * unrelated byte after Escape rather than consuming another key. */
      state = TEXT;
    }
    if (state == CSI || state == IGNORE_CSI) {
      if (byte >= 0x40 && byte <= 0x7e) {
        bool valid = state == CSI;
        state = TEXT;
        if (valid && (parameter == 0 || parameter == 1)) {
          switch (byte) {
          case 'A': *key = TERM_KEY_UP; return CALL_OK;
          case 'B': *key = TERM_KEY_DOWN; return CALL_OK;
          case 'C': *key = TERM_KEY_RIGHT; return CALL_OK;
          case 'D': *key = TERM_KEY_LEFT; return CALL_OK;
          case 'H': *key = TERM_KEY_HOME; return CALL_OK;
          case 'F': *key = TERM_KEY_END; return CALL_OK;
          }
        }
        if (valid && byte == '~') {
          switch (parameter) {
          case 3: *key = TERM_KEY_DELETE; return CALL_OK;
          case 5: *key = TERM_KEY_PAGE_UP; return CALL_OK;
          case 6: *key = TERM_KEY_PAGE_DOWN; return CALL_OK;
          }
        }
        return CALL_OK;
      } else if (state == CSI) {
        if (byte >= '0' && byte <= '9' && parameter < 100) {
          parameter = parameter * 10 + byte - '0';
        } else {
          state = IGNORE_CSI;
        }
      }
      continue;
    }
    *key = byte;
    return CALL_OK;
  }
}

enum call_status term_read_key(struct terminal *term, unsigned *key)
{
  return read_key(term, key, false, 0);
}

enum call_status term_read_key_timeout(struct terminal *term, uint32_t timeout_ms,
                                      unsigned *key)
{
  return read_key(term, key, true, timeout_ms);
}
