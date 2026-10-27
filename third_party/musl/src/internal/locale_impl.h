#ifndef PYXIS_MUSL_LOCALE_IMPL_H
#define PYXIS_MUSL_LOCALE_IMPL_H

/* Pyxis: stands in for musl's private locale header, which getopt.c includes.
 * Libc has only the C locale and no message catalogs, so diagnostics are not
 * translated. weak_alias is musl's definition from its libc.h. */
#define __lctrans_cur(msg) (msg)

#define weak_alias(old, new) \
	extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))

#endif
