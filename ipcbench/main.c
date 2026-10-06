#include <abi/clock.h>
#include <abi/directory.h>
#include <abi/endpoint.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <clock.h>
#include <directory.h>
#include <endpoint.h>
#include <handle.h>
#include <launcher.h>
#include <process.h>
#include <startup.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IPCBENCH_PROTOCOL UINT64_C(0x69706362656e6368)
#define IPCBENCH_OBJECT UINT64_C(1)
#define CALL_BUDGET_NS UINT64_C(30000000000)
#define MAX_MESSAGES 256
#define DEFAULT_MESSAGES 8
#define MAX_ROUNDS 100
#define SEND_GROUP 8
#define CLOCK_READS 1000

enum mode { MODE_CALL = 1, MODE_SEND = 2 };
enum operation { CONFIGURE = 1, RESET, ECHO, DRAIN, VERIFY, STOP };
enum result { RESULT_OK, RESULT_INVALID };

struct options {
  enum mode mode;
  size_t size, messages, rounds;
};

struct command {
  uint64_t operation, size, messages, count;
};

struct sample {
  uint64_t elapsed_ns, admission_ns, attempted, admitted, rejected, consumed;
  uint64_t request_bytes, reply_bytes, round_trips, failed_calls, failed_delivery;
  enum call_status status;
  bool verified, cleanup;
};

struct receiver {
  enum mode mode;
  handle_t control, data;
  size_t size, messages, consumed;
  uint8_t *retained;
  bool configured, valid;
};

