#ifndef USERSPACE_SPACE_H
#define USERSPACE_SPACE_H

#include <abi/handle.h>
#include <abi/space.h>
#include <abi/syscall.h>

/* SET_TITLE authority for the caller's space. Nonempty printable ASCII, at
 * most SPACE_TITLE_MAX bytes. Copies the title; caller retains the string. */
enum call_status space_set_title(handle_t space, const char *title);

/* SET_AFFINITY authority for the caller's space, before its first launch.
 * CPUS holds ceil(CPU_COUNT / 64) words; bit N of word N / 64 selects boot CPU
 * N. See <abi/space.h> for the ceiling, BSP and failure rules. */
enum call_status space_set_affinity(handle_t space, const uint64_t *cpus, uint64_t cpu_count);

#endif
