#include "job_internal.h"
#include <directory.h>
#include <handle.h>
#include <abi/audio.h>
#include <abi/clipboard.h>
#include <abi/clock.h>
#include <abi/console.h>
#include <abi/directory.h>
#include <abi/disk.h>
#include <abi/display.h>
#include <abi/echo.h>
#include <abi/endpoint.h>
#include <abi/execution_group.h>
#include <abi/file.h>
#include <abi/forwarder.h>
#include <abi/keyboard.h>
#include <abi/gamepad.h>
#include <abi/log.h>
#include <abi/memory.h>
#include <abi/jit.h>
#include <abi/mount.h>
#include <abi/net_config.h>
#include <abi/pipe.h>
#include <abi/pointer.h>
#include <abi/process.h>
#include <abi/profile.h>
#include <abi/provider.h>
#include <abi/random.h>
#include <abi/screen_capture.h>
#include <abi/space.h>
#include <abi/system_info.h>
#include <abi/tcp.h>
#include <abi/thread.h>
#include <abi/udp.h>

/* These are the current native-command first profile's explicit resource
 * scopes. New protocols require review rather than inheriting guessed rights.
 * Exported interfaces use the same masks; provider behavior and namespace
 * bindings remain the trusted caller's responsibility for the job's lifetime. */
static enum call_status validate_authority(const struct handle_info *info,
    uint64_t rights, uint64_t transport)
{
  uint64_t allowed_rights;
  switch (info->protocol) {
  case PROTOCOL_CONSOLE:
    allowed_rights = CONSOLE_RIGHTS | CONSOLE_RIGHT_INTERRUPT |
        CONSOLE_RIGHT_ARMED | CONSOLE_RIGHT_PASSTHROUGH;
    break;
  case PROTOCOL_FILE:
    allowed_rights = FILE_RIGHTS;
    break;
  case PROTOCOL_FORWARDER:
    allowed_rights = FORWARDER_RIGHTS;
    break;
  case PROTOCOL_DIRECTORY:
    allowed_rights = DIRECTORY_RIGHTS;
    break;
  case PROTOCOL_MEMORY:
    allowed_rights = MEMORY_RIGHT_MANAGE;
    break;
  case PROTOCOL_JIT:
    if (info->kind != HANDLE_KIND_NATIVE || info->transport) {
      return CALL_WRONG_TYPE;
    }
    allowed_rights = JIT_RIGHT_MANAGE;
    break;
  case PROTOCOL_PROCESS:
    allowed_rights = PROCESS_RIGHT_WAIT;
    break;
  case PROTOCOL_EXECUTION_GROUP:
    allowed_rights = EXECUTION_GROUP_RIGHT_WAIT;
    break;
  case PROTOCOL_THREAD_SERVICE:
    allowed_rights = THREAD_RIGHT_MANAGE;
    break;
  case PROTOCOL_THREAD_COMPLETION:
    allowed_rights = THREAD_COMPLETION_RIGHT_WAIT;
    break;
  case PROTOCOL_DISPLAY:
    allowed_rights = DISPLAY_RIGHT_DRAW;
    break;
  case PROTOCOL_CLOCK:
    allowed_rights = CLOCK_RIGHTS;
    break;
  case PROTOCOL_KEYBOARD:
    allowed_rights = KEYBOARD_RIGHT_INPUT;
    break;
  case PROTOCOL_GAMEPAD:
    allowed_rights = GAMEPAD_RIGHT_INPUT;
    break;
  case PROTOCOL_POINTER:
    allowed_rights = POINTER_RIGHT_INPUT;
    break;
  case PROTOCOL_AUDIO:
    allowed_rights = AUDIO_RIGHT_PLAYBACK;
    break;
  case PROTOCOL_SCREEN_CAPTURE:
    allowed_rights = SCREEN_CAPTURE_RIGHT_CAPTURE;
    break;
  case PROTOCOL_CLIPBOARD:
    allowed_rights = CLIPBOARD_RIGHTS;
    break;
  case PROTOCOL_MOUNT:
    /* Configured content authority, unlike raw storage mutation/acquisition. */
    allowed_rights = MOUNT_RIGHT_OPEN_ROOT | MOUNT_RIGHT_OBSERVE | MOUNT_RIGHT_WRITE;
    break;
  case PROTOCOL_DISKS:
    allowed_rights = DISKS_RIGHT_ENUMERATE;
    break;
  case PROTOCOL_DISK:
    allowed_rights = DISK_RIGHT_INFO | DISK_RIGHT_READ;
    break;
  case PROTOCOL_ECHO:
    allowed_rights = ECHO_RIGHT_SEND;
    break;
  case PROTOCOL_NET_REGISTRY:
    allowed_rights = NET_REGISTRY_RIGHT_READ;
    break;
  case PROTOCOL_UDP_SERVICE:
    allowed_rights = UDP_SERVICE_RIGHT_OPEN | UDP_SERVICE_RIGHT_BROADCAST;
    break;
  case PROTOCOL_UDP:
    allowed_rights = UDP_RIGHTS;
    break;
  case PROTOCOL_TCP_SERVICE:
    allowed_rights = TCP_SERVICE_RIGHTS;
    break;
  case PROTOCOL_TCP:
    allowed_rights = TCP_RIGHTS;
    break;
  case PROTOCOL_TCP_LISTENER:
    allowed_rights = TCP_LISTENER_RIGHTS;
    break;
  case PROTOCOL_RANDOM:
    allowed_rights = RANDOM_RIGHT_READ;
    break;
  case PROTOCOL_SPACE:
    allowed_rights = SPACE_RIGHT_SET_TITLE;
    break;
  case PROTOCOL_PROFILE:
    allowed_rights = PROFILE_RIGHT_MEMORY | PROFILE_RIGHT_HOST;
    break;
  case PROTOCOL_PIPE_SERVICE:
    allowed_rights = PIPE_SERVICE_RIGHT_CREATE;
    break;
  case PROTOCOL_PIPE:
    allowed_rights = PIPE_RIGHT_READ | PIPE_RIGHT_WRITE;
    break;
  case PROTOCOL_ENDPOINT_SERVICE:
    allowed_rights = ENDPOINT_SERVICE_RIGHT_CREATE;
    break;
  case PROTOCOL_ENDPOINT:
    allowed_rights = 0;
    break;
  case PROTOCOL_PROVIDER:
    allowed_rights = PROVIDER_RIGHTS;
    break;
  case PROTOCOL_SYSTEM_INFO:
    allowed_rights = SYSTEM_INFO_RIGHT_READ;
    break;
  case PROTOCOL_LOG:
    allowed_rights = LOG_RIGHT_READ;
    break;
  case PROTOCOL_LAUNCHER:
  case PROTOCOL_SPACE_FACTORY:
  case PROTOCOL_POWER:
  case PROTOCOL_INPUT_POLICY:
  case PROTOCOL_BLUETOOTH_HCI:
  case PROTOCOL_TERMINAL_SERVICE:
  case PROTOCOL_TERMINAL_ATTACHMENT:
  case PROTOCOL_TERMINAL_EVENTS:
  case PROTOCOL_TERMINAL_POINTER:
  case PROTOCOL_ENDPOINT_RECEIVER:
  case PROTOCOL_ENDPOINT_RECEIPT:
  default:
    return CALL_DENIED;
  }

  uint64_t allowed_transport = info->kind == HANDLE_KIND_EXPORTED ||
      info->protocol == PROTOCOL_ENDPOINT ? HANDLE_TRANSPORT_CALL : 0;
  if ((rights & ~allowed_rights) || (transport & ~allowed_transport)) {
    return CALL_DENIED;
  }
  return CALL_OK;
}