static bool number(const char *text, size_t maximum, size_t *result)
{
  size_t value = 0;
  if (!*text) {
    return false;
  }
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') {
      return false;
    }
    size_t digit = *text - '0';
    if (digit > maximum || value > (maximum - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *result = value;
  return true;
}

static bool parse_options(int argc, char **argv, struct options *options)
{
  *options = (struct options){MODE_CALL, 64, DEFAULT_MESSAGES, 5};
  if (argc < 2) {
    return false;
  }
  if (!strcmp(argv[1], "send")) {
    options->mode = MODE_SEND;
  } else if (strcmp(argv[1], "call")) {
    return false;
  }
  unsigned seen = 0;
  for (int i = 2; i < argc; i += 2) {
    if (i + 1 == argc) {
      return false;
    }
    size_t *value;
    size_t maximum;
    unsigned bit;
    if (!strcmp(argv[i], "--size")) {
      value = &options->size;
      maximum = ENDPOINT_DATA_MAX;
      bit = 1;
    } else if (!strcmp(argv[i], "--messages")) {
      value = &options->messages;
      maximum = MAX_MESSAGES;
      bit = 2;
    } else if (!strcmp(argv[i], "--rounds")) {
      value = &options->rounds;
      maximum = MAX_ROUNDS;
      bit = 4;
    } else {
      return false;
    }
    if ((seen & bit) || !number(argv[i + 1], maximum, value)) {
      return false;
    }
    seen |= bit;
  }
  return options->messages != 0 && options->rounds != 0;
}

static uint8_t pattern(size_t message, size_t offset)
{
  return (uint8_t)(message ^ (message >> 8) ^ offset ^ (offset >> 8) ^ 0xa5);
}

static void fill_payload(uint8_t *bytes, size_t size, size_t messages)
{
  for (size_t message = 0; message < messages; ++message) {
    for (size_t offset = 0; offset < size; ++offset) {
      bytes[message * size + offset] = pattern(message, offset);
    }
  }
}

static bool verify_payload(const uint8_t *bytes, size_t size, size_t messages)
{
  for (size_t message = 0; message < messages; ++message) {
    for (size_t offset = 0; offset < size; ++offset) {
      if (bytes[message * size + offset] != pattern(message, offset)) {
        return false;
      }
    }
  }
  return true;
}

static bool close_handle(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  bool ok = handle_close(*handle) == 0;
  *handle = HANDLE_INVALID;
  return ok;
}

static bool close_grants(const struct endpoint_packet *packet)
{
  bool ok = true;
  for (size_t i = 0; i < packet->grant_count; ++i) {
    if (handle_close(packet->grants[i].handle) != 0) {
      ok = false;
    }
  }
  return ok;
}

static enum call_status deadline(handle_t clock, uint64_t *value)
{
  enum call_status status = clock_now(clock, value);
  if (status != CALL_OK) {
    return status;
  }
  if (*value > UINT64_MAX - CALL_BUDGET_NS) {
    return CALL_LIMIT;
  }
  *value += CALL_BUDGET_NS;
  return CALL_OK;
}

static enum call_status command_call(handle_t caller,
    const struct command *command, uint64_t deadline_ns, uint64_t *count)
{
  struct endpoint_packet reply;
  enum call_status status = endpoint_invoke(caller, IPCBENCH_PROTOCOL, command->operation,
      command, sizeof(*command), NULL, 0, deadline_ns, &reply);
  if (status != CALL_OK) {
    return status;
  }
  bool valid = reply.grant_count == 0 && reply.result == RESULT_OK &&
      reply.size == sizeof(*count);
  if (valid) {
    memcpy(count, reply.data, sizeof(*count));
  }
  if (!close_grants(&reply) || !valid) {
    return CALL_IO;
  }
  return CALL_OK;
}

static enum call_status untimed_command(handle_t clock, handle_t caller,
    const struct command *command, uint64_t *count)
{
  uint64_t deadline_ns;
  enum call_status status = deadline(clock, &deadline_ns);
  return status == CALL_OK ? command_call(caller, command, deadline_ns, count) : status;
}

static bool reply_packet(struct endpoint_packet *packet, uint64_t result,
    const void *bytes, size_t size)
{
  enum call_status status = endpoint_reply(packet->receipt, result, bytes, size, NULL, 0);
  bool ok = close_grants(packet);
  if (status != CALL_OK && endpoint_finish(packet->receipt) != CALL_OK) {
    ok = false;
  }
  /* A timed-out CALL no longer has reply authority. Finishing removes its
   * cancellation notice and releases the provider's delivery slot. */
  return ok && (status == CALL_OK || status == CALL_TIMED_OUT);
}

static bool retain_packet(struct receiver *receiver, const struct endpoint_packet *packet)
{
  bool valid = receiver->configured && receiver->consumed < receiver->messages &&
      packet->size == receiver->size && packet->grant_count == 0;
  if (valid) {
    memcpy(receiver->retained + receiver->consumed * receiver->size,
        packet->data, receiver->size);
    ++receiver->consumed;
  }
  receiver->valid &= valid;
  return valid;
}

static bool drain_messages(struct receiver *receiver, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    struct endpoint_packet packet;
    if (endpoint_receive(receiver->data, &packet) != CALL_OK) {
      return false;
    }
    bool valid = packet.kind == ENDPOINT_MESSAGE_SEND && retain_packet(receiver, &packet);
    bool closed = close_grants(&packet);
    if (packet.receipt != HANDLE_INVALID && endpoint_finish(packet.receipt) != CALL_OK) {
      closed = false;
    }
    receiver->valid &= valid;
    if (!closed) {
      return false;
    }
  }
  return true;
}

