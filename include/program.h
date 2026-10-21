#ifndef USERSPACE_PROGRAM_H
#define USERSPACE_PROGRAM_H

#include <command_selection.h>

/* Scratch for explicit paths: COMMAND plus its NUL, and that many directory
 * slots plus CONTEXT's depth. Bare names need no path workspace. */
#define PROGRAM_LOOKUP_EXTRA 1

#ifdef __cplusplus
extern "C" {
#endif

bool program_is_explicit(const char *command);
/* Bare names resolve only through the supplied immutable command-list capture.
 * An absent/empty list means no search. Explicit paths use their held roots/cwd;
 * .pxb paths retain the complete bundle. Neither bin nor cwd is an implicit
 * bare-name fallback. Success owns *selection through preparation/admission. */
enum call_status program_open_search(const struct path_context *context,
    const struct command_search_capture *search, const char *command,
    struct path_workspace *workspace, struct command_selection **selection);
/* As program_open_search; REFUSAL, when given, receives the refusal line
 * described for command_search_resolve_reason. */
enum call_status program_open_search_reason(const struct path_context *context,
    const struct command_search_capture *search, const char *command,
    struct path_workspace *workspace, struct command_selection **selection, char *refusal);
/* Capture the caller's current commands entry once for a bare name. No default
 * entry is added; malformed/revoked lists fail. Explicit paths need no list. */
enum call_status program_open(const struct path_context *context, const char *command,
    struct path_workspace *workspace, struct command_selection **selection);
enum call_status program_open_reason(const struct path_context *context, const char *command,
    struct path_workspace *workspace, struct command_selection **selection, char *refusal);
/* For PATH-aware adapters only: absent PATH uses SEARCH. Otherwise only the
 * exact colon-joined diagnostic labels are supported. Text never grants search
 * authority; an empty projection is empty, never cwd. */
enum call_status program_search_path_validate(const char *path,
    const struct command_search_capture *search);

#ifdef __cplusplus
}
#endif

#endif
