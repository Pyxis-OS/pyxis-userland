#include <wifi.h>
#include <syscall.h>
#include <stdbool.h>
#include <string.h>

static enum call_status invoke(handle_t handle, const void *request, size_t bytes,
    void *reply, size_t capacity)
{
  struct syscall_result result = syscall_call(handle, request, bytes, reply, capacity);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? capacity : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

static bool boot_facts_valid(const struct wifi_boot_facts *boot)
{
  if ((boot->valid & ~WIFI_BOOT_VALID_MASK) ||
      (boot->state & ~WIFI_BOOT_STATE_MASK) || boot->secure_boot > 1) {
    return false;
  }
  if (boot->valid & WIFI_BOOT_RESET_VALID) {
    if (boot->reset_method != WIFI_RESET_FLR && boot->reset_method != WIFI_RESET_POWER) {
      return false;
    }
  } else if (boot->reset_method != WIFI_RESET_NONE ||
      boot->pci_device_capabilities || boot->pci_power_control) {
    return false;
  }
  if (!(boot->valid & WIFI_BOOT_CUT_VALID) && boot->live_cut) {
    return false;
  }
  if (!(boot->valid & WIFI_BOOT_SECURITY_VALID) && boot->secure_boot) {
    return false;
  }
  if (!(boot->valid & WIFI_BOOT_IMAGE_VALID)) {
    for (size_t i = 0; i < sizeof(boot->firmware_version); ++i) {
      if (boot->firmware_version[i]) {
        return false;
      }
    }
  }
  if (!(boot->valid & WIFI_BOOT_BOARD_VALID) &&
      (boot->rfe || boot->xtal || boot->tx_nss || boot->rx_nss ||
       boot->tx_antennas || boot->rx_antennas ||
       boot->board_country[0] || boot->board_country[1])) {
    return false;
  }
  return true;
}

static bool tas_diagnostic_valid(const struct wifi_tas_diagnostic *tas)
{
  return tas->stage <= WIFI_TAS_STAGE_ALIAS && tas->reason <= WIFI_TAS_REASON_ALIAS &&
      tas->object_type <= WIFI_TAS_OBJECT_OTHER && !(tas->flags & ~WIFI_TAS_FLAGS) &&
      ((tas->flags & WIFI_TAS_OBJECT_VALID) || !tas->object_type) &&
      ((tas->flags & WIFI_TAS_LENGTH_VALID) || !tas->length) &&
      ((tas->flags & WIFI_TAS_SUPPORT_VALID) || !tas->supported);
}

static bool sar_source_valid(const struct wifi_sar_source *source, bool dynamic)
{
  if (source->cid != WIFI_SAR_CID_HP && source->cid != WIFI_SAR_CID_RT) return false;
  if (!((source->revision == 1 && source->subbands == 5) ||
      (source->revision == 2 && source->subbands == 11))) return false;
  if (source->antennas != 4 && !(source->cid == WIFI_SAR_CID_RT && source->antennas == 2)) {
    return false;
  }
  return source->profiles && source->profiles <= (dynamic ? WIFI_SAR_PROFILES : 1u);
}

static bool sar_inputs_valid(const struct wifi_sar_inputs *sar)
{
  if (sar->state > WIFI_INPUT_FAILED || (sar->sources & ~WIFI_SAR_SOURCES)) {
    return false;
  }
  if (sar->state != WIFI_INPUT_VALID) {
    const struct wifi_sar_inputs empty = {.state = sar->state};
    return !memcmp(sar, &empty, sizeof(empty));
  }
  const struct wifi_sar_source empty = {0};
  return sar->sources &&
      ((sar->sources & WIFI_SAR_SOURCE_WRDS) ? sar_source_valid(&sar->wrds, false) :
        !memcmp(&sar->wrds, &empty, sizeof(empty))) &&
      ((sar->sources & WIFI_SAR_SOURCE_RWRD) ? sar_source_valid(&sar->rwrd, true) :
        !memcmp(&sar->rwrd, &empty, sizeof(empty)));
}

static bool radio_inputs_valid(const struct wifi_radio_inputs *radio)
{
  if (radio->kind == WIFI_RADIO_INPUTS_NONE) {
    const struct wifi_radio_inputs empty = {0};
    return !memcmp(radio, &empty, sizeof(empty));
  }
  if (radio->kind != WIFI_RADIO_INPUTS_RTL8922AE || radio->reserved ||
      (radio->tables_flags & ~WIFI_TABLE_POWER_DEFAULT) ||
      (radio->rule_channels & ~WIFI_SCAN_CHANNELS) ||
      radio->tables_state > WIFI_INPUT_FAILED || radio->acpi_state > WIFI_INPUT_FAILED ||
      radio->static_sar_state > WIFI_INPUT_FAILED ||
      radio->dynamic_sar_state > WIFI_INPUT_FAILED ||
      radio->sar_selector_state > WIFI_INPUT_FAILED ||
      radio->geographic_sar_state > WIFI_INPUT_FAILED ||
      radio->antenna_gain_state > WIFI_INPUT_FAILED || radio->tas_state > WIFI_INPUT_FAILED ||
      !tas_diagnostic_valid(&radio->tas) || !sar_inputs_valid(&radio->sar)) {
    return false;
  }
  for (size_t i = 0; i < WIFI_CHANNEL_24_COUNT; ++i) {
    const struct wifi_channel_rule *rule = &radio->channel[i];
    if ((rule->flags & ~WIFI_RULE_FLAGS) ||
        (!(radio->rule_channels & (UINT32_C(1) << i)) &&
         (rule->flags || rule->max_eirp_mbm))) {
      return false;
    }
  }
  return true;
}

static bool phy_facts_valid(const struct wifi_phy_facts *phy, uint32_t radio_kind)
{
  if (radio_kind == WIFI_RADIO_INPUTS_NONE) {
    const struct wifi_phy_facts empty = {0};
    return !memcmp(phy, &empty, sizeof(empty));
  }
  if (phy->reserved || (phy->state & ~WIFI_PHY_STATE_MASK) ||
      phy->phase > WIFI_PHY_COMPLETE || phy->channel > 13 || phy->region > 15 ||
      (phy->rfk_completed & ~WIFI_PHY_RFK_MASK) || phy->rfk_state > 4 ||
      phy->rfk_version > UINT8_MAX || (phy->thermal_default_paths & ~UINT32_C(3)) ||
      phy->last_status >= CALL_STATUS_COUNT ||
      ((phy->state & WIFI_PHY_STOP_CONFIRMED) && (phy->state & WIFI_PHY_STOP_UNCERTAIN))) {
    return false;
  }
  if (!(phy->state & WIFI_PHY_RFK_REPORT_VALID) &&
      (phy->rfk_state || phy->rfk_version || phy->rfk_completed)) {
    return false;
  }
  if (phy->phase != WIFI_PHY_COMPLETE && phy->last_status) {
    return false;
  }
  if (phy->state & WIFI_PHY_POWER_VERIFIED) {
    if (!phy->power_verifications || !phy->channel) {
      return false;
    }
    /* Signed nine-bit RF fields, independently of narrower preparation input
     * conversions or the observed board's particular SAR profile. */
    for (size_t path = 0; path < 2; ++path) {
      if (phy->sar_quarter_dbm[path] < -256 || phy->sar_quarter_dbm[path] > 255) {
        return false;
      }
    }
  } else if (phy->power_verifications || phy->sar_quarter_dbm[0] || phy->sar_quarter_dbm[1]) {
    return false;
  }
  /* Checks count attempts, including failed reads. A failed read clears the
   * validity bit without erasing the historical attempt count. */
  if ((phy->state & WIFI_PHY_RFKILL_VALID) && !phy->rfkill_checks) {
    return false;
  }
  return true;
}

static bool status_reply_valid(const struct wifi_status_reply *reply)
{
  if (!memchr(reply->detail, 0, sizeof(reply->detail)) || !boot_facts_valid(&reply->boot) ||
      !radio_inputs_valid(&reply->radio)) {
    return false;
  }
  if (reply->radio.kind == WIFI_RADIO_INPUTS_RTL8922AE &&
      !((reply->country[0] == '0' && reply->country[1] == '0') ||
        (reply->country[0] >= 'A' && reply->country[0] <= 'Z' &&
         reply->country[1] >= 'A' && reply->country[1] <= 'Z'))) {
    return false;
  }
  return true;
}

enum call_status wifi_status(handle_t wifi, struct wifi_status_reply *status)
{
  if (!status) {
    return CALL_BAD_REQUEST;
  }
  *status = (struct wifi_status_reply){0};
  struct message_header request = {PROTOCOL_WIFI, WIFI_STATUS};
  struct wifi_status_reply reply;
  enum call_status result = invoke(wifi, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (!status_reply_valid(&reply)) {
      return CALL_BAD_REQUEST;
    }
    *status = reply;
  }
  return result;
}

enum call_status wifi_status_full(handle_t wifi, struct wifi_status_full_reply *status)
{
  if (!status) {
    return CALL_BAD_REQUEST;
  }
  *status = (struct wifi_status_full_reply){0};
  struct message_header request = {PROTOCOL_WIFI, WIFI_STATUS};
  struct wifi_status_full_reply reply;
  enum call_status result = invoke(wifi, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (!status_reply_valid(&reply.base) || !phy_facts_valid(&reply.phy, reply.base.radio.kind)) {
      return CALL_BAD_REQUEST;
    }
    *status = reply;
  }
  return result;
}

enum call_status wifi_scan_start(handle_t wifi, uint32_t channels,
    uint64_t deadline_ns, handle_t *operation)
{
  if (!operation) {
    return CALL_BAD_REQUEST;
  }
  *operation = HANDLE_INVALID;
  struct wifi_scan_request request = {
    .header = {PROTOCOL_WIFI, WIFI_SCAN_START},
    .channels = channels, .deadline_ns = deadline_ns,
  };
  struct wifi_scan_start_reply reply;
  enum call_status result = invoke(wifi, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (reply.operation == HANDLE_INVALID) {
      return CALL_BAD_REQUEST;
    }
    *operation = reply.operation;
  }
  return result;
}

enum call_status wifi_scan_status(handle_t operation,
    struct wifi_scan_status_reply *status)
{
  if (!status) {
    return CALL_BAD_REQUEST;
  }
  *status = (struct wifi_scan_status_reply){0};
  struct message_header request = {PROTOCOL_WIFI_SCAN, WIFI_SCAN_STATUS};
  struct wifi_scan_status_reply reply;
  enum call_status result = invoke(operation, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (reply.status >= CALL_STATUS_COUNT || reply.record_count > WIFI_SCAN_RECORDS || reply.reserved ||
        (reply.flags & ~(WIFI_SCAN_COMPLETE | WIFI_SCAN_TAKEN | WIFI_SCAN_CANCEL_REQUESTED)) ||
        (!(reply.flags & WIFI_SCAN_COMPLETE) && reply.status != CALL_WOULD_BLOCK)) {
      return CALL_BAD_REQUEST;
    }
    *status = reply;
  }
  return result;
}

enum call_status wifi_scan_take(handle_t operation, handle_t *file)
{
  if (!file) {
    return CALL_BAD_REQUEST;
  }
  *file = HANDLE_INVALID;
  struct message_header request = {PROTOCOL_WIFI_SCAN, WIFI_SCAN_TAKE};
  struct wifi_scan_take_reply reply;
  enum call_status result = invoke(operation, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (reply.file == HANDLE_INVALID) {
      return CALL_BAD_REQUEST;
    }
    *file = reply.file;
  }
  return result;
}

enum call_status wifi_scan_cancel(handle_t operation)
{
  struct message_header request = {PROTOCOL_WIFI_SCAN, WIFI_SCAN_CANCEL};
  return invoke(operation, &request, sizeof(request), NULL, 0);
}

enum call_status wifi_install_policy(handle_t wifi, const char country[2],
    uint32_t channels)
{
  if (!country) {
    return CALL_BAD_REQUEST;
  }
  struct wifi_policy_request request = {
    .header = {PROTOCOL_WIFI, WIFI_INSTALL_POLICY}, .channels = channels,
    .country = {country[0], country[1]},
  };
  return invoke(wifi, &request, sizeof(request), NULL, 0);
}

static enum call_status returned_handle(handle_t target, const void *request, size_t bytes, handle_t *handle)
{
  if (!handle) {
    return CALL_BAD_REQUEST;
  }
  *handle = HANDLE_INVALID;
  handle_t returned = HANDLE_INVALID;
  enum call_status status = invoke(target, request, bytes, &returned, sizeof(returned));
  if (status == CALL_OK) {
    if (!returned) {
      return CALL_BAD_REQUEST;
    }
    *handle = returned;
  }
  return status;
}

enum call_status wifi_credential_seal(handle_t wifi, uint32_t format,
    const void *bytes, size_t length, handle_t *file)
{
  if (!file) {
    return CALL_BAD_REQUEST;
  }
  *file = HANDLE_INVALID;
  if (!bytes || length > WIFI_CREDENTIAL_BYTES) {
    return CALL_BAD_REQUEST;
  }
  struct wifi_credential_request request = {
    .header = {PROTOCOL_WIFI, WIFI_CREDENTIAL_SEAL},
    .credential = {.format = format, .length = length},
  };
  memcpy(request.credential.bytes, bytes, length);
  enum call_status status = returned_handle(wifi, &request, sizeof(request), file);
  return status;
}

enum call_status wifi_connect_start(handle_t radio, const struct wifi_target *target, handle_t credential,
    uint64_t deadline_ns, handle_t *operation)
{
  if (!target) {
    return CALL_BAD_REQUEST;
  }
  struct wifi_connect_request request = {
    .header = {PROTOCOL_WIFI, WIFI_CONNECT_START}, .deadline_ns = deadline_ns, .target = *target,
    .credential = credential,
  };
  return returned_handle(radio, &request, sizeof(request), operation);
}

enum call_status wifi_connect_status(handle_t operation, struct wifi_connect_status_reply *status)
{
  if (!status) {
    return CALL_BAD_REQUEST;
  }
  *status = (struct wifi_connect_status_reply){0};
  struct message_header request = {PROTOCOL_WIFI_CONNECT, WIFI_CONNECT_STATUS};
  struct wifi_connect_status_reply reply = {0};
  enum call_status result = invoke(operation, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (reply.status >= CALL_STATUS_COUNT || reply.reserved[0] || reply.reserved[1] ||
        (reply.flags & ~(WIFI_SCAN_COMPLETE | WIFI_SCAN_TAKEN | WIFI_SCAN_CANCEL_REQUESTED))) {
      return CALL_BAD_REQUEST;
    }
    *status = reply;
  }
  return result;
}

enum call_status wifi_connect_take(handle_t operation, handle_t *link)
{
  struct message_header request = {PROTOCOL_WIFI_CONNECT, WIFI_CONNECT_TAKE};
  return returned_handle(operation, &request, sizeof(request), link);
}

enum call_status wifi_connect_cancel(handle_t operation)
{
  struct message_header request = {PROTOCOL_WIFI_CONNECT, WIFI_CONNECT_CANCEL};
  return invoke(operation, &request, sizeof(request), NULL, 0);
}

enum call_status wifi_link_status(handle_t link, struct wifi_link_status_reply *status)
{
  if (!status) {
    return CALL_BAD_REQUEST;
  }
  *status = (struct wifi_link_status_reply){0};
  struct message_header request = {PROTOCOL_WIFI_LINK, WIFI_LINK_STATUS};
  struct wifi_link_status_reply reply = {0};
  enum call_status result = invoke(link, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (!reply.generation || reply.reserved || reply.status >= CALL_STATUS_COUNT ||
        (reply.flags & ~(WIFI_LINK_ATTEMPTING | WIFI_LINK_ASSOCIATED | WIFI_LINK_OPEN |
            WIFI_LINK_DATA_READY | WIFI_LINK_TERMINAL | WIFI_LINK_CLOSE_REQUESTED | WIFI_LINK_AUTHENTICATED))) {
      return CALL_BAD_REQUEST;
    }
    *status = reply;
  }
  return result;
}

enum call_status wifi_link_disconnect(handle_t link)
{
  struct message_header request = {PROTOCOL_WIFI_LINK, WIFI_LINK_DISCONNECT};
  return invoke(link, &request, sizeof(request), NULL, 0);
}
