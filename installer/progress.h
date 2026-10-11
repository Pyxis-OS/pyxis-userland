#ifndef USERSPACE_INSTALLER_PROGRESS_H
#define USERSPACE_INSTALLER_PROGRESS_H

#include <abi/types.h>
#include <stdbool.h>
#include <stdint.h>

enum install_progress_measure {
  INSTALL_PROGRESS_NONE,
  INSTALL_PROGRESS_RAW_READ,
  INSTALL_PROGRESS_RAW_WRITE,
  INSTALL_PROGRESS_SOURCE_READ,
  INSTALL_PROGRESS_WORK,
};

enum install_progress_event {
  INSTALL_PROGRESS_BEGIN,
  INSTALL_PROGRESS_UPDATE,
  INSTALL_PROGRESS_END,
  INSTALL_PROGRESS_PAUSE,
};

struct install_progress_record {
  enum install_progress_event event;
  enum install_progress_measure measure;
  const char *phase, *item_unit;
  uint64_t bytes, items, total_bytes, total_items;
  bool total_bytes_known, total_items_known;
};

/* One synchronous observer for the single-threaded executor. Labels are static;
 * the observer cannot retain the record pointer or reenter progress operations.
 * Events describe observed work, never operation success or durability. */
typedef void (*install_progress_sink)(void *context, const struct install_progress_record *record);
void install_progress_bind(install_progress_sink sink, void *context);
void install_progress_plain(handle_t clock);
void install_progress_begin(const char *phase, enum install_progress_measure measure,
    const char *item_unit);
void install_progress_bytes_total(uint64_t total);
void install_progress_items_total(uint64_t total);
void install_progress_advance(uint64_t bytes, uint64_t items);
void install_progress_io(enum install_progress_measure measure, uint64_t bytes);
void install_progress_pause(void);
void install_progress_finish(void);

#endif
