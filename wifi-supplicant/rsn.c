#include "rsn.h"

#include <clock.h>
#include <file.h>
#include <handle.h>
#include <wifi_auth.h>
#include <stdlib.h>
#include <string.h>

static enum call_status current_time(struct rsn_context *context, uint64_t *now)
{
  enum call_status status = clock_now(context->clock, now);
  if (status != CALL_OK) {
    rsn_fail(context, status);
  }
  return status;
}

void rsn_fail(struct rsn_context *context, enum call_status status)
{
  if (!context || !context->generation) {
    return;
  }
  if (context->failure == CALL_OK) {
    context->failure = status == CALL_OK ? CALL_IO : status;
  }
  if (!context->failure_sent) {
    enum call_status stopped = wifi_auth_fail(context->auth, context->generation,
        context->failure);
    if (stopped == CALL_OK || stopped == CALL_UNAVAILABLE) {
      context->failure_sent = true;
    } else {
      /* An unconfirmed FAIL cannot be treated as a retired attempt. Closing
       * the worker's auth grant lets the kernel drain its actual ownership. */
      context->failure = stopped;
    }
  }
  context->authorized = false;
}

static int authorize(struct rsn_context *context, bool enabled)
{
  if (context->failure != CALL_OK) {
    return -1;
  }
  enum call_status status = wifi_auth_authorize(context->auth, context->generation, enabled);
  if (status != CALL_OK) {
    rsn_fail(context, status);
    return -1;
  }
  context->authorized = enabled;
  return 0;
}

static void set_state(void *opaque, enum wpa_states state)
{
  struct rsn_context *context = opaque;
  if (context->failure != CALL_OK) {
    return;
  }
  context->state = state;
  if (state == WPA_COMPLETED) {
    if (!context->timeout_cancelled || !context->pairwise_installed ||
        !context->group_installed) {
      rsn_fail(context, CALL_DENIED);
      return;
    }
    uint64_t now;
    if (current_time(context, &now) != CALL_OK) {
      return;
    }
    if (now >= context->phase_deadline) {
      rsn_fail(context, CALL_TIMED_OUT);
      return;
    }
    if (authorize(context, true) == 0) {
      context->phase_deadline = UINT64_MAX;
    }
  }
}

static enum wpa_states get_state(void *opaque)
{
  return ((struct rsn_context *)opaque)->state;
}

static void deauthenticate(void *opaque, u16 reason)
{
  (void)reason;
  rsn_fail(opaque, CALL_CONNECTION_REFUSED);
}

static void reconnect(void *opaque)
{
  /* Kernel connection policy, not the RSN state machine, owns fresh attempts. */
  rsn_fail(opaque, CALL_CONNECTION_RESET);
}

static void *get_network(void *opaque)
{
  return opaque;
}

static int get_bssid(void *opaque, u8 *bssid)
{
  struct rsn_context *context = opaque;
  if (context->failure != CALL_OK) {
    return -1;
  }
  memcpy(bssid, context->bssid, sizeof(context->bssid));
  return 0;
}

static int get_beacon_ie(void *opaque)
{
  struct rsn_context *context = opaque;
  if (context->failure != CALL_OK || !context->sm) {
    return -1;
  }
  if (wpa_sm_set_ap_rsn_ie(context->sm, context->ap_rsn, context->ap_rsn_size)) {
    rsn_fail(context, CALL_NO_MEMORY);
    return -1;
  }
  return 0;
}

static void cancel_auth_timeout(void *opaque)
{
  /* Completion confirms this cancellation before the worker drops its bound. */
  ((struct rsn_context *)opaque)->timeout_cancelled = true;
}

static u8 *alloc_eapol(void *opaque, u8 type, const void *data, u16 size,
    size_t *message_size, void **data_position)
{
  struct rsn_context *context = opaque;
  if (context->failure != CALL_OK || size > RSN_EAPOL_LIMIT - sizeof(struct ieee802_1x_hdr)) {
    rsn_fail(context, CALL_LIMIT);
    return NULL;
  }
  size_t length = sizeof(struct ieee802_1x_hdr) + size;
  struct ieee802_1x_hdr *message = calloc(1, length);
  if (!message) {
    rsn_fail(context, CALL_NO_MEMORY);
    return NULL;
  }
  message->version = EAPOL_VERSION;
  message->type = type;
  message->length = host_to_be16(size);
  if (data && size) {
    memcpy(message + 1, data, size);
  }
  *message_size = length;
  *data_position = message + 1;
  return (u8 *)message;
}

