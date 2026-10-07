#include <term.h>
#include <clock.h>
#include <wait.h>

#define ESCAPE_TIMEOUT_MS 100
#define NANOSECONDS_PER_MILLISECOND UINT64_C(1000000)

enum decoder_state { TEXT, ESCAPE, CSI, IGNORE_CSI };

static bool decode_byte(struct term_event_reader *reader, unsigned char byte, unsigned *key)
{
  if (byte == '\x1b') {
    reader->state = ESCAPE;
    reader->parameter = 0;
    return false;
  }
  if (byte == 3 || byte == '\n' || byte == '\r' || byte == '\b' || byte == 0x7f) {
    *key = byte;
    return true;
  }
  if (reader->state == ESCAPE) {
    if (byte == '[') {
      reader->state = CSI;
      return false;
    }
    /* Alt-modified text is not emitted by the native console. Preserve an
     * unrelated byte after Escape rather than consuming another key. */
    reader->state = TEXT;
  }
  if (reader->state == CSI || reader->state == IGNORE_CSI) {
    if (byte >= 0x40 && byte <= 0x7e) {
      bool valid = reader->state == CSI;
      reader->state = TEXT;
      if (valid && (reader->parameter == 0 || reader->parameter == 1)) {
        switch (byte) {
        case 'A': *key = TERM_KEY_UP; return true;
        case 'B': *key = TERM_KEY_DOWN; return true;
        case 'C': *key = TERM_KEY_RIGHT; return true;
        case 'D': *key = TERM_KEY_LEFT; return true;
        case 'H': *key = TERM_KEY_HOME; return true;
        case 'F': *key = TERM_KEY_END; return true;
        }
      }
      if (valid && byte == '~') {
        switch (reader->parameter) {
        case 3: *key = TERM_KEY_DELETE; return true;
        case 5: *key = TERM_KEY_PAGE_UP; return true;
        case 6: *key = TERM_KEY_PAGE_DOWN; return true;
        }
      }
      return true;
    } else if (reader->state == CSI) {
      if (byte >= '0' && byte <= '9' && reader->parameter < 100) {
        reader->parameter = reader->parameter * 10 + byte - '0';
      } else {
        reader->state = IGNORE_CSI;
      }
    }
    return false;
  }
  *key = byte;
  return true;
}

