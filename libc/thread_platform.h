#ifndef LIBC_THREAD_PLATFORM_H
#define LIBC_THREAD_PLATFORM_H

#include <abi/handle.h>
#include <stdbool.h>
#include <stdint.h>

struct libc_once_frame;

/* These are private retained startup grants, never the caller-only FS grant.
 * An absent optional service remains absent; the runtime adds no authority. */
handle_t libc_thread_service(void);
handle_t libc_thread_clock(void);
handle_t libc_thread_sleep_clock(void);
uint64_t libc_thread_self(void);
void libc_thread_runtime_init(void);
bool libc_thread_shutdown_ready(void);

struct libc_once_frame *libc_thread_once_head(void);
void libc_thread_once_push(struct libc_once_frame *frame);
void libc_thread_once_pop(struct libc_once_frame *frame);

#endif
