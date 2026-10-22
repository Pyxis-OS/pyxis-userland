#include "progress.h"

#include <clock.h>
#include <console.h>
#include <pyxis/descriptor.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define COUNTER_CHECK_BYTES UINT64_C(65536)
#define COUNTER_CHECK_ITEMS UINT64_C(32)
#define TERMINAL_INTERVAL_NS UINT64_C(250000000)
#define LONG_PHASE_NS UINT64_C(1000000000)
#define SECOND_NS UINT64_C(1000000000)
#define LINE_BYTES 256
#define DETAIL_BYTES 512
#define BAR_COLUMNS 16

static struct install_progress_record record;
static install_progress_sink observer;
static void *observer_context;
static bool measuring;

static struct {
  handle_t clock, diagnostics;
  FILE *output;
  uint64_t started_ns, last_ns, checked_bytes, checked_items;
  unsigned activity;
  bool terminal, row, enabled, timed, diagnostic_enabled, verbose, closed;
} plain;

static void emit(enum install_progress_event event)
{
  record.event = event;
  if (observer) {
    observer(observer_context, &record);
  }
}

void install_progress_finish(void)
{
  if (measuring) {
    measuring = false;
    emit(INSTALL_PROGRESS_END);
  }
}

void install_progress_bind(install_progress_sink sink, void *context)
{
  install_progress_finish();
  observer = sink;
  observer_context = context;
}

static void begin(const char *phase, enum install_progress_measure measure,
    const char *item_unit, bool major)
{
  install_progress_finish();
  record = (struct install_progress_record){.phase = phase, .measure = measure,
    .item_unit = item_unit, .major = major};
  measuring = true;
  emit(INSTALL_PROGRESS_BEGIN);
}

void install_progress_begin(const char *phase, enum install_progress_measure measure,
    const char *item_unit)
{
  begin(phase, measure, item_unit, false);
}

void install_progress_begin_major(const char *phase, enum install_progress_measure measure,
    const char *item_unit)
{
  begin(phase, measure, item_unit, true);
}

void install_progress_bytes_total(uint64_t total)
{
  if (measuring) {
    record.total_bytes = total;
    record.total_bytes_known = true;
    emit(INSTALL_PROGRESS_UPDATE);
  }
}

void install_progress_items_total(uint64_t total)
{
  if (measuring) {
    record.total_items = total;
    record.total_items_known = true;
    emit(INSTALL_PROGRESS_UPDATE);
  }
}

void install_progress_advance(uint64_t bytes, uint64_t items)
{
  if (!measuring) {
    return;
  }
  /* Observation overflow cannot change the executor result. */
  record.bytes = bytes > UINT64_MAX - record.bytes ? UINT64_MAX : record.bytes + bytes;
  record.items = items > UINT64_MAX - record.items ? UINT64_MAX : record.items + items;
  emit(INSTALL_PROGRESS_UPDATE);
}

void install_progress_io(enum install_progress_measure measure, uint64_t bytes)
{
  if (measuring && record.measure == measure) {
    install_progress_advance(bytes, 0);
  }
}

void install_progress_pause(void)
{
  if (measuring) {
    emit(INSTALL_PROGRESS_PAUSE);
  }
}

static void plain_end_row(void)
{
  if (plain.row) {
    if (fputc('\n', plain.output) == EOF || fflush(plain.output) == EOF) {
      plain.enabled = false;
    }
    plain.row = false;
  }
}

static void detail_write(const char *text, size_t length)
{
  if (plain.diagnostic_enabled &&
      console_write_all(plain.diagnostics, text, length) != CALL_OK) {
    plain.diagnostic_enabled = false;
  }
  if (plain.verbose && plain.enabled &&
      (fwrite(text, 1, length, plain.output) != length || fflush(plain.output) == EOF)) {
    plain.enabled = false;
  }
}

void install_progress_detail(const char *format, ...)
{
  char text[DETAIL_BYTES];
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  if (length < 0) {
    return;
  }
  size_t count = (size_t)length < sizeof(text) - 1 ? (size_t)length : sizeof(text) - 2;
  text[count++] = '\n';
  detail_write(text, count);
}

