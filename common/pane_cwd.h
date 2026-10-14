#ifndef USERSPACE_PANE_CWD_H
#define USERSPACE_PANE_CWD_H

#include <pyxis/working_path.h>
#include <stdbool.h>

/* Private shell/mux channel, used only by the root shell at an idle prompt.
 * The receiver has one reader; all clients have SEND-only authority. CALL is
 * deliberately excluded: expiry between readiness and RECEIVE can block.
 * Clock and bootstrap are borrowed; init owns only the new receiver. */
struct pane_cwd_provider {
  handle_t receiver;
  handle_t clock;
  uint64_t next_token;
};

enum call_status pane_cwd_provider_init(struct pane_cwd_provider *provider,
    handle_t service, handle_t bootstrap, handle_t clock);
void pane_cwd_provider_close(struct pane_cwd_provider *provider);
void pane_cwd_provider_serve(void *context);

/* Polls and drains one announcement. Success transfers one owned SEND client
 * into fresh output; failure leaves it invalid. Own and close it separately. */
bool pane_cwd_receive_service(handle_t bootstrap_receiver, handle_t *caller);

/* Fresh output only. Owns a complete chain and optional NUL description on
 * success; failure leaves it empty. Environment entries are not transferred.
 * One absolute second covers BEGIN, all READ chunks and DONE. No retry after
 * an uncertain send; receiver close releases queued replies and attachments. */
enum call_status pane_cwd_snapshot(handle_t service, handle_t caller,
    handle_t clock, struct pyxis_working_snapshot *out);

#endif
