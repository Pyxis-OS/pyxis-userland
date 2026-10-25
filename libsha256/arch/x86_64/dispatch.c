/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <sha256.h>
#include <stdatomic.h>
#include "../../internal.h"

enum backend {
  BACKEND_UNSELECTED,
  BACKEND_PORTABLE,
  BACKEND_SHA_NI,
};

static atomic_uint selected_backend;

void sha256_compress(uint32_t state[8], const unsigned char *blocks, size_t count)
{
  if (!count) {
    return;
  }
  unsigned selected = atomic_load_explicit(&selected_backend, memory_order_relaxed);
  if (selected == BACKEND_UNSELECTED) {
    selected = sha256_x86_available() ? BACKEND_SHA_NI : BACKEND_PORTABLE;
    /* Independent first callers may repeat CPUID; there is no mutable table or
     * partially initialized state to publish, and no thread capability needed. */
    atomic_store_explicit(&selected_backend, selected, memory_order_relaxed);
  }
  if (selected == BACKEND_SHA_NI) {
    sha256_x86_blocks(state, blocks, count);
  } else {
    sha256_portable_blocks(state, blocks, count);
  }
}
