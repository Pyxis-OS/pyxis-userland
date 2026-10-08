#include "hash.h"
#include "protocol.h"
#include <directory.h>
#include <errno.h>
#include <fcntl.h>
#include <file.h>
#include <handle.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Source hashing reads and staged writes go through this one block, so
 * memory stays the same whatever the file size. */
#define XFER_BLOCK 65536

static unsigned char block[XFER_BLOCK];

struct transfer {
  struct wire wire;
  int source;
  size_t size, staged, pending;
  char digest[65];
  handle_t parent, stage_file;
  char stage_name[XFER_NAME_MAX + 96];
  bool stage_owned, published;
  const char *error_status;
};

static bool transfer_fail(struct transfer *transfer, const char *status, const char *message)
{
  if (!transfer->wire.error) {
    transfer->error_status = status;
  }
  return wire_fail(&transfer->wire, message);
}

static bool native_fail(struct transfer *transfer, enum call_status status, const char *operation)
{
  char diagnostic[192];
  snprintf(diagnostic, sizeof(diagnostic), "xfer: %s: native status %u\n", operation, (unsigned)status);
  term_print(&transfer->wire.terminal, diagnostic);
  const char *code = status == CALL_DENIED || status == CALL_READ_ONLY ? "EPERM" :
      status == CALL_ALREADY_EXISTS ? "EEXIST" :
      status == CALL_NO_SPACE || status == CALL_QUOTA ? "ENOSPC" :
      status == CALL_FILE_TOO_LARGE ? "EFBIG" :
      status == CALL_NO_MEMORY ? "ENOMEM" : "EIO";
  return transfer_fail(transfer, code, "Native filesystem operation failed");
}

static bool plain_name(const char *name)
{
  for (const unsigned char *byte = (const unsigned char *)name; *byte; byte++) {
    if (*byte < 32 || *byte == 127) {
      return false;
    }
  }
  return *name && strlen(name) <= XFER_NAME_MAX && strcmp(name, ".") &&
      strcmp(name, "..") && !strchr(name, '/') && !strchr(name, '\\') && utf8_text(name);
}

/* Exact reads: a short read means the source shrank while it was being sent. */
static bool source_read(struct transfer *transfer, void *bytes, size_t size)
{
  unsigned char *output = bytes;
  while (size) {
    ssize_t received = read(transfer->source, output, size);
    if (received <= 0) {
      return transfer_fail(transfer, "EIO", "Source read failed or the source size changed");
    }
    output += received;
    size -= received;
  }
  return true;
}

static bool source_at_end(struct transfer *transfer)
{
  unsigned char extra;
  return read(transfer->source, &extra, 1) == 0 ||
      transfer_fail(transfer, "EIO", "Source read failed or the source size changed");
}

/* First pass: the digest is announced before any data, so the whole source is
 * read once here and again while sending. */