static enum call_status read_key(struct terminal *term, unsigned *key,
    bool timed, uint32_t timeout_ms)
{
  if (!term || !key) {
    return CALL_BAD_REQUEST;
  }
  *key = TERM_KEY_UNKNOWN;
  struct term_event_reader reader = {0};
  for (;;) {
    unsigned char byte;
    size_t count;
    enum call_status status = reader.state == TEXT ?
        (timed ? term_read_timeout(term, &byte, 1, timeout_ms, &count) :
                 term_read(term, &byte, 1, &count)) :
        term_read_timeout(term, &byte, 1, ESCAPE_TIMEOUT_MS, &count);
    if (status == CALL_TIMED_OUT) {
      if (reader.state == TEXT) {
        return CALL_TIMED_OUT;
      }
      *key = reader.state == ESCAPE ? 27 : TERM_KEY_UNKNOWN;
      return CALL_OK;
    }
    if (status != CALL_OK) {
      return status;
    }
    if (!count) {
      *key = TERM_KEY_EOF;
      return CALL_OK;
    }
    if (decode_byte(&reader, byte, key)) {
      return CALL_OK;
    }
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

enum call_status term_event_reader_init(struct term_event_reader *reader,
    struct terminal *term, handle_t clock)
{
  if (!reader || !term) {
    return CALL_BAD_REQUEST;
  }
  *reader = (struct term_event_reader){.term = term, .clock = clock};
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  return status == CALL_OK ? term_geometry(term, &reader->size) : status;
}

static enum call_status read_event(struct term_event_reader *reader,
    struct term_event *event, bool timed, uint32_t timeout_ms)
{
  if (!reader || !reader->term || !event || !reader->size.generation) {
    return CALL_BAD_REQUEST;
  }
  *event = (struct term_event){.kind = TERM_EVENT_KEY, .key = TERM_KEY_UNKNOWN};
  uint64_t initial_deadline = 0;
  if (timed && timeout_ms) {
    uint64_t now;
    enum call_status status = clock_now(reader->clock, &now);
    if (status != CALL_OK) {
      return status;
    }
    uint64_t duration = (uint64_t)timeout_ms * NANOSECONDS_PER_MILLISECOND;
    if (now > UINT64_MAX - duration) {
      return CALL_LIMIT;
    }
    initial_deadline = now + duration;
  }

  for (;;) {
    uint64_t now;
    enum call_status status = clock_now(reader->clock, &now);
    if (status != CALL_OK) {
      return status;
    }
    bool byte_expired = reader->state != TEXT && now >= reader->byte_deadline;
    uint64_t deadline = now > UINT64_MAX - WAIT_MAX_WAIT_NS ? UINT64_MAX :
        now + WAIT_MAX_WAIT_NS;
    if (reader->state != TEXT) {
      deadline = reader->byte_deadline;
    } else if (timed) {
      deadline = timeout_ms ? initial_deadline : 0;
      if (deadline > now && deadline - now > WAIT_MAX_WAIT_NS) {
        deadline = now + WAIT_MAX_WAIT_NS;
      }
    }
    struct wait_interest interests[] = {
      {.handle = reader->term->input, .events = WAIT_READABLE},
      {.handle = reader->term->output, .events = WAIT_RESIZED,
       .observed_generation = reader->size.generation},
    };
    uint64_t events[2];
    status = wait_many(interests, 2, deadline, events);
    if (status == CALL_TIMED_OUT) {
      if (reader->state != TEXT) {
        event->key = reader->state == ESCAPE ? 27 : TERM_KEY_UNKNOWN;
        reader->state = TEXT;
        return CALL_OK;
      }
      if (!timed || deadline < initial_deadline) {
        continue;
      }
      return status;
    }
    if (status != CALL_OK) {
      return status;
    }
    bool readable = events[0] & (WAIT_READABLE | WAIT_PEER_FIN | WAIT_ERROR);
    /* Buffered bytes, EOF and input errors precede an expired byte deadline,
     * just as they do in console timed reads. Resize never renews that deadline. */
    if (byte_expired && !readable) {
      event->key = reader->state == ESCAPE ? 27 : TERM_KEY_UNKNOWN;
      reader->state = TEXT;
      return CALL_OK;
    }
    if (!byte_expired && (events[1] & WAIT_RESIZED)) {
      status = term_geometry(reader->term, &reader->size);
      if (status == CALL_OK) {
        event->kind = TERM_EVENT_RESIZED;
        event->size = reader->size;
      }
      return status;
    }
    if ((events[1] & WAIT_ERROR) && !(events[0] & WAIT_ERROR)) {
      return CALL_UNAVAILABLE;
    }
    if (!readable) {
      if (timed && !timeout_ms && reader->state == TEXT) {
        return CALL_TIMED_OUT;
      }
      continue;
    }
    unsigned char byte;
    size_t count;
    status = term_read_timeout(reader->term, &byte, 1, 0, &count);
    if (status == CALL_TIMED_OUT) {
      continue; /* Another reader may have consumed the observed byte. */
    }
    if (status != CALL_OK || !count) {
      reader->state = TEXT;
      if (status == CALL_OK) {
        event->key = TERM_KEY_EOF;
      }
      return status;
    }
    if (decode_byte(reader, byte, &event->key)) {
      reader->state = TEXT;
      return CALL_OK;
    }
    status = clock_now(reader->clock, &now);
    if (status != CALL_OK) {
      return status;
    }
    uint64_t duration = ESCAPE_TIMEOUT_MS * NANOSECONDS_PER_MILLISECOND;
    if (now > UINT64_MAX - duration) {
      return CALL_LIMIT;
    }
    reader->byte_deadline = now + duration;
  }
}

enum call_status term_read_event(struct term_event_reader *reader, struct term_event *event)
{
  return read_event(reader, event, false, 0);
}

enum call_status term_read_event_timeout(struct term_event_reader *reader,
    uint32_t timeout_ms, struct term_event *event)
{
  return read_event(reader, event, true, timeout_ms);
}
