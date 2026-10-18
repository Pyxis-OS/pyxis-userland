#include "battery.h"

#include <system_info.h>

#define INSTALL_MIN_BATTERY_PERCENT 40u

static bool unreadable(FILE *output)
{
  fputs("installer: battery state is unreadable; connect AC and try again. Nothing written.\n", output);
  return false;
}

bool install_battery_check(handle_t system_info, FILE *output)
{
  struct system_info_power power;
  if (system_info_get_power(system_info, &power) != CALL_OK) {
    return unreadable(output);
  }
  if (power.ac == SYSTEM_INFO_AC_ONLINE || !power.battery_count) {
    return true;
  }
  uint64_t remaining = 0, full = 0;
  uint32_t unit = SYSTEM_INFO_BATTERY_UNKNOWN;
  for (uint32_t i = 0; i < power.battery_count; ++i) {
    struct system_info_battery battery;
    if (system_info_get_battery(system_info, i, &battery) != CALL_OK ||
        !battery.sample_ns) {
      return unreadable(output);
    }
    if (!(battery.flags & SYSTEM_INFO_BATTERY_PRESENT)) {
      continue;
    }
    uint32_t capacity = battery.last_full_capacity;
    if (!capacity || capacity == SYSTEM_INFO_BATTERY_UNKNOWN) {
      capacity = battery.design_capacity;
    }
    if (battery.percent == SYSTEM_INFO_BATTERY_UNKNOWN ||
        battery.remaining_capacity == SYSTEM_INFO_BATTERY_UNKNOWN ||
        !capacity || capacity == SYSTEM_INFO_BATTERY_UNKNOWN ||
        (battery.unit != SYSTEM_INFO_BATTERY_UNIT_MWH &&
         battery.unit != SYSTEM_INFO_BATTERY_UNIT_MAH) ||
        (unit != SYSTEM_INFO_BATTERY_UNKNOWN && unit != battery.unit) ||
        remaining > UINT64_MAX - battery.remaining_capacity || full > UINT64_MAX - capacity) {
      return unreadable(output);
    }
    unit = battery.unit;
    remaining += battery.remaining_capacity;
    full += capacity;
  }
  if (!full) {
    return true;
  }
  if (remaining > UINT64_MAX / 100) {
    return unreadable(output);
  }
  uint64_t percent = remaining * 100 / full;
  if (percent < INSTALL_MIN_BATTERY_PERCENT) {
    fprintf(output, "installer: battery is %llu%%; at least %u%% or AC is required. Nothing written.\n",
        (unsigned long long)percent, INSTALL_MIN_BATTERY_PERCENT);
    return false;
  }
  return true;
}
