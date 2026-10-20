#include "progress.h"
#include <clock.h>
#include <stdio.h>
#include <string.h>

#define REDRAW_INTERVAL_NS UINT64_C(200000000)
#define NS_PER_MS UINT64_C(1000000)
#define MS_PER_SECOND UINT64_C(1000)
#define KIB UINT64_C(1024)
#define MIB (KIB * KIB)

static bool now_ns(const struct pkg_progress *progress, uint64_t *now)
{
  return clock_now(progress->clock, now) == CALL_OK;
}

/* Whole units with one decimal, from integers only. */
static void format_size(char *text, size_t capacity, uint64_t bytes, uint64_t unit)
{
  snprintf(text, capacity, "%llu.%llu", (unsigned long long)(bytes / unit),
      (unsigned long long)((bytes % unit) * 10 / unit));
}

static const char *size_unit(uint64_t bytes)
{
  return bytes >= MIB ? "MiB" : "KiB";
}

static uint64_t unit_bytes(uint64_t bytes)
{
  return bytes >= MIB ? MIB : KIB;
}

static unsigned percent(uint64_t done, uint64_t total)
{
  return total ? (unsigned)(done * 100 / total) : 100;
}

/* One line for the current phase: redrawn over the previous text on a console,
 * ignored otherwise so a plain stream sees only the explicit lines. */
static void draw(struct pkg_progress *progress, const char *text)
{
  if (!progress->redraw) {
    return;
  }
  size_t length = strlen(text);
  printf("\r%s%*s", text, progress->drawn > length ? (int)(progress->drawn - length) : 0, "");
  fflush(stdout);
  progress->drawn = length;
  progress->open = true;
}

static void finish_line(struct pkg_progress *progress)
{
  if (progress->open) {
    putchar('\n');
    fflush(stdout);
  }
  progress->open = false;
  progress->drawn = 0;
}

void pkg_progress_init(struct pkg_progress *progress, handle_t clock, bool redraw,
    const char *id, const char *version)
{
  *progress = (struct pkg_progress){
    .clock = clock, .redraw = redraw, .id = id, .version = version, .phase = PKG_PHASE_NONE,
  };
}

void pkg_progress_fetch_begin(struct pkg_progress *progress, uint64_t size)
{
  progress->phase = PKG_PHASE_FETCH;
  if (!now_ns(progress, &progress->started)) {
    progress->started = 0;
  }
  if (!progress->redraw) {
    printf("pkg: fetching %s@%s (%llu bytes)\n", progress->id, progress->version,
        (unsigned long long)size);
    return;
  }
  char amount[24], text[PKG_PROGRESS_LINE_MAX];
  format_size(amount, sizeof(amount), size, unit_bytes(size));
  snprintf(text, sizeof(text), "%s  fetch  %s %s  downloading", progress->id, amount,
      size_unit(size));
  draw(progress, text);
}

void pkg_progress_fetch_end(struct pkg_progress *progress, uint64_t size)
{
  uint64_t now = 0, elapsed_ms = 0;
  bool timed = progress->started && now_ns(progress, &now) && now > progress->started;
  if (timed) {
    elapsed_ms = (now - progress->started) / NS_PER_MS;
    if (!elapsed_ms) {
      elapsed_ms = 1;
    }
  }
  char seconds[40] = "", rate[48] = "", amount[24], text[PKG_PROGRESS_LINE_MAX];
  if (timed) {
    snprintf(seconds, sizeof(seconds), "%llu.%llu s",
        (unsigned long long)(elapsed_ms / MS_PER_SECOND),
        (unsigned long long)(elapsed_ms % MS_PER_SECOND / 100));
    uint64_t per_second = size * MS_PER_SECOND / elapsed_ms;
    char number[24];
    format_size(number, sizeof(number), per_second, unit_bytes(per_second));
    snprintf(rate, sizeof(rate), "%s %s/s", number, size_unit(per_second));
  }
  if (!progress->redraw) {
    printf("pkg: fetched %s@%s: %llu bytes", progress->id, progress->version,
        (unsigned long long)size);
    if (timed) {
      printf(" in %s (%s)", seconds, rate);
    }
    putchar('\n');
    return;
  }
  format_size(amount, sizeof(amount), size, unit_bytes(size));
  snprintf(text, sizeof(text), "%s  fetch  %s %s%s%s%s%s", progress->id, amount,
      size_unit(size), timed ? "  " : "", seconds, timed ? "  " : "", rate);
  draw(progress, text);
  finish_line(progress);
}

void pkg_progress_archive(void *context, enum pkg_archive_phase phase,
    uint64_t done, uint64_t total)
{
  struct pkg_progress *progress = context;
  enum pkg_progress_phase current =
      phase == PKG_ARCHIVE_VERIFY ? PKG_PHASE_VERIFY : PKG_PHASE_EXTRACT;
  bool finished = done == total;
  if (current != progress->phase) {
    finish_line(progress);
    progress->phase = current;
    progress->drawn_at = 0;
  }
  uint64_t now = 0;
  if (!finished && progress->redraw && (!now_ns(progress, &now) ||
      (progress->drawn_at && now - progress->drawn_at < REDRAW_INTERVAL_NS))) {
    return;
  }
  progress->drawn_at = now;
  if (!progress->redraw) {
    if (finished && phase == PKG_ARCHIVE_VERIFY) {
      printf("pkg: verified %s@%s: %llu bytes\n", progress->id, progress->version,
          (unsigned long long)total);
    } else if (finished) {
      printf("pkg: extracted %s@%s: %llu files\n", progress->id, progress->version,
          (unsigned long long)total);
    }
    return;
  }
  char text[PKG_PROGRESS_LINE_MAX];
  if (phase == PKG_ARCHIVE_VERIFY) {
    char seen[24], whole[24];
    format_size(seen, sizeof(seen), done, unit_bytes(total));
    format_size(whole, sizeof(whole), total, unit_bytes(total));
    snprintf(text, sizeof(text), "%s  verify  %s/%s %s  %u%%", progress->id, seen, whole,
        size_unit(total), percent(done, total));
  } else {
    snprintf(text, sizeof(text), "%s  extract  %llu/%llu files  %u%%", progress->id,
        (unsigned long long)done, (unsigned long long)total, percent(done, total));
  }
  draw(progress, text);
  if (finished) {
    finish_line(progress);
  }
}

void pkg_progress_abort(struct pkg_progress *progress)
{
  finish_line(progress);
  progress->phase = PKG_PHASE_NONE;
}