static bool serve(struct receiver *receiver)
{
  for (;;) {
    struct endpoint_packet packet;
    if (endpoint_receive(receiver->control, &packet) != CALL_OK) {
      return false;
    }
    if (packet.kind == ENDPOINT_MESSAGE_CANCEL) {
      if (endpoint_finish(packet.receipt) != CALL_OK) {
        return false;
      }
      continue;
    }
    if (packet.kind == ENDPOINT_MESSAGE_RETIRE) {
      if (packet.protocol == IPCBENCH_PROTOCOL && packet.object_id == IPCBENCH_OBJECT) {
        endpoint_retire_ack(receiver->control, packet.object_id);
      }
      /* Last-client retirement wakes a receiver whose STOP could not be
       * admitted. Exiting closes its data receiver and releases queued SENDs. */
      return false;
    }
    bool envelope = packet.kind == ENDPOINT_MESSAGE_CALL && packet.grant_count == 0 &&
        packet.protocol == IPCBENCH_PROTOCOL && packet.object_id == IPCBENCH_OBJECT;
    if (envelope && receiver->mode == MODE_CALL && packet.operation == ECHO) {
      bool valid = retain_packet(receiver, &packet);
      if (!reply_packet(&packet, valid ? RESULT_OK : RESULT_INVALID, packet.data, packet.size)) {
        return false;
      }
      continue;
    }
    struct command command = {0};
    bool valid = envelope && packet.size == sizeof(command);
    if (valid) {
      memcpy(&command, packet.data, sizeof(command));
      valid = packet.operation == command.operation;
    }
    uint64_t count = receiver->consumed;
    bool stop = false;
    if (valid) {
      switch (command.operation) {
      case CONFIGURE:
        valid = !receiver->configured && command.size <= ENDPOINT_DATA_MAX &&
            command.messages != 0 && command.messages <= MAX_MESSAGES;
        if (valid) {
          size_t bytes = command.size * command.messages;
          receiver->retained = malloc(bytes ? bytes : 1);
          valid = receiver->retained != NULL;
          if (valid) {
            receiver->size = command.size;
            receiver->messages = command.messages;
            receiver->configured = true;
          }
        }
        break;
      case RESET:
        valid = receiver->configured;
        if (valid) {
          receiver->consumed = 0;
          receiver->valid = true;
          memset(receiver->retained, 0, receiver->size * receiver->messages);
          count = 0;
        }
        break;
      case DRAIN:
        valid = receiver->mode == MODE_SEND && receiver->configured &&
            command.count <= SEND_GROUP && command.count <= receiver->messages - receiver->consumed;
        if (valid) {
          if (!drain_messages(receiver, command.count)) {
            endpoint_finish(packet.receipt);
            close_grants(&packet);
            return false;
          }
          count = receiver->consumed;
          valid = receiver->valid;
        }
        break;
      case VERIFY:
        valid = receiver->configured && receiver->valid && command.count == receiver->consumed &&
            verify_payload(receiver->retained, receiver->size, receiver->consumed);
        break;
      case STOP:
        stop = true;
        break;
      default:
        valid = false;
        break;
      }
    }
    if (!envelope) {
      close_grants(&packet);
      if (packet.receipt != HANDLE_INVALID) {
        endpoint_finish(packet.receipt);
      }
      return false;
    }
    if (!reply_packet(&packet, valid ? RESULT_OK : RESULT_INVALID, &count, sizeof(count))) {
      return false;
    }
    if (stop) {
      return true;
    }
  }
}

static int receiver_main(enum mode mode)
{
  handle_t service = startup_resource("service");
  handle_t clock = startup_resource("clock");
  handle_t bootstrap = startup_resource("bootstrap");
  struct endpoint_create_reply control = {0}, data = {0};
  handle_t exported = HANDLE_INVALID;
  struct receiver receiver = {.mode = mode};
  bool ok = false;
  if (service == HANDLE_INVALID || clock == HANDLE_INVALID || bootstrap == HANDLE_INVALID ||
      endpoint_create(service, &control) != CALL_OK) {
    goto done;
  }
  struct endpoint_grant grants[2];
  size_t grant_count;
  if (endpoint_export(service, control.receiver, IPCBENCH_OBJECT, IPCBENCH_PROTOCOL,
      0, HANDLE_TRANSPORT_CALL, &exported) != CALL_OK) {
    goto done;
  }
  grants[0] = (struct endpoint_grant){exported, 0, HANDLE_TRANSPORT_CALL};
  if (mode == MODE_CALL) {
    grant_count = 1;
  } else {
    if (endpoint_create(service, &data) != CALL_OK) {
      goto done;
    }
    grants[1] = (struct endpoint_grant){data.caller, 0, HANDLE_TRANSPORT_SEND};
    grant_count = 2;
  }
  uint64_t deadline_ns, ready = mode;
  struct endpoint_packet reply;
  if (deadline(clock, &deadline_ns) != CALL_OK || endpoint_request(bootstrap,
      &ready, sizeof(ready), grants, grant_count, deadline_ns, &reply) != CALL_OK) {
    goto done;
  }
  bool acknowledged = reply.result == RESULT_OK && reply.size == 0 && reply.grant_count == 0;
  bool closed = close_grants(&reply);
  if (!acknowledged || !closed) {
    goto done;
  }
  if (!close_handle(&exported) || !close_handle(&control.caller) ||
      !close_handle(&data.caller) || !close_handle(&bootstrap)) {
    goto done;
  }
  receiver.control = control.receiver;
  receiver.data = data.receiver;
  ok = serve(&receiver);
done:
  free(receiver.retained);
  if (!close_handle(&exported)) {
    ok = false;
  }
  if (!close_handle(&control.caller)) {
    ok = false;
  }
  if (!close_handle(&control.receiver)) {
    ok = false;
  }
  if (!close_handle(&data.caller)) {
    ok = false;
  }
  if (!close_handle(&data.receiver)) {
    ok = false;
  }
  if (!close_handle(&bootstrap)) {
    ok = false;
  }
  return ok ? 0 : 1;
}