static int ether_send(void *opaque, const u8 *destination, u16 protocol,
    const u8 *bytes, size_t size)
{
  struct rsn_context *context = opaque;
  if (context->failure != CALL_OK) {
    return -1;
  }
  if (!destination || memcmp(destination, context->bssid, sizeof(context->bssid)) ||
      protocol != ETH_P_EAPOL || !bytes || size > RSN_EAPOL_LIMIT) {
    rsn_fail(context, CALL_DENIED);
    return -1;
  }
  uint32_t flags = context->pairwise_installed ? WIFI_AUTH_EAPOL_ENCRYPT : 0;
  enum call_status status = wifi_auth_send(context->auth, context->generation, bytes, size, flags);
  if (status != CALL_OK) {
    rsn_fail(context, status);
    return -1;
  }
  return 0;
}

static int set_key(void *opaque, int link_id, enum wpa_alg algorithm,
    const u8 *address, int key_id, int set_tx, const u8 *sequence, size_t sequence_size,
    const u8 *key, size_t key_size, enum key_flag flags)
{
  struct rsn_context *context = opaque;
  if (context->failure != CALL_OK) {
    return -1;
  }
  /* Upstream explicitly permits refusal of this RX-only optimization. The
   * mandatory RX/TX install follows; no firmware mutation happened here. */
  if (flags == KEY_FLAG_PAIRWISE_NEXT) {
    return -1;
  }
  bool group = flags == KEY_FLAG_GROUP_RX;
  bool pairwise = flags == KEY_FLAG_PAIRWISE_RX_TX;
  bool removing = algorithm == WPA_ALG_NONE && !key_size;
  static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
  if (link_id != -1 || (!group && !pairwise) || !address ||
      memcmp(address, group ? broadcast : context->bssid, 6) ||
      key_id < 0 || key_id > (group ? 3 : 0) ||
      (!removing && (algorithm != WPA_ALG_CCMP || !key || key_size != 16)) ||
      (!removing && (sequence_size != 6 || !sequence)) ||
      (removing && (key || sequence_size || sequence)) || (!group && !removing && !set_tx)) {
    rsn_fail(context, CALL_BAD_OPERATION);
    return -1;
  }
  uint64_t receive_pn = 0;
  for (size_t i = 0; i < sequence_size; ++i) {
    receive_pn |= (uint64_t)sequence[i] << (8 * i);
  }
  if (context->authorized) {
    /* Upstream reaches key installation only after authenticating the frame.
     * An unauthenticated message 1 may change state but cannot pause data. */
    uint64_t now;
    if (current_time(context, &now) != CALL_OK) {
      return -1;
    }
    if (now > UINT64_MAX - RSN_REKEY_MAX_NS) {
      rsn_fail(context, CALL_LIMIT);
      return -1;
    }
    context->phase_deadline = now + RSN_REKEY_MAX_NS;
    context->timeout_cancelled = false;
    if (authorize(context, false)) {
      return -1;
    }
  }
  uint32_t action = removing ? WIFI_AUTH_KEY_REMOVE : WIFI_AUTH_KEY_INSTALL;
  enum call_status status = wifi_auth_key(context->auth, context->generation, group,
      (uint32_t)key_id, action, receive_pn, removing ? NULL : key);
  if (status != CALL_OK) {
    rsn_fail(context, status);
    return -1;
  }
  if (group) {
    if (removing) {
      context->group_installed &= (uint8_t)~(1u << key_id);
    } else {
      context->group_installed |= (uint8_t)(1u << key_id);
    }
  } else {
    context->pairwise_installed = !removing;
  }
  return 0;
}

static int set_protection(void *opaque, const u8 *address, int protection, int key_type)
{
  struct rsn_context *context = opaque;
  if (!address || memcmp(address, context->bssid, sizeof(context->bssid)) ||
      key_type != MLME_SETPROTECTION_KEY_TYPE_PAIRWISE ||
      (protection != MLME_SETPROTECTION_PROTECT_TYPE_RX &&
       protection != MLME_SETPROTECTION_PROTECT_TYPE_RX_TX)) {
    rsn_fail(context, CALL_BAD_OPERATION);
    return -1;
  }
  /* Key replacement owns the data pause, including when upstream skips an
   * already installed key while processing an authenticated retransmission. */
  return authorize(context, context->authorized || context->state == WPA_COMPLETED);
}

