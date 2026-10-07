#ifndef USERSPACE_POWER_H
#define USERSPACE_POWER_H

#include <abi/handle.h>
#include <abi/power.h>
#include <abi/syscall.h>

/* Borrowed grant with the OFF or RESTART right. The kernel holds user tasks,
 * flushes the native pools and powers off or restarts; success does not
 * return. Failure leaves the system running; see abi/power.h for statuses. */
enum call_status power_off(handle_t power);
enum call_status power_restart(handle_t power);

#endif