static enum call_status launch_receiver(handle_t launcher, handle_t image,
    handle_t memory, handle_t service, handle_t clock, handle_t bootstrap,
    enum mode mode, handle_t *child)
{
  enum { MEMORY, SERVICE, CLOCK, BOOTSTRAP, GRANT_COUNT };
  struct launch_grant grants[GRANT_COUNT] = {
    [MEMORY] = {memory, MEMORY_RIGHT_MANAGE, 0},
    [SERVICE] = {service, ENDPOINT_SERVICE_RIGHT_CREATE, 0},
    [CLOCK] = {clock, CLOCK_RIGHT_READ, 0},
    [BOOTSTRAP] = {bootstrap, 0, HANDLE_TRANSPORT_CALL},
  };
  struct launch_binding resources[GRANT_COUNT] = {
    {(uintptr_t)"memory", MEMORY},
    {(uintptr_t)"service", SERVICE},
    {(uintptr_t)"clock", CLOCK},
    {(uintptr_t)"bootstrap", BOOTSTRAP},
  };
  const char *arguments[] = {"ipcbench.pxe", "--receiver", mode == MODE_CALL ? "call" : "send"};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .grant_count = GRANT_COUNT,
    .resources = (uintptr_t)resources,
    .resource_count = GRANT_COUNT,
    .argv = (uintptr_t)arguments,
    .argc = sizeof(arguments) / sizeof(arguments[0]),
  };
  return launcher_launch(launcher, &request, child);
}

static enum call_status receive_ready(handle_t bootstrap, enum mode mode,
    handle_t *control, handle_t *data)
{
  struct endpoint_packet packet;
  enum call_status status = endpoint_receive(bootstrap, &packet);
  if (status != CALL_OK) {
    return status;
  }
  uint64_t ready = 0;
  bool valid = packet.kind == ENDPOINT_MESSAGE_CALL && packet.size == sizeof(ready) &&
      packet.grant_count == (mode == MODE_CALL ? 1 : 2);
  if (valid) {
    memcpy(&ready, packet.data, sizeof(ready));
    valid = ready == (uint64_t)mode;
  }
  if (valid) {
    for (size_t i = 0; i < packet.grant_count; ++i) {
      struct handle_info info;
      uint64_t transport = i == 0 ? HANDLE_TRANSPORT_CALL : HANDLE_TRANSPORT_SEND;
      if (handle_query(packet.grants[i].handle, &info) != CALL_OK ||
          info.rights != 0 || info.transport != transport ||
          (i == 0 && (info.kind != HANDLE_KIND_EXPORTED || info.protocol != IPCBENCH_PROTOCOL))) {
        valid = false;
      }
    }
  }
  if (!valid) {
    close_grants(&packet);
    if (packet.receipt != HANDLE_INVALID) {
      endpoint_finish(packet.receipt);
    }
    return CALL_IO;
  }
  *control = packet.grants[0].handle;
  if (mode == MODE_SEND) {
    *data = packet.grants[1].handle;
  }
  status = endpoint_reply(packet.receipt, RESULT_OK, NULL, 0, NULL, 0);
  if (status != CALL_OK) {
    endpoint_finish(packet.receipt);
  }
  return status;
}

