#ifndef USERSPACE_PROGRAM_RUNTIME_H
#define USERSPACE_PROGRAM_RUNTIME_H

#include <program_interpreters.h>
#include <program_sources.h>

enum program_runtime_mode {
  PROGRAM_RUNTIME_HANDED,
  PROGRAM_RUNTIME_POSITIONAL,
  PROGRAM_RUNTIME_INTERACTIVE,
};

#ifdef __cplusplus
extern "C" {
#endif

/* Explicit standalone CLI opt-in, before init hooks, -l or user code. Never
 * call automatically from a Lua module/library or an embedding. Captures only
 * held startup source capabilities and retained interpreter FILE identities.
 * Root arguments are borrowed explicit trusted publication selections; absent
 * archive_root may use the captured interpreter_root, never commands/PATH/pkg
 * lookup. The optional self image is a borrowed held FILE, not a name claim.
 * HANDED/POSITIONAL mirror Shell script ownership, INTERACTIVE owns no group.
 * Without own launch authority, language-only modes need no group and launch
 * routing refuses DENIED; no language profile acquires extra permissions.
 * One successful begin per process; sources remain alive until finish. */
enum call_status program_runtime_begin(enum program_interpreter_kind kind,
    enum program_runtime_mode mode, handle_t self_image, handle_t archive_root,
    handle_t package_root);

bool program_runtime_active(void);
bool program_runtime_interactive(void);
const struct program_sources *program_runtime_sources(void);
const struct program_interpreters *program_runtime_interpreters(void);

/* Borrow launchers for foreground run or background spawn. An optional owned
 * command scope is selected first, exactly as in Shell. No permission or new
 * launcher is synthesized. INVALID results mean absent authority. */
enum call_status program_runtime_route(bool foreground, handle_t command_launcher,
    handle_t *admission, handle_t *child);

/* Idempotent cleanup, independent of Lua_State. Stops/waits only the owned
 * script scope, then drops owners. On cleanup failure reports native status,
 * changes success to EXIT_FAILURE and preserves explicit nonzero status.
 * Standalone os.exit must call this too; the C atexit safety net cannot observe
 * the requested exit status and reports a late error without replacing it. */
int program_runtime_finish(int status);
enum call_status program_runtime_cleanup_status(void);

#ifdef __cplusplus
}
#endif

#endif
