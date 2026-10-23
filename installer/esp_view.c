#include "esp_view.h"

#include <clock.h>
#include <handle.h>
#include <string.h>

#define RETIRE_POLL_NS UINT64_C(10000000)
#define CONTROL_RIGHTS (MOUNT_RIGHT_OPEN_ROOT | MOUNT_RIGHT_OBSERVE | MOUNT_RIGHT_WRITE | \
    MOUNT_RIGHT_RETIRE | MOUNT_RIGHT_INTERVAL)

static bool nonzero_guid(const uint8_t guid[16])
{
  for (size_t i = 0; i < 16; ++i) {
    if (guid[i]) {
      return true;
    }
  }
  return false;
}

static bool selection_valid(const struct disk_partition_selection *selection)
{
  return selection && selection->id && selection->id <= UINT32_MAX &&
      selection->partition && selection->partition <= UINT32_MAX &&
      selection->device_block_size && selection->device_block_count &&
      selection->device_block_count <= UINT64_MAX / selection->device_block_size &&
      selection->first_block && selection->first_block < selection->device_block_count &&
      selection->block_count &&
      selection->block_count <= selection->device_block_count - selection->first_block &&
      nonzero_guid(selection->disk_guid) && nonzero_guid(selection->partition_guid);
}

static bool identity_matches(const struct mount_state_reply *state,
    const struct disk_partition_selection *selection)
{
  return (state->flags & MOUNT_STATE_IDENTITY_VALID) && state->device == selection->id &&
      state->partition == selection->partition && state->first_block == selection->first_block &&
      state->block_count == selection->block_count &&
      state->device_block_size == selection->device_block_size &&
      state->device_block_count == selection->device_block_count &&
      state->partition_attributes == selection->attributes &&
      !memcmp(state->disk_guid, selection->disk_guid, 16) &&
      !memcmp(state->partition_guid, selection->partition_guid, 16) &&
      !memcmp(state->partition_type, selection->partition_type, 16);
}

static struct disk_partition_selection selection_of(const struct mount_state_reply *state)
{
  struct disk_partition_selection selection = {
    .id = state->device, .partition = state->partition,
    .first_block = state->first_block, .block_count = state->block_count,
    .device_block_size = state->device_block_size, .device_block_count = state->device_block_count,
    .attributes = state->partition_attributes,
  };
  memcpy(selection.disk_guid, state->disk_guid, 16);
  memcpy(selection.partition_guid, state->partition_guid, 16);
  memcpy(selection.partition_type, state->partition_type, 16);
  return selection;
}

static enum call_status native_grant(handle_t handle, uint64_t protocol, uint64_t rights)
{
  struct handle_info info;
  enum call_status status = handle_query(handle, &info);
  if (status != CALL_OK) {
    return status;
  }
  return info.kind == HANDLE_KIND_NATIVE && info.protocol == protocol &&
      !info.transport && info.rights == rights ? CALL_OK : CALL_DENIED;
}

static enum call_status writable_state(const struct mount_state_reply *state, bool checked)
{
  if (!(state->flags & MOUNT_STATE_IDENTITY_VALID) || !state->domain ||
      !(state->flags & MOUNT_STATE_READ_WRITE) ||
      (state->flags & (MOUNT_STATE_RETIRED | MOUNT_STATE_FAILED | MOUNT_STATE_CLEANUP_PENDING))) {
    return CALL_DENIED;
  }
  if (checked && (state->flags & MOUNT_STATE_ALLOCATION_INVALID)) {
    return CALL_IO;
  }
  return CALL_OK;
}

static enum call_status observe_current(struct install_esp_view *view,
    struct mount_state_reply *state, bool checked)
{
  enum call_status status = mount_observe(view->control, state);
  if (status != CALL_OK) {
    return status;
  }
  struct disk_partition_selection selection = selection_of(&view->expected);
  if (state->domain != view->expected.domain || !identity_matches(state, &selection)) {
    return CALL_NOT_FOUND;
  }
  return writable_state(state, checked);
}