static enum call_status measure_clock(handle_t clock, uint64_t *cost)
{
  uint64_t start, end;
  enum call_status status = clock_now(clock, &start);
  if (status != CALL_OK) {
    return status;
  }
  for (size_t i = 0; i < CLOCK_READS; ++i) {
    status = clock_now(clock, &end);
    if (status != CALL_OK) {
      return status;
    }
  }
  if (end < start) {
    return CALL_IO;
  }
  *cost = (end - start) / CLOCK_READS;
  return CALL_OK;
}

static void measure_call(const struct options *options, handle_t clock,
    handle_t caller, const uint8_t *payload, uint8_t *retained, bool timed, struct sample *sample)
{
  uint64_t start, end;
  sample->status = clock_now(clock, &start);
  if (sample->status != CALL_OK || start > UINT64_MAX - CALL_BUDGET_NS) {
    if (sample->status == CALL_OK) {
      sample->status = CALL_LIMIT;
    }
    return;
  }
  uint64_t deadline_ns = start + CALL_BUDGET_NS;
  for (size_t i = 0; i < options->messages; ++i) {
    struct endpoint_packet reply;
    ++sample->attempted;
    sample->status = endpoint_invoke(caller, IPCBENCH_PROTOCOL, ECHO,
        payload + i * options->size, options->size, NULL, 0, deadline_ns, &reply);
    if (sample->status != CALL_OK) {
      sample->failed_delivery = reply.delivery;
      ++sample->failed_calls;
      break;
    }
    ++sample->admitted;
    ++sample->round_trips;
    sample->request_bytes += options->size;
    sample->reply_bytes += reply.size;
    bool valid = reply.result == RESULT_OK && reply.size == options->size && reply.grant_count == 0;
    if (valid) {
      memcpy(retained + i * options->size, reply.data, options->size);
    }
    if (!close_grants(&reply) || !valid) {
      sample->status = CALL_IO;
      break;
    }
  }
  if (timed) {
    enum call_status clock_status = clock_now(clock, &end);
    if (clock_status == CALL_OK && end >= start) {
      sample->elapsed_ns = end - start;
    } else if (sample->status == CALL_OK) {
      sample->status = clock_status == CALL_OK ? CALL_IO : clock_status;
    }
  }
  sample->consumed = sample->round_trips;
}

static void measure_send(const struct options *options, handle_t clock,
    handle_t control, handle_t data, const uint8_t *payload, bool timed, struct sample *sample)
{
  uint64_t start, end;
  sample->status = clock_now(clock, &start);
  if (sample->status != CALL_OK || start > UINT64_MAX - CALL_BUDGET_NS) {
    if (sample->status == CALL_OK) {
      sample->status = CALL_LIMIT;
    }
    return;
  }
  uint64_t deadline_ns = start + CALL_BUDGET_NS;
  while (sample->admitted < options->messages) {
    uint64_t admission_start, admission_end;
    if (timed) {
      sample->status = clock_now(clock, &admission_start);
      if (sample->status != CALL_OK) {
        break;
      }
    }
    size_t group = options->messages - sample->admitted;
    if (group > SEND_GROUP) {
      group = SEND_GROUP;
    }
    size_t admitted = 0;
    for (size_t i = 0; i < group; ++i) {
      ++sample->attempted;
      sample->status = endpoint_send(data, payload + sample->admitted * options->size,
          options->size, NULL, 0);
      if (sample->status != CALL_OK) {
        ++sample->rejected;
        break;
      }
      ++admitted;
      ++sample->admitted;
      sample->request_bytes += options->size;
    }
    if (timed) {
      enum call_status clock_status = clock_now(clock, &admission_end);
      if (clock_status == CALL_OK && admission_end >= admission_start) {
        sample->admission_ns += admission_end - admission_start;
      } else if (sample->status == CALL_OK) {
        sample->status = clock_status == CALL_OK ? CALL_IO : clock_status;
      }
    }
    if (admitted != 0) {
      struct command command = {.operation = DRAIN, .count = admitted};
      uint64_t consumed = 0;
      enum call_status drain_status = command_call(control, &command, deadline_ns, &consumed);
      if (drain_status == CALL_OK) {
        sample->consumed = consumed;
        if (consumed != sample->admitted && sample->status == CALL_OK) {
          sample->status = CALL_IO;
        }
      } else if (sample->status == CALL_OK) {
        sample->status = drain_status;
      }
    }
    if (sample->status != CALL_OK) {
      break;
    }
  }
  if (timed) {
    enum call_status clock_status = clock_now(clock, &end);
    if (clock_status == CALL_OK && end >= start) {
      sample->elapsed_ns = end - start;
    } else if (sample->status == CALL_OK) {
      sample->status = clock_status == CALL_OK ? CALL_IO : clock_status;
    }
  }
}

