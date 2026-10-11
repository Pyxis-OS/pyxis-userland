#include "progress.h"

#include <clock.h>
#include <stdio.h>
#include <unistd.h>

#define COUNTER_CHECK_BYTES UINT64_C(65536)
#define COUNTER_CHECK_ITEMS UINT64_C(32)
#define TERMINAL_INTERVAL_NS UINT64_C(250000000)
#define PLAIN_INTERVAL_NS UINT64_C(1000000000)

static struct install_progress_record record;
static install_progress_sink observer;
static void *observer_context;
static bool measuring;

static struct {
  handle_t clock;
  FILE *output;
  uint64_t last_ns, checked_bytes, checked_items;
  bool terminal, row, enabled, timed;
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

void install_progress_begin(const char *phase, enum install_progress_measure measure,
    const char *item_unit)
{
  install_progress_finish();
  record = (struct install_progress_record){.phase = phase, .measure = measure,
    .item_unit = item_unit};
  measuring = true;
  emit(INSTALL_PROGRESS_BEGIN);
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
    if (fputc('\n', plain.output) == EOF) {
      plain.enabled = false;
    }
    plain.row = false;
  }
}

static bool plain_counter(const struct install_progress_record *snapshot)
{
  if (fprintf(plain.output, "%s%s: ", plain.terminal ? "\r" : "", snapshot->phase) < 0) {
    return false;
  }
  if (snapshot->item_unit) {
    if (fprintf(plain.output, "%llu", (unsigned long long)snapshot->items) < 0 ||
        (snapshot->total_items_known &&
          fprintf(plain.output, "/%llu", (unsigned long long)snapshot->total_items) < 0) ||
        fprintf(plain.output, " %s%s", snapshot->item_unit,
          snapshot->measure == INSTALL_PROGRESS_NONE ? "" : ", ") < 0) {
      return false;
    }
  }
  if (snapshot->measure != INSTALL_PROGRESS_NONE) {
    if (fprintf(plain.output, "%llu", (unsigned long long)snapshot->bytes) < 0 ||
        (snapshot->total_bytes_known &&
          fprintf(plain.output, "/%llu", (unsigned long long)snapshot->total_bytes) < 0) ||
        fputs(" bytes", plain.output) == EOF) {
      return false;
    }
  }
  return plain.terminal || fputc('\n', plain.output) != EOF;
}

static void plain_observe(void *context, const struct install_progress_record *snapshot)
{
  (void)context;
  if (!plain.enabled) {
    return;
  }
  if (snapshot->event == INSTALL_PROGRESS_PAUSE) {
    plain_end_row();
    return;
  }
  if (snapshot->event == INSTALL_PROGRESS_BEGIN) {
    plain.checked_bytes = plain.checked_items = 0;
    if (fprintf(plain.output, "%s...\n", snapshot->phase) < 0) {
      plain.enabled = false;
    }
    plain.timed = clock_now(plain.clock, &plain.last_ns) == CALL_OK;
    return;
  }
  if (snapshot->event == INSTALL_PROGRESS_UPDATE) {
    if (!plain.timed ||
        (snapshot->bytes - plain.checked_bytes < COUNTER_CHECK_BYTES &&
          snapshot->items - plain.checked_items < COUNTER_CHECK_ITEMS)) {
      return;
    }
    plain.checked_bytes = snapshot->bytes;
    plain.checked_items = snapshot->items;
    uint64_t now;
    if (clock_now(plain.clock, &now) != CALL_OK || now < plain.last_ns) {
      plain.timed = false;
      plain_end_row();
      return;
    }
    uint64_t interval = plain.terminal ? TERMINAL_INTERVAL_NS : PLAIN_INTERVAL_NS;
    if (now - plain.last_ns < interval) {
      return;
    }
    plain.last_ns = now;
  }
  if (snapshot->measure != INSTALL_PROGRESS_NONE || snapshot->item_unit) {
    plain.row = plain.terminal;
    if (!plain_counter(snapshot)) {
      plain.enabled = false;
    }
  }
  if (snapshot->event == INSTALL_PROGRESS_END || !plain.enabled) {
    plain_end_row();
  }
}

void install_progress_plain_stream(handle_t clock, FILE *output)
{
  install_progress_finish();
  plain = (typeof(plain)){.clock = clock, .output = output,
    .terminal = isatty(fileno(output)), .enabled = true};
  install_progress_bind(plain_observe, NULL);
}

void install_progress_plain(handle_t clock)
{
  install_progress_plain_stream(clock, stdout);
}
