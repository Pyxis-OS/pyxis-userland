#ifndef WIFI_SUPPLICANT_RSN_H
#define WIFI_SUPPLICANT_RSN_H

#include <abi/wifi.h>
#include <abi/syscall.h>
#include <wpa-rsn.h>

#define RSN_TIMER_LIMIT 8u
#define RSN_EAPOL_LIMIT 1500u
#define RSN_REKEY_MAX_NS UINT64_C(30000000000)

struct rsn_timer {
  bool active;
  uint64_t deadline;
  eloop_timeout_handler handler;
  void *eloop_data, *user_data;
};

/* One leaf worker, one thread, one active state machine. Grants are borrowed.
 * BEGIN consumes its credential handle, including on failure; no FILE or PMK
 * copy remains here. The upstream state machine owns its transient key state. */
struct rsn_context {
  handle_t auth, clock, random;
  struct wpa_sm *sm;
  struct rsn_supp_config config;
  enum wpa_states state;
  enum call_status failure;
  uint64_t generation, phase_deadline;
  bool authorized, pairwise_installed, failure_sent, timeout_cancelled;
  uint8_t group_installed;
  uint8_t bssid[6], local_address[6], ssid[32];
  size_t ssid_size, ap_rsn_size, association_rsn_size;
  uint8_t ap_rsn[WIFI_AUTH_RSN_BYTES], association_rsn[WIFI_AUTH_RSN_BYTES];
  struct rsn_timer timers[RSN_TIMER_LIMIT];
};

enum call_status rsn_begin(struct rsn_context *context, const struct wifi_auth_event *event);
enum call_status rsn_eapol(struct rsn_context *context, const struct wifi_auth_event *event);
enum call_status rsn_dispatch_timers(struct rsn_context *context);
/* UINT64_MAX when no timer/handshake deadline is pending. */
uint64_t rsn_deadline(const struct rsn_context *context);
/* Local erasure/cancellation only: kernel auth lifetime owns port/key teardown. */
void rsn_clear(struct rsn_context *context);

/* Private OS adapter state, never used concurrently or from another thread. */
struct rsn_context *rsn_os_context(void);
void rsn_os_enter(struct rsn_context *context);
void rsn_fail(struct rsn_context *context, enum call_status status);

#endif