static bool run_pass(const struct options *options, handle_t clock,
    handle_t control, handle_t data, const uint8_t *payload, uint8_t *retained,
    bool timed, struct sample *sample)
{
  struct command command = {.operation = RESET};
  uint64_t count = 0;
  sample->status = untimed_command(clock, control, &command, &count);
  if (sample->status != CALL_OK || count != 0) {
    if (sample->status == CALL_OK) {
      sample->status = CALL_IO;
    }
    return false;
  }
  memset(retained, 0, options->size * options->messages);
  if (options->mode == MODE_CALL) {
    measure_call(options, clock, control, payload, retained, timed, sample);
  } else {
    measure_send(options, clock, control, data, payload, timed, sample);
  }
  if (timed && sample->status == CALL_OK && (sample->elapsed_ns == 0 ||
      (options->mode == MODE_SEND && sample->admission_ns == 0))) {
    sample->status = CALL_IO;
  }
  command = (struct command){.operation = VERIFY, .count = sample->consumed};
  enum call_status status = untimed_command(clock, control, &command, &count);
  sample->verified = status == CALL_OK && count == sample->consumed &&
      (options->mode == MODE_SEND || verify_payload(retained, options->size, sample->round_trips));
  if (!sample->verified && sample->status == CALL_OK) {
    sample->status = status == CALL_OK ? CALL_IO : status;
  }
  if (sample->status == CALL_OK && sample->consumed != options->messages) {
    sample->status = CALL_IO;
  }
  return sample->status == CALL_OK && sample->verified &&
      sample->consumed == options->messages && sample->rejected == 0;
}

static bool run_sample(const struct options *options, handle_t clock,
    handle_t memory, handle_t service, handle_t launcher, handle_t image,
    const uint8_t *payload, uint8_t *retained, bool timed, struct sample *sample)
{
  handle_t child = HANDLE_INVALID, control = HANDLE_INVALID, data = HANDLE_INVALID;
  struct endpoint_create_reply bootstrap = {0};
  bool ok = false;
  sample->cleanup = true;
  sample->status = endpoint_create(service, &bootstrap);
  if (sample->status != CALL_OK) {
    goto done;
  }
  sample->status = launch_receiver(launcher, image, memory, service, clock,
      bootstrap.caller, options->mode, &child);
  if (sample->status != CALL_OK) {
    goto done;
  }
  sample->status = receive_ready(bootstrap.receiver, options->mode, &control, &data);
  if (sample->status != CALL_OK) {
    goto done;
  }
  struct command command = {.operation = CONFIGURE, .size = options->size, .messages = options->messages};
  uint64_t count = 0;
  sample->status = untimed_command(clock, control, &command, &count);
  if (sample->status != CALL_OK || count != 0) {
    if (sample->status == CALL_OK) {
      sample->status = CALL_IO;
    }
    goto done;
  }
  ok = run_pass(options, clock, control, data, payload, retained, timed, sample);
done:
  if (control != HANDLE_INVALID) {
    struct command stop = {.operation = STOP};
    uint64_t count = 0;
    enum call_status stop_status = untimed_command(clock, control, &stop, &count);
    if (stop_status != CALL_OK) {
      fprintf(stderr, "ipcbench: stop failed (status %u); releasing control client\n", stop_status);
      sample->cleanup = false;
    }
  }
  if (!close_handle(&control)) {
    sample->cleanup = false;
  }
  if (!close_handle(&data)) {
    sample->cleanup = false;
  }
  if (!close_handle(&bootstrap.receiver)) {
    sample->cleanup = false;
  }
  if (!close_handle(&bootstrap.caller)) {
    sample->cleanup = false;
  }
  if (child != HANDLE_INVALID) {
    struct process_result result;
    enum call_status wait_status = process_wait(child, &result);
    if (wait_status != CALL_OK || result.kind != PROCESS_EXITED || result.exit_status != 0) {
      fprintf(stderr, "ipcbench: child cleanup failed (wait_status %u kind %llu exit %lld)\n",
          wait_status, (unsigned long long)result.kind, (long long)result.exit_status);
      sample->cleanup = false;
    }
  }
  if (!close_handle(&child)) {
    sample->cleanup = false;
  }
  if (!sample->cleanup && sample->status == CALL_OK) {
    sample->status = CALL_IO;
  }
  return ok && sample->cleanup;
}

