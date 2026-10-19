#include "../common/bootstrap_environment.h"
#include <wifi.h>
#include <startup.h>
#include <clock.h>
#include <console.h>
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
        records[i].channel > 11 || !(header.channels & (1u << (records[i].channel - 1)))) {
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
    fputs("\"\n", stdout);
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

int main(int argc, char **argv)
{
  if (argc != 2 || (strcmp(argv[1], "status") && strcmp(argv[1], "scan"))) {
    fputs("usage: wifi status | wifi scan\n", stderr);
    return EXIT_FAILURE;
  }
  if (!strcmp(argv[1], "scan")) {
    return scan();
  }
  handle_t radio = bootstrap_capability("wifi");
  if (radio == HANDLE_INVALID) {
    return failure("READ authority", CALL_DENIED);
  }
  struct wifi_status_reply reply;
  enum call_status status = wifi_status(radio, &reply);
  if (status != CALL_OK) {
    return failure("status", status);
  }
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
  return ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
}
