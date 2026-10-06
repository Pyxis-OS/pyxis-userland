#ifndef XFER_PROTOCOL_H
#define XFER_PROTOCOL_H

#include <clock.h>
#include <term.h>

#define XFER_FRAME_MAX 4096
#define XFER_CHUNK_MAX 2048
#define XFER_FILE_MAX (16 * 1024 * 1024)
#define XFER_NAME_MAX 200
#define XFER_PATH_MAX 1024
#define XFER_IDLE_NS UINT64_C(120000000000)
#define XFER_CANCEL_NS UINT64_C(5000000000)

struct packet {
  char body[XFER_FRAME_MAX];
  const char *action, *id, *fid, *name, *status, *size, *data;
  const char *file_type, *transfer_type, *compression, *sha256, *extension;
};

struct wire {
  struct terminal terminal;
  handle_t clock;
  char id[64];
  const char *error;
  bool active, peer_cancelled, buffered;
  unsigned char input[XFER_FRAME_MAX];
  size_t input_offset, input_size;
};

bool wire_fail(struct wire *wire, const char *message);
bool wire_send(struct wire *wire, const char *action, const char *fields);
bool wire_status(struct wire *wire, const char *status, const char *fid, size_t size);
bool wire_next(struct wire *wire, struct packet *packet);
bool wire_expect(struct wire *wire, const char *status, const char *fid,
    size_t size, bool extension);
void wire_cancel(struct wire *wire);
bool wire_poll_cancel(struct wire *wire);
bool base64_encode(const void *input, size_t size, char *output, size_t capacity);
bool base64_decode(const char *input, void *output, size_t capacity, size_t *size);
bool decimal_size(const char *input, size_t *size);
bool protocol_id(const char *input);
bool utf8_text(const char *input);

#endif
