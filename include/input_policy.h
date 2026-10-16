#ifndef USERSPACE_INPUT_POLICY_H
#define USERSPACE_INPUT_POLICY_H

#include <abi/handle.h>
#include <abi/input_policy.h>
#include <abi/syscall.h>

/* Borrowed stock-boot-init-only grant. Installs directions and speed once for this
 * boot; no child delegation, query or live update is provided. */
enum call_status input_policy_install_once(handle_t input_policy,
    enum input_scroll_direction mouse, enum input_scroll_direction touchpad,
    int32_t touchpad_speed);

#endif
