#ifndef INIT_INSTALL_UPDATE_H
#define INIT_INSTALL_UPDATE_H

#include "../installer/installer.h"
#include <term.h>

struct installer_update_choice {
  uint64_t disk;
  struct install_layout layout;
  bool coexist;
};

/* Discover through RO GPT, native pool and ESP validators, then select exactly
 * one recognized installation. No mount, replay or write of the native pool.
 * All native RO ESP views retire before success; the child must inspect again.
 * Back, cancellation or no eligible target succeeds with back set. Inspection
 * cleanup failure prevents launch even when another disk supplied a candidate. */
bool installer_update_choose(struct terminal *terminal, handle_t disks, handle_t clock,
    handle_t random, struct installer_update_choice *choice, bool *back);

#endif