static void detail_record(const struct install_progress_record *snapshot)
{
  static const char *const events[] = {"BEGIN", "UPDATE", "END", "PAUSE"};
  char items[96] = "", bytes[96] = "";
  if (snapshot->item_unit) {
    if (snapshot->total_items_known) {
      snprintf(items, sizeof(items), " %llu/%llu %s", (unsigned long long)snapshot->items,
          (unsigned long long)snapshot->total_items, snapshot->item_unit);
    } else {
      snprintf(items, sizeof(items), " %llu %s", (unsigned long long)snapshot->items,
          snapshot->item_unit);
    }
  }
  if (snapshot->measure != INSTALL_PROGRESS_NONE) {
    if (snapshot->total_bytes_known) {
      snprintf(bytes, sizeof(bytes), " %llu/%llu bytes", (unsigned long long)snapshot->bytes,
          (unsigned long long)snapshot->total_bytes);
    } else {
      snprintf(bytes, sizeof(bytes), " %llu bytes", (unsigned long long)snapshot->bytes);
    }
  }
  install_progress_detail("installer progress %s %s:%s%s", events[snapshot->event],
      snapshot->phase, items, bytes);
}

static size_t plain_width(void)
{
  if (!plain.terminal) {
    return 79;
  }
  struct pyxis_descriptor_binding binding;
  struct console_size_reply size;
  if (pyxis_descriptor_borrow(fileno(plain.output), &binding) < 0 ||
      console_size(binding.handle, &size) != CALL_OK) {
    plain_end_row();
    plain.terminal = false;
    return 79;
  }
  /* Reserve the final column so an observed line never triggers auto-wrap. */
  return size.columns < LINE_BYTES ? (size.columns ? (size_t)size.columns - 1 : 0) : LINE_BYTES - 1;
}

static void plain_line(const struct install_progress_record *snapshot, uint64_t elapsed,
    bool ending)
{
  if (!plain.enabled || plain.verbose) {
    return;
  }
  size_t width = plain_width();
  if (!width || (!plain.terminal && !ending)) {
    return;
  }
  uint64_t completed = snapshot->bytes, total = snapshot->total_bytes;
  bool known = snapshot->total_bytes_known;
  const char *unit = "bytes";
  if (snapshot->item_unit && (snapshot->total_items_known || !known)) {
    completed = snapshot->items;
    total = snapshot->total_items;
    known = snapshot->total_items_known;
    unit = snapshot->item_unit;
  }
  char counter[128] = "", rate[48] = "", bar[BAR_COLUMNS + 3];
  if (known) {
    snprintf(counter, sizeof(counter), " %llu/%llu %s", (unsigned long long)completed,
        (unsigned long long)total, unit);
  } else if (snapshot->item_unit || snapshot->measure != INSTALL_PROGRESS_NONE) {
    snprintf(counter, sizeof(counter), " %llu %s", (unsigned long long)completed, unit);
  }
  if (plain.timed && elapsed && snapshot->bytes) {
    unsigned __int128 measured = (unsigned __int128)snapshot->bytes * SECOND_NS / elapsed;
    uint64_t per_second = measured > UINT64_MAX ? UINT64_MAX : (uint64_t)measured;
    const char *rate_unit = "B/s";
    if (per_second >= UINT64_C(1048576)) {
      per_second /= UINT64_C(1048576);
      rate_unit = "MiB/s";
    } else if (per_second >= 1024) {
      per_second /= 1024;
      rate_unit = "KiB/s";
    }
    snprintf(rate, sizeof(rate), " %llu %s", (unsigned long long)per_second, rate_unit);
  }
  if (known && total) {
    unsigned filled = completed >= total ? BAR_COLUMNS :
        (unsigned)((unsigned __int128)completed * BAR_COLUMNS / total);
    bar[0] = '[';
    for (unsigned i = 0; i < BAR_COLUMNS; ++i) {
      bar[i + 1] = i < filled ? '#' : '-';
    }
    bar[BAR_COLUMNS + 1] = ']';
    bar[BAR_COLUMNS + 2] = '\0';
  } else {
    snprintf(bar, sizeof(bar), "[%c]", ending ? '-' : "|/-\\"[plain.activity++ % 4]);
  }
  size_t suffix = 1 + strlen(bar) + strlen(counter) + strlen(rate);
  if (suffix + 12 > width) {
    rate[0] = '\0';
    suffix = 1 + strlen(bar) + strlen(counter);
  }
  if (suffix + 12 > width) {
    bar[0] = '\0';
    suffix = strlen(counter);
  }
  if (suffix + 12 > width) {
    counter[0] = '\0';
    suffix = 0;
  }
  char line[LINE_BYTES];
  size_t phase_width = width - suffix;
  int length = snprintf(line, sizeof(line), "%.*s%s%s%s%s", (int)phase_width, snapshot->phase,
      bar[0] ? " " : "", bar, counter, rate);
  if (length < 0) {
    return;
  }
  if (plain.terminal) {
    plain.row = true;
  }
  if ((plain.terminal && fputc('\r', plain.output) == EOF) ||
      fwrite(line, 1, (size_t)length, plain.output) != (size_t)length ||
      (plain.terminal && fputs("\033[K", plain.output) == EOF) ||
      (!plain.terminal && fputc('\n', plain.output) == EOF) || fflush(plain.output) == EOF) {
    plain.enabled = false;
  }
  if (ending || !plain.enabled) {
    plain_end_row();
  }
}

