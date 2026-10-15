#ifndef INSTALLER_PUBLICATION_FRONTEND_H
#define INSTALLER_PUBLICATION_FRONTEND_H
#include <term.h>
#include <stdbool.h>
/* Scratch-build entry point; never the installed Install/Update executor. */
int install_publication_scratch(struct terminal *terminal, bool verbose);
#endif