static void report_sample(const struct options *options, const struct sample *sample,
    size_t round, bool successful)
{
  fprintf(stderr, "ipcbench %s %s %zu: %s; requested_messages=%zu attempted=%llu; "
      "status=%u verified=%s", options->mode == MODE_CALL ? "call" : "send",
      round == 0 ? "warmup" : "pass", round, successful ? "OK" : "FAILED",
      options->messages, (unsigned long long)sample->attempted,
      sample->status, sample->verified ? "yes" : "no");
  if (options->mode == MODE_CALL) {
    fprintf(stderr, "; round_trips=%llu failed_calls=%llu failed_call_delivery=%llu; "
        "confirmed_request_bytes=%llu reply_bytes=%llu",
        (unsigned long long)sample->round_trips, (unsigned long long)sample->failed_calls,
        (unsigned long long)sample->failed_delivery, (unsigned long long)sample->request_bytes,
        (unsigned long long)sample->reply_bytes);
    if (round != 0 && successful && sample->round_trips != 0) {
      fprintf(stderr, "; batch mean=%.3f ns/round_trip", (double)sample->elapsed_ns / sample->round_trips);
    }
  } else {
    fprintf(stderr, "; admitted=%llu rejected=%llu acknowledged_consumed=%llu; admitted_bytes=%llu acknowledged_bytes=%llu",
        (unsigned long long)sample->admitted, (unsigned long long)sample->rejected,
        (unsigned long long)sample->consumed, (unsigned long long)sample->request_bytes,
        (unsigned long long)(sample->consumed * options->size));
    if (round != 0) {
      fprintf(stderr, "; admission_intervals=%.3f ms (groups <=%u)", sample->admission_ns / 1000000.0, SEND_GROUP);
    }
    if (round != 0 && successful && sample->admitted != 0) {
      fprintf(stderr, "; admission batch mean=%.3f ns/send", (double)sample->admission_ns / sample->admitted);
    }
  }
  if (round == 0) {
    fputs("; untimed\n", stderr);
    return;
  }
  fprintf(stderr, "; completion_elapsed=%.3f ms", sample->elapsed_ns / 1000000.0);
  uint64_t bytes = sample->request_bytes + sample->reply_bytes;
  if (successful && bytes != 0 && sample->elapsed_ns != 0) {
    fprintf(stderr, "; application payload=%.3f MiB/s", bytes * 1000000000.0 /
        (sample->elapsed_ns * 1048576.0));
  }
  fputc('\n', stderr);
}

static void report_times(const char *interval, const struct options *options,
    const struct sample *samples, bool admission)
{
  uint64_t times[MAX_ROUNDS] = {0};
  for (size_t i = 0; i < options->rounds; ++i) {
    times[i] = admission ? samples[i + 1].admission_ns : samples[i + 1].elapsed_ns;
    size_t j = i;
    while (j != 0 && times[j - 1] > times[j]) {
      uint64_t temporary = times[j - 1];
      times[j - 1] = times[j];
      times[j] = temporary;
      --j;
    }
  }
  size_t middle = options->rounds / 2;
  double median = times[middle];
  if (options->rounds % 2 == 0) {
    median = times[middle - 1] / 2.0 + times[middle] / 2.0;
  }
  fprintf(stderr, "ipcbench %s summary: %zu verified samples; median=%.3f ms "
      "range=%.3f..%.3f ms\n", interval,
      options->rounds, median / 1000000.0, times[0] / 1000000.0,
      times[options->rounds - 1] / 1000000.0);
}

