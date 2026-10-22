#ifndef USERSPACE_COMMAND_SELECTION_H
#define USERSPACE_COMMAND_SELECTION_H

#include <command_search.h>
#include <package_commands.h>
#include <bundle.h>

/* An owned resolution, kept alive through launch preparation and admission.
 * IMAGE is borrowed from BUNDLE when non-NULL, otherwise independently owned.
 * PACKAGE_VIEW retains the fixed set, including bundle resource provenance.
 * Diagnostic labels/paths never confer authority. */
struct command_selection {
  char *explicit_path; /* Full diagnostic path, owned; NULL for a bare name. */
  handle_t commands; /* Owned originating list; zero for an explicit path. */
  bool commands_exported;
  uint64_t commands_identity;
  handle_t image;
  struct bundle_program *bundle;
  handle_t package_view;
  size_t ordinal;
  char label[COMMAND_LIST_LABEL_MAX + 1];
  char name[PACKAGE_COMMAND_NAME_MAX + 1];
  char path[PACKAGE_COMMAND_NAME_MAX + 5];
  bool package;
  struct package_command_info origin;
  struct package_command_record target;
};

/* Resolve only this held list, without defaults or implicit fallback roots.
 * Every live package volume is captured once, even when its first entry misses.
 * Subsequent entries reuse that set; selected errors never fall through.
 * Conventional directories admit NAME.pxe or NAME.pxb, refusing a collision.
 * No launch, arbitrary package authority or process replacement occurs here.
 * Launch adapters retain the same selection through preparation/admission. */
enum call_status command_search_resolve(const struct path_context *context,
    const struct command_search_capture *search, const char *name,
    struct command_selection **selection);
/* Room for one refusal line, including its NUL. */
#define COMMAND_REFUSAL_MAX 512
/* As command_search_resolve. A package command whose package's ABI the running
 * system does not provide, as read from the caller's boot://os-release, is
 * refused with CALL_DENIED before anything runs (the read handle the kernel's
 * lookup returned is closed, and no selection is made); a non-NULL REFUSAL
 * of COMMAND_REFUSAL_MAX bytes then receives one line naming the package, both
 * sides and the remedy, and is empty otherwise. */
enum call_status command_search_resolve_reason(const struct path_context *context,
    const struct command_search_capture *search, const char *name,
    struct command_selection **selection, char *refusal);
void command_selection_close(struct command_selection *selection);

#endif