static bool hash_source(struct transfer *transfer, const char *path, const char **name)
{
  *name = strrchr(path, '/');
  *name = *name ? *name + 1 : path;
  if (!plain_name(*name)) {
    return transfer_fail(transfer, "EINVAL", "Source must have a plain UTF-8 file name of at most 200 bytes");
  }
  transfer->source = open(path, O_RDONLY);
  if (transfer->source < 0) {
    return transfer_fail(transfer, errno == EACCES ? "EPERM" : "EIO", "Cannot open the source file");
  }
  struct stat info;
  if (fstat(transfer->source, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
    return transfer_fail(transfer, "EINVAL", "Source must be a readable regular file");
  }
  transfer->size = info.st_size;
  if (!digest_begin()) {
    return transfer_fail(transfer, "EIO", "SHA-256 failed");
  }
  size_t offset = 0;
  while (offset < transfer->size) {
    size_t count = transfer->size - offset;
    if (count > sizeof(block)) {
      count = sizeof(block);
    }
    if (!source_read(transfer, block, count)) {
      return false;
    }
    if (!digest_update(block, count)) {
      return transfer_fail(transfer, "EIO", "SHA-256 failed");
    }
    offset += count;
    if (!wire_poll_cancel(&transfer->wire)) {
      return false;
    }
  }
  if (!source_at_end(transfer)) {
    return false;
  }
  if (!digest_finish(transfer->digest)) {
    return transfer_fail(transfer, "EIO", "SHA-256 failed");
  }
  if (lseek(transfer->source, 0, SEEK_SET) != 0) {
    return transfer_fail(transfer, "EIO", "Cannot rewind the source file");
  }
  return wire_poll_cancel(&transfer->wire);
}

static bool send_file(struct transfer *transfer, const char *path)
{
  struct wire *wire = &transfer->wire;
  const char *name;
  if (!hash_source(transfer, path, &name)) {
    return false;
  }
  char encoded_name[(XFER_NAME_MAX + 2) / 3 * 4 + 1];
  char fields[XFER_FRAME_MAX];
  base64_encode(name, strlen(name), encoded_name, sizeof(encoded_name));
  snprintf(fields, sizeof(fields), "n=%s;sz=%zu;sha256=%s;px_sha256=1;ft=regular;tt=simple;zip=none",
      encoded_name, transfer->size, transfer->digest);
  wire->active = true;
  if (!wire_send(wire, "send", fields) || !wire_expect(wire, "OK", NULL, 0, true)) {
    return false;
  }
  snprintf(fields, sizeof(fields), "fid=f1;n=%s;sz=%zu;sha256=%s;ft=regular;tt=simple;zip=none",
      encoded_name, transfer->size, transfer->digest);
  if (!wire_send(wire, "file", fields) || !wire_expect(wire, "STARTED", "f1", 0, false)) {
    return false;
  }
  if (!digest_begin()) {
    return transfer_fail(transfer, "EIO", "SHA-256 failed");
  }
  size_t offset = 0;
  do {
    size_t count = transfer->size - offset;
    if (count > XFER_CHUNK_MAX) {
      count = XFER_CHUNK_MAX;
    }
    bool last = offset + count == transfer->size;
    unsigned char bytes[XFER_CHUNK_MAX];
    if (!source_read(transfer, bytes, count)) {
      return false;
    }
    if (!digest_update(bytes, count)) {
      return transfer_fail(transfer, "EIO", "SHA-256 failed");
    }
    /* The second read must match the announced digest before end_data. */
    if (last) {
      char digest[65];
      if (!source_at_end(transfer)) {
        return false;
      }
      if (!digest_finish(digest)) {
        return transfer_fail(transfer, "EIO", "SHA-256 failed");
      }
      if (!digest_equal(transfer->digest, digest)) {
        return transfer_fail(transfer, "EIO", "Source changed while it was being sent");
      }
      int source = transfer->source;
      transfer->source = -1;
      if (close(source) != 0) {
        return transfer_fail(transfer, "EIO", "Source close failed");
      }
    }
    char data[(XFER_CHUNK_MAX + 2) / 3 * 4 + 1];
    base64_encode(bytes, count, data, sizeof(data));
    snprintf(fields, sizeof(fields), "fid=f1;d=%s", data);
    if (!wire_send(wire, last ? "end_data" : "data", fields)) {
      return false;
    }
    offset += count;
    if (!wire_expect(wire, last ? "OK" : "PROGRESS", "f1", offset, false)) {
      return false;
    }
    if (last) {
      break;
    }
  } while (offset < transfer->size);
  if (!wire_send(wire, "finish", "") || !wire_expect(wire, "OK", NULL, 0, false)) {
    return false;
  }
  wire->active = false;
  return true;
}

static bool receive_authority(struct transfer *transfer, const char *name)
{
  if (!plain_name(name)) {
    return transfer_fail(transfer, "EINVAL", "Destination must be one plain UTF-8 name of at most 200 bytes");
  }
  size_t count = startup_working_directory_count();
  if (!count) {
    return transfer_fail(transfer, "EPERM", "No inherited working directory");
  }
  transfer->parent = startup_working_directory(count - 1);
  uint64_t rights;
  enum call_status status = handle_rights(transfer->parent, &rights, NULL);
  if (status != CALL_OK) {
    return native_fail(transfer, status, "query working directory");
  }
  uint64_t required = DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_REMOVE | DIRECTORY_RIGHT_WRITE_FILES;
  if ((rights & required) != required) {
    return transfer_fail(transfer, "EPERM", "Working directory needs CREATE, REMOVE and WRITE_FILES authority");
  }
  snprintf(transfer->stage_name, sizeof(transfer->stage_name), ".%s.xfer-partial-%s", name, transfer->wire.id);
  return true;
}

static bool receive_catalog(struct transfer *transfer, char host_name[XFER_PATH_MAX + 1], char fid[64])
{
  struct packet packet;
  struct wire *wire = &transfer->wire;
  if (!wire_expect(wire, "OK", NULL, 0, true) || !wire_next(wire, &packet)) {
    return false;
  }
  char actual_fid[64];
  size_t name_size, fid_size;
  if (strcmp(packet.action, "file") || !packet.fid || strcmp(packet.fid, "q1") ||
      !base64_decode(packet.status, actual_fid, sizeof(actual_fid) - 1, &fid_size) ||
      memchr(actual_fid, 0, fid_size)) {
    return transfer_fail(transfer, "EINVAL", "Expected one regular-file catalog entry");
  }
  actual_fid[fid_size] = 0;
  if (!protocol_id(actual_fid) || !packet.file_type || strcmp(packet.file_type, "regular") ||
      (packet.transfer_type && strcmp(packet.transfer_type, "simple")) ||
      (packet.compression && strcmp(packet.compression, "none")) ||
      !decimal_size(packet.size, &transfer->size) || !digest_valid(packet.sha256) ||
      !base64_decode(packet.name, host_name, XFER_PATH_MAX, &name_size) ||
      !name_size || memchr(host_name, 0, name_size)) {
    return transfer_fail(transfer, "EINVAL", "Invalid file metadata, encoding or mandatory SHA-256");
  }
  host_name[name_size] = 0;
  if (!utf8_text(host_name)) {
    return transfer_fail(transfer, "EINVAL", "Host path is not valid UTF-8");
  }
  memcpy(transfer->digest, packet.sha256, sizeof(transfer->digest));
  strcpy(fid, actual_fid);
  return wire_expect(wire, "OK", NULL, 0, false);
}

static bool stage_create(struct transfer *transfer)
{
  enum call_status status = directory_create(transfer->parent, transfer->stage_name,
      DIRECTORY_KIND_FILE, FILE_RIGHT_WRITE, &transfer->stage_file);
  if (status != CALL_OK) {
    return native_fail(transfer, status, "exclusive staging-file creation");
  }
  transfer->stage_owned = true;
  return true;
}

static bool stage_flush(struct transfer *transfer)
{
  size_t offset = 0;
  while (offset < transfer->pending) {
    size_t written;
    enum call_status status = file_write(transfer->stage_file, transfer->staged,
        block + offset, transfer->pending - offset, &written);
    if (status != CALL_OK) {
      return native_fail(transfer, status, "staging-file write");
    }
    offset += written;
    transfer->staged += written;
  }
  transfer->pending = 0;
  return true;
}

static bool stage_append(struct transfer *transfer, const void *bytes, size_t size)
{
  if (size > sizeof(block) - transfer->pending && !stage_flush(transfer)) {
    return false;
  }
  memcpy(block + transfer->pending, bytes, size);
  transfer->pending += size;
  return true;
}

static bool receive_data(struct transfer *transfer, const char *fid)
{
  struct wire *wire = &transfer->wire;
  size_t offset = 0;
  for (;;) {
    struct packet packet;
    if (!wire_next(wire, &packet)) {
      return false;
    }
    bool last = !strcmp(packet.action, "end_data");
    size_t count;
    unsigned char bytes[XFER_CHUNK_MAX];
    if ((!last && strcmp(packet.action, "data")) || !packet.fid || strcmp(packet.fid, fid) ||
        (packet.compression && strcmp(packet.compression, "none")) ||
        !base64_decode(packet.data, bytes, sizeof(bytes), &count) ||
        count > transfer->size - offset || (!last && !count)) {
      return transfer_fail(transfer, "EINVAL", "Unexpected or invalid transfer data");
    }
    if (!digest_update(bytes, count)) {
      return transfer_fail(transfer, "EIO", "SHA-256 failed");
    }
    if (!stage_append(transfer, bytes, count)) {
      return false;
    }
    offset += count;
    if (last) {
      if (offset != transfer->size) {
        return transfer_fail(transfer, "EINVAL", "Received file size does not match its metadata");
      }
      char digest[65];
      if (!digest_finish(digest)) {
        return transfer_fail(transfer, "EIO", "SHA-256 failed");
      }
      if (!digest_equal(transfer->digest, digest)) {
        return transfer_fail(transfer, "EINVAL", "Received SHA-256 does not match its metadata");
      }
      return wire_poll_cancel(wire);
    }
    if (!wire_status(wire, "PROGRESS", fid, offset)) {
      return false;
    }
  }
}

static bool publish_file(struct transfer *transfer, const char *name, bool overwrite)
{
  if (!stage_flush(transfer) || !wire_poll_cancel(&transfer->wire)) {
    return false;
  }
  enum call_status status = file_sync(transfer->stage_file);
  if (status != CALL_OK) {
    return native_fail(transfer, status, "staging-file synchronization");
  }
  if (handle_close(transfer->stage_file) != 0) {
    transfer->stage_file = HANDLE_INVALID;
    return transfer_fail(transfer, "EIO", "Staging-file handle close failed");
  }
  transfer->stage_file = HANDLE_INVALID;
  if (!wire_poll_cancel(&transfer->wire)) {
    return false;
  }
  status = directory_rename(transfer->parent, transfer->stage_name, transfer->parent,
      name, overwrite ? DIRECTORY_RENAME_REPLACE : DIRECTORY_RENAME_NO_REPLACE);
  if (status == CALL_ALREADY_EXISTS) {
    term_print(&transfer->wire.terminal, "xfer: ");
    term_print(&transfer->wire.terminal, name);
    term_print(&transfer->wire.terminal, overwrite ? " already exists and cannot be replaced\n" :
        " already exists; use --overwrite to replace it\n");
    return transfer_fail(transfer, "EEXIST", overwrite ? "Existing destination cannot be replaced" :
        "Destination already exists; use --overwrite to replace it");
  }
  if (status != CALL_OK) {
    return native_fail(transfer, status, "atomic publication (outcome may be uncertain on failure)");
  }
  /* Commit point: neither later cancellation nor cleanup removes the target. */
  transfer->stage_owned = false;
  transfer->published = true;
  status = directory_sync(transfer->parent);
  return status == CALL_OK || native_fail(transfer, status, "published directory synchronization");
}

static bool receive_file(struct transfer *transfer, const char *host_path, const char *name, bool overwrite)
{
  struct wire *wire = &transfer->wire;
  if (!receive_authority(transfer, name) || !*host_path || strlen(host_path) > XFER_PATH_MAX ||
      !utf8_text(host_path)) {
    return transfer_fail(transfer, "EINVAL", "Host path must be UTF-8 and at most 1024 bytes");
  }
  char encoded_name[(XFER_PATH_MAX + 2) / 3 * 4 + 1];
  char fields[sizeof(encoded_name) + 96];
  base64_encode(host_path, strlen(host_path), encoded_name, sizeof(encoded_name));
  wire->active = true;
  wire->buffered = true;
  if (!wire_send(wire, "receive", "sz=1;px_sha256=1;tt=simple;zip=none")) {
    return false;
  }
  snprintf(fields, sizeof(fields), "fid=q1;n=%s", encoded_name);
  if (!wire_send(wire, "file", fields)) {
    return false;
  }
  char host_name[XFER_PATH_MAX + 1], fid[64];
  if (!receive_catalog(transfer, host_name, fid)) {
    return false;
  }
  /* Data starts now: verified frames stream into the private staging name. */
  if (!stage_create(transfer)) {
    return false;
  }
  if (!digest_begin()) {
    return transfer_fail(transfer, "EIO", "SHA-256 failed");
  }
  base64_encode(host_name, strlen(host_name), encoded_name, sizeof(encoded_name));
  snprintf(fields, sizeof(fields), "fid=%s;n=%s", fid, encoded_name);
  if (!wire_send(wire, "file", fields) || !receive_data(transfer, fid) ||
      !publish_file(transfer, name, overwrite)) {
    return false;
  }
  /* No read-ahead after finish: input following the final ACK belongs to shell. */
  wire->buffered = false;
  if (!wire_send(wire, "finish", "") || !wire_expect(wire, "OK", NULL, 0, false)) {
    return false;
  }
  wire->active = false;
  return true;
}

static void cleanup(struct transfer *transfer)
{
  if (transfer->stage_file != HANDLE_INVALID) {
    if (handle_close(transfer->stage_file) != 0) {
      term_print(&transfer->wire.terminal, "xfer: staging-file handle close failed\n");
    }
  }
  if (transfer->stage_owned) {
    enum call_status status = directory_remove(transfer->parent, transfer->stage_name, DIRECTORY_KIND_FILE);
    if (status != CALL_OK && status != CALL_NOT_FOUND) {
      char diagnostic[512];
      snprintf(diagnostic, sizeof(diagnostic), "xfer: cannot remove staging file %s (native status %u)\n",
          transfer->stage_name, (unsigned)status);
      term_print(&transfer->wire.terminal, diagnostic);
    }
  }
  if (transfer->source >= 0 && close(transfer->source) != 0) {
    term_print(&transfer->wire.terminal, "xfer: source close failed\n");
  }
  hash_close();
}

int main(int argc, char **argv)
{
  bool sending = argc == 3 && !strcmp(argv[1], "send");
  bool overwrite = argc == 5 && !strcmp(argv[1], "receive") && !strcmp(argv[2], "--overwrite");
  bool receiving = (argc == 4 && !strcmp(argv[1], "receive")) || overwrite;
  if (!sending && !receiving) {
    fputs("usage: xfer send FILE\n       xfer receive [--overwrite] HOST_PATH NAME\n", stderr);
    return EXIT_FAILURE;
  }
  struct transfer transfer = {
    .wire = {
      .terminal = {startup_resource("input"), startup_resource("output")},
      .clock = startup_resource("clock"),
    },
    .source = -1,
    .stage_file = HANDLE_INVALID,
    .error_status = "EIO",
  };
  struct wire *wire = &transfer.wire;
  uint64_t now;
  if (wire->terminal.input == HANDLE_INVALID || wire->terminal.output == HANDLE_INVALID ||
      clock_now(wire->clock, &now) != CALL_OK) {
    fputs("xfer: named terminal input/output and a readable clock are required\n", stderr);
    return EXIT_FAILURE;
  }
  snprintf(wire->id, sizeof(wire->id), "px%llx", (unsigned long long)now);
  handle_t passthrough;
  if (term_passthrough(&wire->terminal, &passthrough) != CALL_OK) {
    term_print(&wire->terminal, "xfer: terminal passthrough unavailable\n");
    return EXIT_FAILURE;
  }
  bool success = hash_init(wire->clock, startup_resource("random"));
  if (!success) {
    transfer_fail(&transfer, "EIO", "PSA initialization failed; native entropy and clock grants are required");
  } else if (sending) {
    success = send_file(&transfer, argv[2]);
  } else {
    success = receive_file(&transfer, argv[overwrite ? 3 : 2], argv[overwrite ? 4 : 3], overwrite);
  }
  if (!success) {
    if (wire->active && !wire->peer_cancelled) {
      char status[192];
      snprintf(status, sizeof(status), "%s:%s", transfer.error_status, wire->error ? wire->error : "Transfer failed");
      wire_status(wire, status, NULL, 0);
    }
    wire_cancel(wire);
  }
  cleanup(&transfer);
  if (handle_close(passthrough) != 0) {
    success = wire_fail(wire, "Terminal passthrough close failed");
  }
  if (success) {
    char diagnostic[128];
    snprintf(diagnostic, sizeof(diagnostic), "xfer: %s %zu bytes, SHA-256 verified\n",
        sending ? "sent" : "received", transfer.size);
    term_print(&wire->terminal, diagnostic);
  } else {
    term_print(&wire->terminal, "xfer: ");
    term_print(&wire->terminal, wire->error ? wire->error : "Transfer failed");
    term_print(&wire->terminal, "\n");
    if (transfer.published) {
      term_print(&wire->terminal, "xfer: destination was already published and has been retained\n");
    }
  }
  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
