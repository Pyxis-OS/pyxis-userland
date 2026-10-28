#ifndef USERSPACE_PROGRAM_INTERPRETERS_H
#define USERSPACE_PROGRAM_INTERPRETERS_H

#include <abi/launcher.h>
#include <abi/syscall.h>
#include <stddef.h>

enum program_interpreter_kind {
  PROGRAM_INTERPRETER_NONE,
  PROGRAM_INTERPRETER_SHELL,
  PROGRAM_INTERPRETER_LUA,
  PROGRAM_INTERPRETER_LUA51,
};

struct program_interpreter_record {
  handle_t image;
  uint64_t domain, object;
};

/* Code-only owner records, not authority sources. Each image owns native FILE
 * READ; identity remains meaningful because this owner retains the FILE.
 * Zero initialize, do not copy the owner, and close after admission borrows end. */
struct program_interpreters {
  struct program_interpreter_record records[3];
};

/* Explicit private interpreter.shell/interpreter.lua/interpreter.lua51 records
 * win over optional trusted roots. A supplied held self FILE overrides its own
 * kind. Roots are caller-selected trusted publication views, never commands or
 * mutable name searches; they and environment are borrowed only during this
 * call. INVALID means absent. Package candidates precede archive candidates;
 * only NOT_FOUND permits fallback. Wrong type, denial and invalid identity are
 * final. Failure closes partial records and leaves an empty owner. */
enum call_status program_interpreters_capture(handle_t environment, handle_t archive_root,
    handle_t package_root, enum program_interpreter_kind self_kind, handle_t self_image,
    struct program_interpreters *interpreters);
void program_interpreters_close(struct program_interpreters *interpreters);

/* NONE on missing/unusable identity or a nonmatching native FILE. Never treats
 * names, content hashes, argv or claims made by the candidate as identity. */
enum program_interpreter_kind program_interpreters_match(
    const struct program_interpreters *interpreters, handle_t candidate);

/* Append private FILE READ metadata, no runtime grants. Caller arrays and owner
 * remain borrowed through synchronous admission. Duplicate names refuse, and
 * failure leaves request counts unchanged. Array spare slots may be written. */
enum call_status program_interpreters_stage(const struct program_interpreters *interpreters,
    struct launch_request *request, struct launch_grant *grants, size_t grant_capacity,
    struct launch_environment_entry *entries, size_t entry_capacity);

#endif
