#ifndef PKG_PROGRESS_H
#define PKG_PROGRESS_H

#include "archive.h"
#include <abi/handle.h>
#include <stdbool.h>
#include <stdint.h>

#define PKG_PROGRESS_LINE_MAX 160

/* Per-package install display. On a console each phase is one line redrawn in
 * place and kept when the phase ends. Otherwise each phase prints plain lines
 * once, with no carriage returns. No phase predicts a remaining time. */
enum pkg_progress_phase {
  PKG_PHASE_NONE,
  PKG_PHASE_FETCH,
  PKG_PHASE_VERIFY,
  PKG_PHASE_EXTRACT,
};

struct pkg_progress {
  handle_t clock;
  bool redraw;
  const char *id, *version;
  enum pkg_progress_phase phase;
  bool open;
  size_t drawn;
  uint64_t started, drawn_at;
};

void pkg_progress_init(struct pkg_progress *progress, handle_t clock, bool redraw,
    const char *id, const char *version);
/* The provider reports nothing until the whole body is present, so fetch shows
 * the indexed size while blocked and the measured size, time and rate after. */
void pkg_progress_fetch_begin(struct pkg_progress *progress, uint64_t size);
void pkg_progress_fetch_end(struct pkg_progress *progress, uint64_t size);
/* Matches pkg_archive_progress.update. */
void pkg_progress_archive(void *context, enum pkg_archive_phase phase,
    uint64_t done, uint64_t total);
/* Finish a redrawn line left open by a failed phase. */
void pkg_progress_abort(struct pkg_progress *progress);

#endif
