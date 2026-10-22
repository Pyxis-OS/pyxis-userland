#include "../common/bootstrap_environment.h"
#include "profile.h"
#include <environment.h>
#include <output.h>
#include <pyxis/stdio.h>
#include <wifi.h>
#include <startup.h>
#include <clock.h>
#include <console.h>
#include <term.h>
#include <file.h>
#include <handle.h>
#include <wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failure(const char *operation, enum call_status status)
{
  fprintf(stderr, "wifi: %s failed (status %u)\n", operation, (unsigned)status);
  return EXIT_FAILURE;
}

static enum call_status read_exact(handle_t file, uint64_t offset, void *buffer, size_t bytes)
{
  size_t total = 0;
  while (total < bytes) {
    size_t got;
    enum call_status status = file_read(file, offset + total,
        (unsigned char *)buffer + total, bytes - total, &got);
    if (status != CALL_OK || !got) {
      return status != CALL_OK ? status : CALL_IO;
    }
    total += got;
  }
  return CALL_OK;
}

static int print_result(handle_t file)
{
  uint64_t size;
  struct wifi_scan_file_header header;
  enum call_status status = file_size(file, &size);
  if (status != CALL_OK) {
    return failure("result size", status);
  }
  status = read_exact(file, 0, &header, sizeof(header));
  if (status != CALL_OK) {
    return failure("result header", status);
  }
  if (header.count > WIFI_SCAN_RECORDS || header.record_bytes != sizeof(struct wifi_bss_record) ||
      header.reserved || (header.channels & ~WIFI_SCAN_CHANNELS) ||
      size != sizeof(header) + (uint64_t)header.count * header.record_bytes) {
    return failure("result format", CALL_BAD_REQUEST);
  }
  struct wifi_bss_record records[WIFI_SCAN_RECORDS];
  status = read_exact(file, sizeof(header), records, header.count * sizeof(*records));
  if (status != CALL_OK) {
    return failure("result records", status);
  }
  /* Validate the complete bounded snapshot before printing any records. */
  for (size_t i = 0; i < header.count; ++i) {
    if (records[i].ssid_bytes > sizeof(records[i].ssid) || !records[i].channel ||
        records[i].channel > WIFI_CHANNEL_24_COUNT || !(header.channels & (1u << (records[i].channel - 1))) ||
        (records[i].security_flags & ~WIFI_BSS_RSN_FLAGS)) {
      return failure("result record", CALL_BAD_REQUEST);
    }
  }
  printf("wifi: passive scan complete; %u records\n", header.count);
  for (size_t i = 0; i < header.count; ++i) {
    const struct wifi_bss_record *record = &records[i];
    printf("ch=%u signal=%d BSSID=", record->channel, record->signal_dbm);
    for (size_t j = 0; j < sizeof(record->bssid); ++j) {
      printf("%s%02x", j ? ":" : "", record->bssid[j]);
    }
    fputs(" SSID=\"", stdout);
    for (size_t j = 0; j < record->ssid_bytes; ++j) {
      unsigned char byte = record->ssid[j];
      if (byte >= 0x20 && byte <= 0x7e && byte != '"' && byte != '\\') {
        fputc(byte, stdout);
      } else {
        printf("\\x%02x", byte);
      }
    }
    printf("\" security=%u\n", record->security_flags);
  }
  return ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int scan(void)
{
  handle_t radio = bootstrap_capability("wifi_scan");
  if (radio == HANDLE_INVALID) {
    return failure("SCAN authority", CALL_DENIED);
  }
  handle_t clock = bootstrap_capability("clock"), operation;
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK || now > UINT64_MAX - WIFI_SCAN_MAX_NS) {
    return failure("clock", status == CALL_OK ? CALL_BAD_REQUEST : status);
  }
  uint64_t deadline = now + WIFI_SCAN_MAX_NS;
  status = wifi_scan_start(radio, 0, deadline, &operation);
  if (status != CALL_OK) {
    return failure("scan start", status);
  }
  handle_t interrupt = HANDLE_INVALID;
  handle_t input = bootstrap_capability("stdin");
  if (input != HANDLE_INVALID) {
    status = console_arm_interrupt(input, &interrupt);
    if (status != CALL_OK && status != CALL_DENIED && status != CALL_WRONG_TYPE && status != CALL_BAD_OPERATION) {
      handle_close(operation);
      return failure("interrupt arm", status);
    }
  }
  struct wait_interest interests[] = {
    {operation, WAIT_COMPLETE, 0}, {interrupt, WAIT_INTERRUPT, 0},
  };
  uint64_t events[2] = {0};
  status = wait_many(interests, interrupt == HANDLE_INVALID ? 1 : 2, deadline, events);
  if (interrupt != HANDLE_INVALID) {
    handle_close(interrupt);
  }
  if (status != CALL_OK || (events[1] & WAIT_INTERRUPT)) {
    wifi_scan_cancel(operation);
    handle_close(operation);
    return failure("scan wait", status != CALL_OK ? status : CALL_ABANDONED);
  }
  handle_t file;
  status = wifi_scan_take(operation, &file);
  handle_close(operation);
  if (status != CALL_OK) {
    return failure("scan result", status);
  }
  int result = print_result(file);
  handle_close(file);
  return result;
}