enum call_status libc_job_validate_grants(const struct launch_grant *grants, size_t count)
{
  if (count && !grants) {
    return CALL_BAD_REQUEST;
  }
  for (size_t i = 0; i < count; ++i) {
    struct handle_info info;
    enum call_status status = handle_query(grants[i].source, &info);
    if (status != CALL_OK) {
      return status;
    }
    if ((grants[i].rights & ~info.rights) || (grants[i].transport & ~info.transport)) {
      return CALL_DENIED;
    }
    status = validate_authority(&info, grants[i].rights, grants[i].transport);
    if (status != CALL_OK) {
      return status;
    }
  }
  return CALL_OK;
}

static bool capture_protocol_denied(uint64_t protocol)
{
  switch (protocol) {
  case PROTOCOL_CONSOLE:
  case PROTOCOL_KEYBOARD:
  case PROTOCOL_GAMEPAD:
  case PROTOCOL_POINTER:
  case PROTOCOL_DISPLAY:
  case PROTOCOL_SCREEN_CAPTURE:
  case PROTOCOL_CLIPBOARD:
  case PROTOCOL_AUDIO:
  case PROTOCOL_SPACE:
  case PROTOCOL_LOG:
  case PROTOCOL_MOUNT:
  case PROTOCOL_DISKS:
  case PROTOCOL_DISK:
  case PROTOCOL_RETENTION_FACTORY:
  case PROTOCOL_RETENTION_ACCOUNT:
    return true;
  default:
    return false;
  }
}

enum call_status libc_job_validate_capture_grants(const struct launch_grant *grants,
    size_t count, handle_t private_root)
{
  enum call_status status = libc_job_validate_grants(grants, count);
  if (status != CALL_OK) {
    return status;
  }
  struct handle_info root_info;
  status = handle_query(private_root, &root_info);
  if (status != CALL_OK) {
    return status;
  }
  if ((root_info.protocol != PROTOCOL_DIRECTORY && root_info.protocol != PROTOCOL_RETENTION_ACCOUNT) ||
      root_info.kind != HANDLE_KIND_NATIVE ||
      root_info.transport) {
    return CALL_WRONG_TYPE;
  }
  struct file_info_reply root_identity = {0};
  if (root_info.protocol == PROTOCOL_DIRECTORY) {
    status = directory_info(private_root, &root_identity);
    if (status != CALL_OK) {
      return status;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (grants[i].source == private_root) {
      return CALL_DENIED;
    }
    struct handle_info info;
    status = handle_query(grants[i].source, &info);
    if (status != CALL_OK) {
      return status;
    }
    if (capture_protocol_denied(info.protocol)) {
      return CALL_DENIED;
    }
    if (root_info.protocol == PROTOCOL_DIRECTORY && info.protocol == PROTOCOL_DIRECTORY) {
      struct file_info_reply identity;
      status = directory_info(grants[i].source, &identity);
      if (status != CALL_OK) {
        return status;
      }
      uint64_t identity_bits = FILE_INFO_DOMAIN_VALID | FILE_INFO_OBJECT_VALID;
      if ((root_identity.valid & identity_bits) == identity_bits &&
          (identity.valid & identity_bits) == identity_bits &&
          root_identity.domain == identity.domain && root_identity.object == identity.object) {
        return CALL_DENIED;
      }
    }
  }
  /* Identity can reject known aliases, but cannot establish private RAM backing,
   * exclusive access, subtree isolation or trusted provider/credential behavior. */
  return CALL_OK;
}
