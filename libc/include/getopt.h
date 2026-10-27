#ifndef LIBC_GETOPT_H
#define LIBC_GETOPT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Long-option parsing from musl, sharing getopt's state and diagnostics (see
 * <unistd.h>). Unlike plain getopt, getopt_long permutes ARGV so operands end
 * up after the options, unless OPTSTRING starts with "+" or "-". Unique
 * prefixes of long names match; "--name=value" and "--name value" supply a
 * required argument, and only "--name=value" an optional one. A match returns
 * VAL, or stores it through FLAG and returns 0. INDEX, if not NULL, receives
 * the matching LONGOPTS element. LONGOPTS ends with an all-zero element.
 * getopt_long_only also accepts a single "-" before long names.
 *
 * optreset is musl's non-POSIX way to restart; setting optind to 0 does
 * the same. */
struct option {
  const char *name;
  int has_arg;
  int *flag;
  int val;
};

#define no_argument 0
#define required_argument 1
#define optional_argument 2

int getopt(int argc, char *const argv[], const char *optstring);
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *index);
int getopt_long_only(int argc, char *const argv[], const char *optstring,
                     const struct option *longopts, int *index);
extern char *optarg;
extern int optind, opterr, optopt, optreset;

#ifdef __cplusplus
}
#endif

#endif
