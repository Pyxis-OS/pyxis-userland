#ifndef INSTALLER_PUBLICATION_FRONTEND_H
#define INSTALLER_PUBLICATION_FRONTEND_H
#include <term.h>
#include <stdbool.h>
/* Explicit disposable-image frontend; never offered by the ordinary UI. */
int install_publication_scratch(struct terminal *terminal, bool verbose);
/* Fresh explicit completion Review for one selected existing installation.
 * No new publication/attempt is permitted; all identities and pending evidence
 * are revalidated. Both Classic and Coexist use their existing scoped grants. */
int install_publication_complete(struct terminal *terminal, bool verbose, uint64_t disk,
    uint64_t esp, uint64_t pool, const char *guid);
#endif