enum call_status install_esp_view_open(struct install_esp_view *view, handle_t disks,
    const struct disk_partition_selection *selection,
    const struct mount_state_reply *expected_ro, uint64_t root_rights)
{
  static const uint8_t esp_type[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
  };
  if (!view || !selection_valid(selection) || view->attempted || view->root != HANDLE_INVALID ||
      view->control != HANDLE_INVALID || view->interval_id ||
      (root_rights & ~INSTALL_ESP_VIEW_RIGHTS)) {
    return CALL_BAD_REQUEST;
  }
  if (memcmp(selection->partition_type, esp_type, 16)) {
    return CALL_DENIED;
  }
  if (expected_ro && !identity_matches(expected_ro, selection)) {
    return CALL_NOT_FOUND;
  }
  view->attempted = true;
  struct mount_fat32_reply mounted;
#if INSTALLER_PUBLICATION_SCRATCH
  enum call_status status = disks_open_owned_esp(disks, selection,
      MOUNT_ACCESS_READ_WRITE, root_rights, &mounted);
#else
  enum call_status status = disks_open_fat32(disks, selection->id, selection->partition,
      MOUNT_ACCESS_READ_WRITE, root_rights, selection->disk_guid, &mounted);
#endif
  if (status == CALL_OK) {
    view->root = mounted.root;
    view->control = mounted.control;
    status = native_grant(view->root, PROTOCOL_DIRECTORY, root_rights);
  }
  if (status == CALL_OK) {
    status = native_grant(view->control, PROTOCOL_MOUNT, CONTROL_RIGHTS);
  }
  struct mount_state_reply observed;
  if (status == CALL_OK) {
    status = mount_observe(view->control, &observed);
    if (status == CALL_OK) {
      view->expected = observed;
      status = identity_matches(&observed, selection) ? writable_state(&observed, false) :
          CALL_NOT_FOUND;
    }
  }
  if (status == CALL_OK && (observed.flags & MOUNT_STATE_INTERVAL)) {
    status = CALL_BUSY;
  }
  view->admitted = status == CALL_OK;
  view->failure_status = status;
  return status;
}

enum call_status install_esp_view_admit_boot(struct install_esp_view *view, handle_t issuer,
    const struct mount_boot_reply *boot, uint64_t root_rights)
{
  if (!view || !boot || view->attempted || view->root != HANDLE_INVALID ||
      view->control != HANDLE_INVALID || view->interval_id ||
      (root_rights & ~INSTALL_ESP_VIEW_RIGHTS)) {
    return CALL_BAD_REQUEST;
  }
  view->attempted = true;
  struct mount_fat32_reply mounted;
  enum call_status status = mount_boot_admit(issuer, boot->disk_guid, boot->partition_guid,
      root_rights, &mounted);
  if (status == CALL_OK) {
    view->root = mounted.root;
    view->control = mounted.control;
    status = native_grant(view->root, PROTOCOL_DIRECTORY, root_rights);
  }
  if (status == CALL_OK) {
    status = native_grant(view->control, PROTOCOL_MOUNT, CONTROL_RIGHTS);
  }
  struct mount_state_reply observed;
  if (status == CALL_OK) {
    status = mount_observe(view->control, &observed);
    if (status == CALL_OK) {
      view->expected = observed;
      struct disk_partition_selection selection = selection_of(&observed);
      status = (observed.flags & MOUNT_STATE_IDENTITY_VALID) && selection_valid(&selection) &&
          !memcmp(observed.disk_guid, boot->disk_guid, 16) &&
          !memcmp(observed.partition_guid, boot->partition_guid, 16) ?
          writable_state(&observed, false) : CALL_NOT_FOUND;
    }
  }
  if (status == CALL_OK && (observed.flags & MOUNT_STATE_INTERVAL)) {
    status = CALL_BUSY;
  }
  view->admitted = status == CALL_OK;
  view->failure_status = status;
  return status;
}

