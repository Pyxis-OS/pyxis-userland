#ifndef LIBC_PYXIS_SIGNAL_H
#define LIBC_PYXIS_SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Optional explicit WINCH terminal selection, overriding automatic stdio
 * selection on handler installation. Initial-thread-only. Captures this fd's
 * exact slot generation and SIZE baseline; close/replacement ends its binding.
 * No native grant is copied, no raw mode is changed and no input is consumed.
 * Failure preserves the previous selection. */
int pyxis_signal_set_terminal(int descriptor);

#ifdef __cplusplus
}
#endif

#endif