int main(int argc, char **argv)
{
  if (argc == 3 && !strcmp(argv[1], "--receiver")) {
    if (!strcmp(argv[2], "call")) {
      return receiver_main(MODE_CALL);
    }
    if (!strcmp(argv[2], "send")) {
      return receiver_main(MODE_SEND);
    }
    return 1;
  }
  struct options options;
  if (!parse_options(argc, argv, &options)) {
    fputs("usage: session boot://ipcbench.pxe call|send [--size 0..4096] "
        "[--messages 1..256] [--rounds 1..100]\n", stderr);
    return 1;
  }
  handle_t clock = startup_resource("clock");
  handle_t memory = startup_resource("memory");
  handle_t service = startup_resource("service");
  handle_t launcher = startup_resource("launcher");
  handle_t boot = startup_root("boot");
  handle_t image = HANDLE_INVALID;
  struct sample *samples = calloc(options.rounds + 1, sizeof(*samples));
  size_t bytes = options.size * options.messages;
  uint8_t *payload = malloc(bytes ? bytes : 1), *retained = malloc(bytes ? bytes : 1);
  enum call_status status = CALL_OK;
  bool ok = false, cleanup = true;
  size_t completed = 0;
  uint64_t clock_cost = 0;
  if (!samples || !payload || !retained) {
    status = CALL_NO_MEMORY;
    goto done;
  }
  if (clock == HANDLE_INVALID || memory == HANDLE_INVALID || service == HANDLE_INVALID ||
      launcher == HANDLE_INVALID || boot == HANDLE_INVALID) {
    fputs("ipcbench: missing clock, memory, endpoint service, launcher or boot root grant\n", stderr);
    status = CALL_BAD_HANDLE;
    goto done;
  }
  fill_payload(payload, options.size, options.messages);
  status = measure_clock(clock, &clock_cost);
  if (status != CALL_OK) {
    goto done;
  }
  status = directory_lookup(boot, "ipcbench.pxe", DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    goto done;
  }
  fprintf(stderr, "ipcbench: mode=%s size=%zu messages=%zu rounds=%zu; no payload attachments; "
      "one untimed warmup; fresh receiver per pass; clock-read batch mean=%llu ns (not subtracted)\n",
      options.mode == MODE_CALL ? "exported CALL echo" : "raw SEND", options.size,
      options.messages, options.rounds, (unsigned long long)clock_cost);
  fputs("ipcbench: child uses launching CPU and space; current CPU number unavailable in public ABI; "
      "startup, allocation, readiness, verification and cleanup outside intervals\n", stderr);
  for (size_t i = 0; i <= options.rounds; ++i) {
    bool pass = run_sample(&options, clock, memory, service, launcher, image,
        payload, retained, i != 0, &samples[i]);
    ++completed;
    if (!pass) {
      status = samples[i].status;
      cleanup = samples[i].cleanup;
      goto done;
    }
  }
  ok = true;
done:
  if (!close_handle(&image)) {
    cleanup = false;
  }
  for (size_t i = 0; i < completed; ++i) {
    bool successful = cleanup && samples[i].cleanup && samples[i].status == CALL_OK && samples[i].verified &&
        samples[i].consumed == options.messages && samples[i].rejected == 0;
    report_sample(&options, &samples[i], i, successful);
  }
  if (ok && cleanup) {
    report_times("completion", &options, samples, false);
    if (options.mode == MODE_SEND) {
      report_times("admission", &options, samples, true);
    }
    fputs("ipcbench: means are batch means, not latency percentiles\n", stderr);
  } else {
    fprintf(stderr, "ipcbench: FAILED (status %u cleanup=%s)\n", status, cleanup ? "yes" : "no");
  }
  free(payload);
  free(retained);
  free(samples);
  return ok && cleanup ? 0 : 1;
}