/* Prompt through stderr only: stdout is the empty typed lease producer. */
static enum call_status prompt_credential(handle_t radio, handle_t *credential)
{
  *credential = HANDLE_INVALID;
  struct pyxis_stdio_binding input, error;
  if (pyxis_stdio_stream(stdin, &input) || pyxis_stdio_stream(stderr, &error) ||
      input.protocol != PROTOCOL_CONSOLE || error.protocol != PROTOCOL_CONSOLE) {
    return CALL_DENIED;
  }
  struct terminal terminal = {input.handle, error.handle};
  enum call_status status = term_print(&terminal, "Wi-Fi passphrase (not echoed): ");
  char bytes[WIFI_CREDENTIAL_BYTES + 1] = {0};
  if (status == CALL_OK) {
    struct term_line_result line = term_read_line_quiet(&terminal, "", bytes, sizeof(bytes));
    enum call_status newline = term_print(&terminal, "\n");
    if (line.status != TERM_LINE_OK) {
      status = line.status == TERM_LINE_ERROR || line.status == TERM_LINE_INPUT_LOST ?
          line.error : CALL_ABANDONED;
    } else if (line.limit_reached || line.length < 8 || line.length > 63) {
      status = CALL_BAD_REQUEST;
    } else if (newline != CALL_OK) {
      status = newline;
    } else {
      status = wifi_credential_seal(radio, WIFI_CREDENTIAL_PASSPHRASE,
          bytes, line.length, credential);
    }
  }
  memset_explicit(bytes, 0, sizeof(bytes));
  return status;
}

static int connect_profile(const char *path)
{
  /* Require an empty native typed collector before reading a profile or TX.
   * A console or byte pipeline cannot silently orphan a successful lease. */
  struct pyxis_stdio_binding binding;
  struct output_status_reply output;
  if (pyxis_stdio_stream(stdout, &binding) != 0 || binding.protocol != PROTOCOL_FILE) {
    return failure("typed capture output (use link=$(wifi connect --profile ...))", CALL_BAD_OPERATION);
  }
  enum call_status status = output_producer_status(binding.handle, &output);
  if (status != CALL_OK || output.mode != OUTPUT_MODE_EMPTY || output.flags) {
    return failure("empty typed capture", status == CALL_OK ? CALL_IO : status);
  }
  handle_t radio = bootstrap_capability("wifi_connect");
  if (!radio) return failure("CONNECT authority", CALL_DENIED);
  struct wifi_target target;
  status = wifi_read_profile(path, &target);
  if (status != CALL_OK) return failure("private profile", status);
  handle_t credential = HANDLE_INVALID;
  if (target.security == WIFI_SECURITY_WPA2_PSK) {
    status = prompt_credential(radio, &credential);
    if (status != CALL_OK) return failure("credential input", status);
  }
  handle_t clock = bootstrap_capability("clock"), operation = HANDLE_INVALID;
  uint64_t now = 0;
  status = clock_now(clock, &now);
  if (status != CALL_OK || now > UINT64_MAX - WIFI_CONNECT_MAX_NS) {
    if (credential) handle_close(credential);
    return failure("clock", status == CALL_OK ? CALL_LIMIT : status);
  }
  uint64_t deadline = now + WIFI_CONNECT_MAX_NS;
  status = wifi_connect_start(radio, &target, credential, deadline, &operation);
  memset_explicit(&target, 0, sizeof(target));
  if (credential) handle_close(credential);
  if (status != CALL_OK) return failure("connect start", status);
  handle_t interrupt = HANDLE_INVALID, input = bootstrap_capability("stdin");
  if (input) {
    status = console_arm_interrupt(input, &interrupt);
    if (status != CALL_OK && status != CALL_DENIED && status != CALL_WRONG_TYPE && status != CALL_BAD_OPERATION) {
      handle_close(operation);
      return failure("interrupt arm", status);
    }
  }
  struct wait_interest interests[] = {{operation, WAIT_COMPLETE, 0}, {interrupt, WAIT_INTERRUPT, 0}};
  uint64_t events[2] = {0};
  status = wait_many(interests, interrupt ? 2 : 1, deadline, events);
  if (interrupt) handle_close(interrupt);
  if (status != CALL_OK || (events[1] & WAIT_INTERRUPT)) {
    wifi_connect_cancel(operation);
    handle_close(operation);
    return failure("connect wait", status == CALL_OK ? CALL_ABANDONED : status);
  }
  handle_t link = HANDLE_INVALID;
  status = wifi_connect_take(operation, &link);
  if (handle_close(operation) && status == CALL_OK) status = CALL_BAD_HANDLE;
  if (status != CALL_OK) {
    if (link) handle_close(link);
    return failure("connection result", status);
  }
  struct output_handoff_request owned = {.value = link};
  status = pyxis_stdio_handoff_owned(stdout, &owned);
  if (owned.value) handle_close(owned.value);
  return status == CALL_OK ? EXIT_SUCCESS : failure("publish connection (link is being drained)", status);
}