static bool valid_address(const uint8_t address[6])
{
  uint8_t any = 0;
  for (size_t i = 0; i < 6; ++i) {
    any |= address[i];
  }
  return any && !(address[0] & 1);
}

static bool valid_rsn(const uint8_t *bytes, size_t size, bool association)
{
  if (size < 2 || size > WIFI_AUTH_RSN_BYTES || bytes[0] != WLAN_EID_RSN ||
      (size_t)bytes[1] + 2 != size) {
    return false;
  }
  struct wpa_ie_data parsed = {0};
  if (wpa_parse_wpa_ie_rsn(bytes, size, &parsed) || parsed.proto != WPA_PROTO_RSN ||
      !parsed.has_group || parsed.group_cipher != WPA_CIPHER_CCMP ||
      !parsed.has_pairwise || !(parsed.pairwise_cipher & WPA_CIPHER_CCMP) ||
      !(parsed.key_mgmt & WPA_KEY_MGMT_PSK) || parsed.num_pmkid ||
      (parsed.capabilities & (WPA_CAPABILITY_MFPR | WPA_CAPABILITY_SPP_A_MSDU_REQUIRED))) {
    return false;
  }
  if (association && (parsed.pairwise_cipher != WPA_CIPHER_CCMP ||
      parsed.key_mgmt != WPA_KEY_MGMT_PSK ||
      (parsed.capabilities & (WPA_CAPABILITY_MFPC | WPA_CAPABILITY_OCVC |
        WPA_CAPABILITY_EXT_KEY_ID_FOR_UNICAST)))) {
    return false;
  }
  return true;
}

static enum call_status read_pmk(struct rsn_context *context, handle_t file, uint8_t pmk[32])
{
  struct wifi_credential credential = {0};
  struct handle_info info;
  uint64_t size = 0;
  size_t count = 0;
  enum call_status status = handle_query(file, &info);
  if (status == CALL_OK && (info.kind != HANDLE_KIND_NATIVE ||
      info.protocol != PROTOCOL_FILE || info.rights != FILE_RIGHT_READ || info.transport)) {
    status = CALL_DENIED;
  }
  if (status == CALL_OK) {
    status = file_size(file, &size);
  }
  if (status == CALL_OK && size != sizeof(credential)) {
    status = CALL_BAD_REQUEST;
  }
  if (status == CALL_OK) {
    status = file_read(file, 0, &credential, sizeof(credential), &count);
  }
  if (status == CALL_OK && count != sizeof(credential)) {
    status = CALL_IO;
  }
  if (status == CALL_OK) {
    if ((credential.format != WIFI_CREDENTIAL_PASSPHRASE &&
         credential.format != WIFI_CREDENTIAL_PMK) ||
        credential.length > sizeof(credential.bytes) ||
        (credential.format == WIFI_CREDENTIAL_PASSPHRASE &&
         (credential.length < 8 || credential.length > 63)) ||
        (credential.format == WIFI_CREDENTIAL_PMK && credential.length != 32)) {
      status = CALL_BAD_REQUEST;
    }
  }
  for (size_t i = 0; status == CALL_OK && i < sizeof(credential.bytes); ++i) {
    uint8_t byte = credential.bytes[i];
    if ((i >= credential.length && byte) ||
        (credential.format == WIFI_CREDENTIAL_PASSPHRASE && i < credential.length &&
         (!byte || byte == '\r' || byte == '\n'))) {
      status = CALL_BAD_REQUEST;
    }
  }
  if (status == CALL_OK) {
    if (credential.format == WIFI_CREDENTIAL_PMK) {
      memcpy(pmk, credential.bytes, 32);
    } else if (pbkdf2_sha1((const char *)credential.bytes, context->ssid,
        context->ssid_size, 4096, pmk, 32)) {
      status = CALL_IO;
    }
  }
  memset_explicit(&credential, 0, sizeof(credential));
  if (handle_close(file) && status == CALL_OK) {
    status = CALL_IO;
  }
  return status;
}

void rsn_clear(struct rsn_context *context)
{
  if (!context) {
    return;
  }
  rsn_os_enter(context);
  if (context->sm) {
    wpa_sm_deinit(context->sm);
  }
  handle_t auth = context->auth, clock = context->clock, random = context->random;
  memset_explicit(context, 0, sizeof(*context));
  context->auth = auth;
  context->clock = clock;
  context->random = random;
  rsn_os_enter(NULL);
}

