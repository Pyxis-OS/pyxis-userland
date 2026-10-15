#ifndef USERSPACE_SHELL_VALUES_H
#define USERSPACE_SHELL_VALUES_H

#include "syntax.h"

enum command_result shell_assign(struct shell *shell,
    const struct shell_expanded_word *word);
bool shell_value_builtin(const char *name);
enum command_result shell_values_command(struct shell *shell,
    const struct shell_expansion *expansion);

#endif