static void plain_observe(void *context, const struct install_progress_record *snapshot)
{
  (void)context;
  if (snapshot->event == INSTALL_PROGRESS_BEGIN) {
    plain.checked_bytes = plain.checked_items = 0;
    plain.activity = 0;
    plain.closed = false;
    plain.timed = clock_coarse_now(plain.clock, &plain.started_ns) == CALL_OK;
    plain.last_ns = plain.started_ns;
    detail_record(snapshot);
    if (snapshot->major) {
      plain_line(snapshot, 0, false);
    }
    return;
  }
  uint64_t elapsed = 0;
  if (snapshot->event == INSTALL_PROGRESS_UPDATE) {
    if (!plain.timed ||
        (snapshot->bytes - plain.checked_bytes < COUNTER_CHECK_BYTES &&
          snapshot->items - plain.checked_items < COUNTER_CHECK_ITEMS)) {
      return;
    }
    plain.checked_bytes = snapshot->bytes;
    plain.checked_items = snapshot->items;
  }
  if (plain.timed) {
    uint64_t now;
    if (clock_coarse_now(plain.clock, &now) != CALL_OK || now < plain.last_ns) {
      plain.timed = false;
      plain_end_row();
    } else {
      elapsed = now - plain.started_ns;
      if (snapshot->event == INSTALL_PROGRESS_UPDATE && now - plain.last_ns < TERMINAL_INTERVAL_NS) {
        return;
      }
      plain.last_ns = now;
    }
  }
  detail_record(snapshot);
  if (snapshot->event == INSTALL_PROGRESS_PAUSE || snapshot->event == INSTALL_PROGRESS_END) {
    if (!plain.closed && (snapshot->major || plain.row || elapsed >= LONG_PHASE_NS)) {
      plain_line(snapshot, elapsed, true);
    }
    plain_end_row();
    plain.closed = true;
  } else if (plain.timed && (snapshot->major || elapsed >= LONG_PHASE_NS)) {
    plain.closed = false;
    plain_line(snapshot, elapsed, false);
  }
}

void install_progress_plain_stream(handle_t clock, FILE *output, handle_t diagnostics,
    bool verbose)
{
  install_progress_finish();
  plain = (typeof(plain)){.clock = clock, .output = output, .diagnostics = diagnostics,
    .terminal = isatty(fileno(output)), .enabled = true,
    .diagnostic_enabled = diagnostics != HANDLE_INVALID, .verbose = verbose};
  install_progress_bind(plain_observe, NULL);
}