static enum call_status configure(struct rsn_context *context)
{
  struct wpa_sm_ctx *callbacks = calloc(1, sizeof(*callbacks));
  if (!callbacks) {
    return CALL_NO_MEMORY;
  }
  *callbacks = (struct wpa_sm_ctx){
    .ctx = context, .msg_ctx = context, .set_state = set_state, .get_state = get_state,
    .deauthenticate = deauthenticate, .reconnect = reconnect, .set_key = set_key,
    .get_network_ctx = get_network, .get_bssid = get_bssid, .get_beacon_ie = get_beacon_ie,
    .cancel_auth_timeout = cancel_auth_timeout, .alloc_eapol = alloc_eapol,
    .ether_send = ether_send, .mlme_setprotection = set_protection,
  };
  context->sm = wpa_sm_init(callbacks);
  if (!context->sm) {
    free(callbacks);
    return CALL_NO_MEMORY;
  }
  context->config = (struct rsn_supp_config){
    .network_ctx = context, .allowed_pairwise_cipher = WPA_CIPHER_CCMP,
    .ssid = context->ssid, .ssid_len = context->ssid_size,
  };
  wpa_sm_set_config(context->sm, &context->config);
  wpa_sm_set_own_addr(context->sm, context->local_address);
  wpa_sm_set_ssid(context->sm, context->ssid, context->ssid_size);
  const struct { enum wpa_sm_conf_params parameter; unsigned value; } parameters[] = {
    {WPA_PARAM_PROTO, WPA_PROTO_RSN}, {WPA_PARAM_PAIRWISE, WPA_CIPHER_CCMP},
    {WPA_PARAM_GROUP, WPA_CIPHER_CCMP}, {WPA_PARAM_KEY_MGMT, WPA_KEY_MGMT_PSK},
    {WPA_PARAM_RSN_ENABLED, 1}, {WPA_PARAM_MGMT_GROUP, 0}, {WPA_PARAM_MFP, 0},
    {WPA_PARAM_OCV, 0}, {WPA_PARAM_EXT_KEY_ID, 0}, {WPA_PARAM_USE_EXT_KEY_ID, 0},
  };
  for (size_t i = 0; i < sizeof(parameters) / sizeof(*parameters); ++i) {
    if (wpa_sm_set_param(context->sm, parameters[i].parameter, parameters[i].value)) {
      return CALL_BAD_OPERATION;
    }
  }
  if (wpa_sm_set_assoc_wpa_ie(context->sm, context->association_rsn,
      context->association_rsn_size) ||
      wpa_sm_set_ap_rsn_ie(context->sm, context->ap_rsn, context->ap_rsn_size)) {
    return CALL_NO_MEMORY;
  }
  return CALL_OK;
}

enum call_status rsn_begin(struct rsn_context *context, const struct wifi_auth_event *event)
{
  if (!context || !event) {
    if (event && event->credential != HANDLE_INVALID) {
      handle_close(event->credential);
    }
    return CALL_BAD_REQUEST;
  }
  rsn_clear(context);
  rsn_os_enter(context);
  context->generation = event->generation;
  context->phase_deadline = event->deadline_ns;
  context->state = WPA_ASSOCIATED;
  const struct wifi_auth_begin *begin = &event->body.begin;
  enum call_status status = CALL_OK;
  if (event->kind != WIFI_AUTH_BEGIN || !event->generation ||
      event->status != CALL_OK || event->flags || event->length != sizeof(*begin) ||
      !begin->ssid_bytes || begin->ssid_bytes > sizeof(context->ssid) ||
      !valid_address(begin->bssid) || !valid_address(begin->local_address) ||
      !valid_rsn(begin->ap_rsn, begin->ap_rsn_bytes, false) ||
      !valid_rsn(begin->association_rsn, begin->association_rsn_bytes, true)) {
    status = CALL_BAD_REQUEST;
  }
  for (size_t i = 0; status == CALL_OK && i < sizeof(begin->reserved); ++i) {
    if (begin->reserved[i]) {
      status = CALL_BAD_REQUEST;
    }
  }
  uint64_t now;
  if (status == CALL_OK) {
    status = current_time(context, &now);
    if (status == CALL_OK && (event->deadline_ns <= now ||
        event->deadline_ns - now > WIFI_CONNECT_MAX_NS)) {
      status = CALL_TIMED_OUT;
    }
  }
  if (status == CALL_OK) {
    context->ssid_size = begin->ssid_bytes;
    memcpy(context->ssid, begin->ssid, context->ssid_size);
    memcpy(context->bssid, begin->bssid, sizeof(context->bssid));
    memcpy(context->local_address, begin->local_address, sizeof(context->local_address));
    context->ap_rsn_size = begin->ap_rsn_bytes;
    context->association_rsn_size = begin->association_rsn_bytes;
    memcpy(context->ap_rsn, begin->ap_rsn, context->ap_rsn_size);
    memcpy(context->association_rsn, begin->association_rsn, context->association_rsn_size);
  }
  uint8_t pmk[32] = {0};
  if (status == CALL_OK) {
    status = read_pmk(context, event->credential, pmk);
  } else if (event->credential != HANDLE_INVALID && handle_close(event->credential)) {
    status = CALL_IO;
  }
  if (status == CALL_OK) {
    status = current_time(context, &now);
    if (status == CALL_OK && now >= context->phase_deadline) {
      status = CALL_TIMED_OUT;
    }
  }
  if (status == CALL_OK) {
    status = configure(context);
  }
  if (status == CALL_OK) {
    wpa_sm_set_pmk(context->sm, pmk, sizeof(pmk), NULL, NULL);
    wpa_sm_notify_assoc(context->sm, context->bssid);
  }
  memset_explicit(pmk, 0, sizeof(pmk));
  if (status != CALL_OK) {
    rsn_fail(context, status);
  }
  return context->failure == CALL_OK ? status : context->failure;
}

