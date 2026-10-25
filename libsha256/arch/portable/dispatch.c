/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <sha256.h>
#include "../../internal.h"

void sha256_compress(uint32_t state[8], const unsigned char *blocks, size_t count)
{
  sha256_portable_blocks(state, blocks, count);
}
