#ifndef TLSF_CAELUM_H
#define TLSF_CAELUM_H
#include <limits.h>
#include <stddef.h>
#include <kernel/log.h>
#include <kernel/memory.h>
#include <kernel/panic.h>

/* Retain upstream's checks and diagnostics in the freestanding kernel. */
#define tlsf_assert(condition) KASSERT(condition)
#define printf klog
#endif