static int link_command(const char *operation, const char *argument)
{
  const char *tail = strstr(argument, "://");
  if (!tail || tail == argument || tail[3] || (size_t)(tail - argument) > ENVIRONMENT_NAME_MAX) {
    return failure("held link (use $link)", CALL_BAD_REQUEST);
  }
  char name[ENVIRONMENT_NAME_MAX + 1];
  memcpy(name, argument, tail - argument);
  name[tail - argument] = 0;
  handle_t link = HANDLE_INVALID;
  enum call_status status = environment_capability(startup_environment_handle(), name, &link);
  if (status != CALL_OK) return failure("held link", status);
  struct handle_info info;
  status = handle_query(link, &info);
  if (status == CALL_OK && (info.protocol != PROTOCOL_WIFI_LINK || info.kind != HANDLE_KIND_NATIVE)) {
    status = CALL_WRONG_TYPE;
  }
  if (status == CALL_OK && !strcmp(operation, "disconnect")) {
    status = wifi_link_disconnect(link);
    uint64_t now;
    if (status == CALL_OK) status = clock_now(bootstrap_capability("clock"), &now);
    if (status == CALL_OK && now > UINT64_MAX - WIFI_CONNECT_MAX_NS) status = CALL_LIMIT;
    if (status == CALL_OK) {
      struct wait_interest interest = {link, WAIT_CLOSED, 0};
      uint64_t events;
      status = wait_many(&interest, 1, now + WIFI_CONNECT_MAX_NS, &events);
      if (status == CALL_OK && !(events & WAIT_CLOSED)) status = CALL_TIMED_OUT;
    }
  }
  struct wifi_link_status_reply reply;
  if (status == CALL_OK) status = wifi_link_status(link, &reply);
  if (handle_close(link) && status == CALL_OK) status = CALL_BAD_HANDLE;
  if (status != CALL_OK) return failure(operation, status);
  printf("wifi: generation=%llu interface=%llu associated=%u open=%u data-ready=%u terminal=%u status=%u authenticated=%u\n",
      (unsigned long long)reply.generation, (unsigned long long)reply.interface_id,
      !!(reply.flags & WIFI_LINK_ASSOCIATED), !!(reply.flags & WIFI_LINK_OPEN),
      !!(reply.flags & WIFI_LINK_DATA_READY), !!(reply.flags & WIFI_LINK_TERMINAL), reply.status,
      !!(reply.flags & WIFI_LINK_AUTHENTICATED));
  if (!strcmp(operation, "disconnect") && reply.status != CALL_OK && reply.status != CALL_ABANDONED) {
    return failure("disconnect drain", reply.status);
  }
  return ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void print_boot_facts(const struct wifi_boot_facts *boot)
{
  printf("wifi: boot valid=%x state=%x\n", boot->valid, boot->state);
  if (boot->valid & WIFI_BOOT_RESET_VALID) {
    printf("wifi: reset method=%s devcap=%x pmcsr=%x\n",
        boot->reset_method == WIFI_RESET_FLR ? "flr" : "power",
        boot->pci_device_capabilities, (unsigned)boot->pci_power_control);
  }
  if (boot->valid & WIFI_BOOT_CUT_VALID) {
    printf("wifi: cut=%u\n", (unsigned)boot->live_cut);
  }
  if (boot->valid & WIFI_BOOT_SECURITY_VALID) {
    printf("wifi: security secure=%u\n", (unsigned)boot->secure_boot);
  }
  if (boot->valid & WIFI_BOOT_IMAGE_VALID) {
    printf("wifi: firmware=%u.%u.%u.%u\n", (unsigned)boot->firmware_version[0],
        (unsigned)boot->firmware_version[1], (unsigned)boot->firmware_version[2],
        (unsigned)boot->firmware_version[3]);
  }
  if (boot->valid & WIFI_BOOT_BOARD_VALID) {
    printf("wifi: board rfe=%u xtal=%u NSS=%u/%u antennas=%u/%u country=%02x%02x\n",
        (unsigned)boot->rfe, (unsigned)boot->xtal, (unsigned)boot->tx_nss,
        (unsigned)boot->rx_nss, (unsigned)boot->tx_antennas,
        (unsigned)boot->rx_antennas, (unsigned)boot->board_country[0],
        (unsigned)boot->board_country[1]);
  }
}

int main(int argc, char **argv)
{
  if (argc == 4 && !strcmp(argv[1], "connect") && !strcmp(argv[2], "--profile")) {
    return connect_profile(argv[3]);
  }
  if (argc == 3 && (!strcmp(argv[1], "status") || !strcmp(argv[1], "disconnect"))) {
    return link_command(argv[1], argv[2]);
  }
  if (argc != 2 || (strcmp(argv[1], "status") && strcmp(argv[1], "scan"))) {
    fputs("usage: wifi status [LINK] | scan | connect --profile FILE | disconnect LINK\n", stderr);
    return EXIT_FAILURE;
  }
  if (!strcmp(argv[1], "scan")) return scan();
  handle_t radio = bootstrap_capability("wifi");
  if (!radio) return failure("READ authority", CALL_DENIED);
  struct wifi_status_reply reply;
  enum call_status status = wifi_status(radio, &reply);
  if (status != CALL_OK) return failure("status", status);
  printf("wifi: present=%u enabled=%u prepared=%u ready=%u rfkill=%u failed=%u\n",
      !!(reply.flags & WIFI_PRESENT), !!(reply.flags & WIFI_ENABLED),
      !!(reply.flags & WIFI_PREPARED), !!(reply.flags & WIFI_READY),
      !!(reply.flags & WIFI_RFKILL), !!(reply.flags & WIFI_FAILED));
  printf("wifi: %s\n", reply.detail);
  printf("wifi: DMA=%llu firmware=%llu RX=%u buffers=%u command=%u queues=%u high-water=%u/%u\n",
      (unsigned long long)reply.dma_bytes, (unsigned long long)reply.firmware_dma_bytes,
      reply.rx_entries, reply.rx_buffers, reply.command_entries, reply.command_queues,
      reply.rx_high_water, reply.command_high_water);
  printf("wifi: interrupts=%llu packets=%llu passive-mask=%x policy-mask=%x\n",
      (unsigned long long)reply.interrupts, (unsigned long long)reply.received_packets,
      reply.allowed_channels, reply.policy_channels);
  printf("wifi: data TX queued=%llu complete=%llu failed=%llu dropped=%llu pending=%u high-water=%u ring=%u RX queued=%llu dropped=%llu high-water=%u\n",
      (unsigned long long)reply.tx_queued, (unsigned long long)reply.tx_completed,
      (unsigned long long)reply.tx_failed, (unsigned long long)reply.tx_dropped,
      reply.data_tx_pending, reply.data_tx_high_water, reply.data_ring_entries,
      (unsigned long long)reply.rx_queued, (unsigned long long)reply.rx_dropped, reply.data_rx_high_water);
  print_boot_facts(&reply.boot);
  return ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
}