enum call_status rsn_eapol(struct rsn_context *context, const struct wifi_auth_event *event)
{
  if (!context || !event || !context->sm || event->generation != context->generation) {
    return CALL_UNAVAILABLE;
  }
  if (context->failure != CALL_OK) {
    return context->failure;
  }
  if (event->kind != WIFI_AUTH_EAPOL || event->length > RSN_EAPOL_LIMIT ||
      event->status != CALL_OK || (event->flags & ~WIFI_AUTH_EVENT_ENCRYPTED) ||
      event->credential != HANDLE_INVALID) {
    rsn_fail(context, CALL_BAD_REQUEST);
    return context->failure;
  }
  uint64_t now;
  if (current_time(context, &now) != CALL_OK) {
    return context->failure;
  }
  if (now >= context->phase_deadline) {
    rsn_fail(context, CALL_TIMED_OUT);
    return context->failure;
  }
  rsn_os_enter(context);
  /* The native event marks encryption only after MIC and PN validation. */
  enum frame_encryption encrypted = (event->flags & WIFI_AUTH_EVENT_ENCRYPTED) ?
      FRAME_ENCRYPTED : FRAME_NOT_ENCRYPTED;
  wpa_sm_rx_eapol(context->sm, context->bssid, event->body.data, event->length, encrypted);
  return context->failure;
}

uint64_t rsn_deadline(const struct rsn_context *context)
{
  if (!context || !context->sm || context->failure != CALL_OK) {
    return UINT64_MAX;
  }
  uint64_t deadline = context->phase_deadline;
  for (size_t i = 0; i < RSN_TIMER_LIMIT; ++i) {
    if (context->timers[i].active && context->timers[i].deadline < deadline) {
      deadline = context->timers[i].deadline;
    }
  }
  return deadline;
}

enum call_status rsn_dispatch_timers(struct rsn_context *context)
{
  if (!context || !context->sm) {
    return CALL_OK;
  }
  rsn_os_enter(context);
  for (size_t dispatched = 0; dispatched < RSN_TIMER_LIMIT &&
      context->failure == CALL_OK; ++dispatched) {
    uint64_t now;
    if (current_time(context, &now) != CALL_OK) {
      break;
    }
    if (now >= context->phase_deadline) {
      rsn_fail(context, CALL_TIMED_OUT);
      break;
    }
    size_t selected = RSN_TIMER_LIMIT;
    for (size_t i = 0; i < RSN_TIMER_LIMIT; ++i) {
      if (context->timers[i].active && context->timers[i].deadline <= now &&
          (selected == RSN_TIMER_LIMIT ||
           context->timers[i].deadline < context->timers[selected].deadline)) {
        selected = i;
      }
    }
    if (selected == RSN_TIMER_LIMIT) {
      break;
    }
    struct rsn_timer timer = context->timers[selected];
    memset(&context->timers[selected], 0, sizeof(context->timers[selected]));
    timer.handler(timer.eloop_data, timer.user_data);
  }
  return context->failure;
}