enum call_status install_esp_view_begin_checked(struct install_esp_view *view)
{
  if (!view || !view->admitted || view->retired || view->control == HANDLE_INVALID ||
      view->root == HANDLE_INVALID || view->interval_id || view->cleanup_status != CALL_OK) {
    return CALL_BAD_REQUEST;
  }
  if (view->failure_status != CALL_OK) {
    return view->failure_status;
  }
  struct mount_state_reply state;
  enum call_status status = observe_current(view, &state, false);
  if (status == CALL_OK && (state.flags & MOUNT_STATE_INTERVAL)) {
    status = CALL_BUSY;
  }
  struct mount_interval_reply interval = {0};
  if (status == CALL_OK) {
    status = mount_begin(view->control, &interval);
    if (status == CALL_OK) {
      view->interval_id = interval.id;
      view->interval_deadline_ns = interval.deadline_ns;
      status = mount_check(view->control);
    }
  }
  if (status == CALL_OK) {
    status = observe_current(view, &state, true);
    if (status == CALL_OK && (!(state.flags & MOUNT_STATE_INTERVAL) ||
        state.interval_id != interval.id || state.interval_deadline_ns != interval.deadline_ns)) {
      status = CALL_TIMED_OUT;
    }
  }
  view->failure_status = status;
  return status;
}

enum call_status install_esp_view_end(struct install_esp_view *view)
{
  if (!view) {
    return CALL_BAD_REQUEST;
  }
  if (view->cleanup_status != CALL_OK) {
    return view->cleanup_status;
  }
  if (!view->interval_id) {
    return CALL_OK;
  }
  enum call_status status = mount_end(view->control, view->interval_id);
  if (status == CALL_OK) {
    view->interval_id = 0;
    view->interval_deadline_ns = 0;
  } else {
    view->cleanup_status = status;
  }
  return status;
}

enum call_status install_esp_view_end_checked(struct install_esp_view *view, handle_t clock)
{
  if (!view || !view->interval_id) {
    return CALL_BAD_REQUEST;
  }
  uint64_t deadline = view->interval_deadline_ns;
  struct mount_state_reply state;
  enum call_status status = observe_current(view, &state, true);
  if (status == CALL_OK && (!(state.flags & MOUNT_STATE_INTERVAL) ||
      state.interval_id != view->interval_id || state.interval_deadline_ns != deadline)) {
    status = CALL_TIMED_OUT;
  }
  if (status == CALL_OK) {
    status = install_esp_view_end(view);
  }
  if (status == CALL_OK) {
    uint64_t now;
    status = clock_now(clock, &now);
    if (status == CALL_OK && now >= deadline) {
      status = CALL_TIMED_OUT;
    }
  }
  if (status != CALL_OK) {
    view->failure_status = status;
  }
  return status;
}

enum call_status install_esp_view_close_and_retire(struct install_esp_view *view,
    handle_t clock, uint64_t deadline_ns)
{
  if (!view) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = install_esp_view_end(view);
  if (status != CALL_OK) {
    return status;
  }
  if (view->root != HANDLE_INVALID) {
    if (handle_close(view->root) != 0) {
      view->cleanup_status = CALL_OUTCOME_UNKNOWN;
      return view->cleanup_status;
    }
    view->root = HANDLE_INVALID;
  }
  if (view->control == HANDLE_INVALID) {
    return view->failure_status == CALL_OUTCOME_UNKNOWN ? CALL_OUTCOME_UNKNOWN : CALL_OK;
  }
  while (!view->retired) {
    uint64_t now;
    status = clock_now(clock, &now);
    if (status != CALL_OK) {
      break;
    }
    if (now >= deadline_ns) {
      return CALL_TIMED_OUT;
    }
    status = mount_retire(view->control);
    if (status == CALL_OK) {
      view->retired = true;
      break;
    }
    if (status != CALL_BUSY) {
      break;
    }
    status = clock_now(clock, &now);
    if (status != CALL_OK) {
      break;
    }
    if (now >= deadline_ns) {
      return CALL_TIMED_OUT;
    }
    uint64_t delay = deadline_ns - now;
    if (delay > RETIRE_POLL_NS) {
      delay = RETIRE_POLL_NS;
    }
    status = clock_sleep_until(clock, now + delay);
    if (status != CALL_OK) {
      break;
    }
  }
  if (!view->retired) {
    view->cleanup_status = status;
    return status;
  }
  if (handle_close(view->control) != 0) {
    view->cleanup_status = CALL_OUTCOME_UNKNOWN;
    return view->cleanup_status;
  }
  view->control = HANDLE_INVALID;
  view->admitted = false;
  return CALL_OK;
}
