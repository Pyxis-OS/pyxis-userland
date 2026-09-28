#include "iobench.h"
#include "../common/directory.h"
#include <abi/clock.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <endpoint.h>
#include <errno.h>
#include <handle.h>
#include <launcher.h>
#include <pipe.h>
#include <process.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONTROL_TIMEOUT_NS (UINT64_C(30) * 1000000000)
enum worker_role { PRODUCER, CONSUMER, WORKERS };
enum control_operation { COMPLETE = 1, VERIFY, STOP };

struct ready {
  uint64_t role, prepared;
};

struct transfer {
  uint64_t bytes, calls, shorts;
  int error;
};

struct pipe_result {
  struct transfer producer, consumer;
  uint64_t accepted_ns, completed_ns;
  bool success, timed;
};

static bool close_owned(handle_t *handle)
{
  handle_t owned = *handle;
  *handle = HANDLE_INVALID;
  return owned == HANDLE_INVALID || handle_close(owned) == 0;
}

static bool discard_grants(struct endpoint_packet *packet)
{
  bool ok = true;
  for (size_t i = 0; i < packet->grant_count; ++i) {
    if (!close_owned(&packet->grants[i].handle)) {
      ok = false;
    }
  }
  return ok;
}

static bool deadline(handle_t clock, uint64_t *end)
{
  uint64_t now;
  if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - CONTROL_TIMEOUT_NS) {
    return false;
  }
  *end = now + CONTROL_TIMEOUT_NS;
  return true;
}

static bool control_call(handle_t client, uint64_t operation, uint64_t end,
    struct transfer *transfer)
{
  struct endpoint_packet reply = {0};
  enum call_status status = endpoint_request(client, &operation, sizeof(operation),
      NULL, 0, end, &reply);
  bool ok = status == CALL_OK && reply.result == 0 && reply.grant_count == 0 &&
      reply.size == (transfer ? sizeof(*transfer) : 0);
  if (ok && transfer) {
    memcpy(transfer, reply.data, sizeof(*transfer));
  }
  return discard_grants(&reply) && ok;
}

static void transfer_bytes(bool producer, size_t buffer, unsigned char *bytes,
    struct transfer *result)
{
  while (result->bytes < FIXTURE_BYTES) {
    size_t count = FIXTURE_BYTES - result->bytes;
    if (count > buffer) {
      count = buffer;
    }
    ++result->calls;
    ssize_t done = producer ? write(STDOUT_FILENO, bytes + result->bytes, count) :
        read(STDIN_FILENO, bytes + result->bytes, count);
    if (done <= 0) {
      result->error = done < 0 ? errno : EIO;
      break;
    }
    result->shorts += (size_t)done < count;
    result->bytes += (size_t)done;
  }
}

static bool consume(handle_t receiver, unsigned char *bytes, size_t buffer)
{
  struct transfer result = {0};
  transfer_bytes(false, buffer, bytes, &result);
  bool input_open = true, completed = false;
  if (result.error) {
    /* Release a blocked writer before waiting for its completion request. */
    close(STDIN_FILENO);
    input_open = false;
  }
  bool success = false;
  for (;;) {
    struct endpoint_packet packet = {0};
    if (endpoint_receive(receiver, &packet) != CALL_OK) {
      break;
    }
    if (packet.kind == ENDPOINT_MESSAGE_CANCEL) {
      endpoint_finish(packet.receipt);
      continue;
    }
    uint64_t operation = 0;
    bool valid = packet.kind == ENDPOINT_MESSAGE_CALL && packet.grant_count == 0 &&
        packet.size == sizeof(operation);
    if (valid) {
      memcpy(&operation, packet.data, sizeof(operation));
    }
    bool grants_closed = discard_grants(&packet);
    if (!valid || !grants_closed) {
      endpoint_finish(packet.receipt);
      break;
    }
    if (operation == COMPLETE && !completed) {
      completed = true;
      enum call_status status = endpoint_reply(packet.receipt, 0, &result,
          sizeof(result), NULL, 0);
      if (status != CALL_OK) {
        endpoint_finish(packet.receipt);
      }
      continue;
    }
    if (operation == VERIFY && completed && input_open) {
      /* The writer closes before the coordinator requests verification. */
      unsigned char extra;
      success = result.bytes == FIXTURE_BYTES && !result.error &&
          read(STDIN_FILENO, &extra, 1) == 0 && verify_fixture(bytes, FIXTURE_BYTES);
    }
    if (input_open) {
      if (close(STDIN_FILENO) < 0) {
        success = false;
      }
      input_open = false;
    }
    if (endpoint_reply(packet.receipt, success ? 0 : 1, NULL, 0, NULL, 0) != CALL_OK) {
      endpoint_finish(packet.receipt);
      success = false;
    }
    break;
  }
  if (input_open && close(STDIN_FILENO) < 0) {
    success = false;
  }
  return success;
}

static bool produce(handle_t coordinator, handle_t consumer, handle_t clock,
    unsigned char *bytes, size_t buffer, bool timed)
{
  struct pipe_result result = {.timed = timed};
  uint64_t end, start = 0, accepted = 0, completed = 0;
  bool ok = deadline(clock, &end);
  if (ok && timed) {
    ok = clock_now(clock, &start) == CALL_OK;
  }
  if (ok) {
    transfer_bytes(true, buffer, bytes, &result.producer);
    if (timed) {
      ok = clock_now(clock, &accepted) == CALL_OK;
    }
  }
  /* Closing early is necessary to release a reader after partial failure. */
  bool output_open = true;
  if (!ok || result.producer.error) {
    close(STDOUT_FILENO);
    output_open = false;
  }
  bool acknowledged = ok && control_call(consumer, COMPLETE, end, &result.consumer);
  if (acknowledged && timed) {
    ok = clock_now(clock, &completed) == CALL_OK;
  }
  if (output_open && close(STDOUT_FILENO) < 0) {
    ok = false;
  }
  if (timed && accepted > start && completed >= accepted) {
    result.accepted_ns = accepted - start;
    result.completed_ns = completed - start;
  } else if (timed) {
    ok = false;
  }
  result.success = ok && acknowledged && !result.producer.error && !result.consumer.error &&
      result.producer.bytes == FIXTURE_BYTES && result.consumer.bytes == FIXTURE_BYTES;
  if (!close_owned(&consumer)) {
    result.success = false;
  }
  struct endpoint_packet reply = {0};
  if (!deadline(clock, &end)) {
    return false;
  }
  enum call_status status = endpoint_request(coordinator, &result, sizeof(result),
      NULL, 0, end, &reply);
  bool success = status == CALL_OK && reply.result == 0 && reply.size == 0 &&
      reply.grant_count == 0 && result.success;
  return discard_grants(&reply) && success;
}

int pipe_worker(int argc, char **argv)
{
  if (argc != 5 || (strcmp(argv[2], "producer") && strcmp(argv[2], "consumer")) ||
      (strcmp(argv[4], "timed") && strcmp(argv[4], "warmup"))) {
    return EXIT_FAILURE;
  }
  size_t buffer = 0;
  for (const char *p = argv[3]; *p; ++p) {
    if (*p < '0' || *p > '9') {
      return EXIT_FAILURE;
    }
    size_t digit = *p - '0';
    if (buffer > (MAX_BUFFER_BYTES - digit) / 10) {
      return EXIT_FAILURE;
    }
    buffer = buffer * 10 + digit;
  }
  if (!buffer) {
    return EXIT_FAILURE;
  }
  bool producer = !strcmp(argv[2], "producer");
  handle_t coordinator = startup_resource("coordinator");
  handle_t clock = startup_resource("clock");
  struct endpoint_create_reply control = {0};
  unsigned char *bytes = malloc(FIXTURE_BYTES);
  bool prepared = bytes != NULL;
  if (prepared) {
    fill_fixture(bytes, FIXTURE_BYTES, !producer);
  }
  if (!producer && endpoint_create(startup_resource("service"), &control) != CALL_OK) {
    prepared = false;
  }
  struct ready ready = {.role = producer ? PRODUCER : CONSUMER, .prepared = prepared};
  struct endpoint_grant grant = {control.caller, 0, HANDLE_TRANSPORT_CALL};
  struct endpoint_packet reply = {0};
  uint64_t end;
  bool ok = deadline(clock, &end) && endpoint_request(coordinator, &ready, sizeof(ready),
      control.caller ? &grant : NULL, control.caller ? 1 : 0, end, &reply) == CALL_OK;
  if (!close_owned(&control.caller)) {
    ok = false;
  }
  handle_t consumer = HANDLE_INVALID;
  if (ok && prepared && reply.result == 0 && reply.size == 0 &&
      reply.grant_count == (producer ? 1U : 0U)) {
    if (producer) {
      consumer = reply.grants[0].handle;
      reply.grants[0].handle = HANDLE_INVALID;
    }
  } else {
    ok = false;
  }
  if (!discard_grants(&reply)) {
    ok = false;
  }
  if (ok) {
    ok = producer ? produce(coordinator, consumer, clock, bytes, buffer,
        !strcmp(argv[4], "timed")) : consume(control.receiver, bytes, buffer);
    consumer = HANDLE_INVALID;
  }
  if (!close_owned(&consumer)) {
    ok = false;
  }
  if (!close_owned(&control.receiver)) {
    ok = false;
  }
  free(bytes);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

static bool wait_worker(handle_t *child)
{
  if (*child == HANDLE_INVALID) {
    return true;
  }
  struct process_result result;
  bool ok = process_wait(*child, &result) == CALL_OK &&
      result.kind == PROCESS_EXITED && result.exit_status == 0;
  return close_owned(child) && ok;
}

static bool pipe_pass(const struct options *options, handle_t image, handle_t clock,
    bool timed, struct pipe_result *result)
{
  *result = (struct pipe_result){0};
  struct pipe_create_reply pipe = {0};
  struct endpoint_create_reply coordinator = {0};
  handle_t children[WORKERS] = {0}, consumer = HANDLE_INVALID;
  handle_t ready_receipts[WORKERS] = {0};
  struct endpoint_packet packet = {0};
  bool ok = false;
  enum call_status status = pipe_create(startup_resource("pipe"), &pipe);
  if (status != CALL_OK) {
    goto cleanup;
  }
  status = endpoint_create(startup_resource("service"), &coordinator);
  if (status != CALL_OK) {
    goto cleanup;
  }
  enum { MEMORY, CLOCK, COORDINATOR, STREAM, SERVICE, GRANTS };
  struct launch_grant grants[WORKERS][GRANTS];
  struct launch_binding resources[] = {
    {(uintptr_t)"memory", MEMORY}, {(uintptr_t)"clock", CLOCK},
    {(uintptr_t)"coordinator", COORDINATOR}, {(uintptr_t)"service", SERVICE},
  };
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%zu", options->buffer);
  const char *arguments[WORKERS][5] = {
    {"iobench.pxe", "--pipe-worker", "producer", buffer, timed ? "timed" : "warmup"},
    {"iobench.pxe", "--pipe-worker", "consumer", buffer, timed ? "timed" : "warmup"},
  };
  struct launch_request requests[WORKERS] = {0};
  for (size_t i = 0; i < WORKERS; ++i) {
    grants[i][MEMORY] = (struct launch_grant){startup_resource("memory"), MEMORY_RIGHT_MANAGE, 0};
    grants[i][CLOCK] = (struct launch_grant){clock, CLOCK_RIGHT_READ, 0};
    grants[i][COORDINATOR] = (struct launch_grant){coordinator.caller, 0, HANDLE_TRANSPORT_CALL};
    grants[i][STREAM] = (struct launch_grant){i == PRODUCER ? pipe.writer : pipe.reader,
        i == PRODUCER ? PIPE_RIGHT_WRITE : PIPE_RIGHT_READ, 0};
    grants[i][SERVICE] = (struct launch_grant){startup_resource("service"), ENDPOINT_SERVICE_RIGHT_CREATE, 0};
    requests[i] = (struct launch_request){
      .image = image, .grants = (uintptr_t)grants[i],
      .grant_count = i == CONSUMER ? GRANTS : GRANTS - 1,
      .resources = (uintptr_t)resources, .resource_count = i == CONSUMER ? 4 : 3,
      .argv = (uintptr_t)arguments[i], .argc = 5,
    };
    requests[i].streams[i == PRODUCER ? STARTUP_STDOUT : STARTUP_STDIN] =
        (struct launch_stream){PROTOCOL_PIPE, STREAM};
  }
  uint64_t failed;
  status = launcher_launch_batch(startup_resource("launcher"), requests, WORKERS,
      children, &failed);
  if (status != CALL_OK) {
    goto cleanup;
  }
  /* Only the workers retain stream endpoints: errors can now produce EOF/EPIPE. */
  bool closed = close_owned(&pipe.reader);
  closed = close_owned(&pipe.writer) && closed;
  if (!closed) {
    goto cleanup;
  }
  for (size_t i = 0; i < WORKERS; ++i) {
    status = endpoint_receive(coordinator.receiver, &packet);
    if (status != CALL_OK) {
      goto cleanup;
    }
    struct ready ready;
    if (packet.kind != ENDPOINT_MESSAGE_CALL || packet.size != sizeof(ready)) {
      goto cleanup;
    }
    memcpy(&ready, packet.data, sizeof(ready));
    if (ready.role >= WORKERS || !ready.prepared || ready_receipts[ready.role] ||
        packet.grant_count != (ready.role == CONSUMER ? 1U : 0U)) {
      goto cleanup;
    }
    ready_receipts[ready.role] = packet.receipt;
    packet.receipt = HANDLE_INVALID;
    if (ready.role == CONSUMER) {
      consumer = packet.grants[0].handle;
      packet.grants[0].handle = HANDLE_INVALID;
    }
    discard_grants(&packet);
    packet = (struct endpoint_packet){0};
  }
  status = endpoint_reply(ready_receipts[CONSUMER], 0, NULL, 0, NULL, 0);
  if (status != CALL_OK) {
    goto cleanup;
  }
  ready_receipts[CONSUMER] = HANDLE_INVALID;
  struct endpoint_grant grant = {consumer, 0, HANDLE_TRANSPORT_CALL};
  status = endpoint_reply(ready_receipts[PRODUCER], 0, NULL, 0, &grant, 1);
  if (status != CALL_OK) {
    goto cleanup;
  }
  ready_receipts[PRODUCER] = HANDLE_INVALID;
  status = endpoint_receive(coordinator.receiver, &packet);
  if (status != CALL_OK || packet.kind != ENDPOINT_MESSAGE_CALL ||
      packet.size != sizeof(*result) || packet.grant_count != 0) {
    goto cleanup;
  }
  memcpy(result, packet.data, sizeof(*result));
  uint64_t end;
  bool verified = deadline(clock, &end) && control_call(consumer, VERIFY, end, NULL);
  /* Retain the control grant through cleanup in case VERIFY was not delivered. */
  ok = verified && result->success && result->timed == timed;
  status = endpoint_reply(packet.receipt, ok ? 0 : 1, NULL, 0, NULL, 0);
  if (status == CALL_OK) {
    packet.receipt = HANDLE_INVALID;
  } else {
    ok = false;
  }
cleanup:
  if (status != CALL_OK) {
    fprintf(stderr, "iobench pipe: transport/setup failed (status %u)\n", status);
  }
  if (!discard_grants(&packet)) {
    ok = false;
  }
  if (!close_owned(&packet.receipt)) {
    ok = false;
  }
  for (size_t i = 0; i < WORKERS; ++i) {
    if (!close_owned(&ready_receipts[i])) {
      ok = false;
    }
  }
  if (!close_owned(&coordinator.receiver)) {
    ok = false;
  }
  if (!close_owned(&coordinator.caller)) {
    ok = false;
  }
  if (!close_owned(&pipe.reader)) {
    ok = false;
  }
  if (!close_owned(&pipe.writer)) {
    ok = false;
  }
  /* Release the producer before stopping a consumer that may still be reading. */
  if (!wait_worker(&children[PRODUCER])) {
    ok = false;
  }
  if (consumer != HANDLE_INVALID) {
    uint64_t end;
    if (deadline(clock, &end)) {
      control_call(consumer, STOP, end, NULL);
    }
    if (!close_owned(&consumer)) {
      ok = false;
    }
  }
  if (!wait_worker(&children[CONSUMER])) {
    ok = false;
  }
  return ok;
}

bool run_pipe(const struct options *options, handle_t clock)
{
  handle_t image = HANDLE_INVALID;
  enum call_status status = resolve_file("app://iobench.pxe", FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    fprintf(stderr, "iobench pipe: open worker image failed (status %u)\n", status);
    return false;
  }
  fputs("Pipe: coordinator and two workers on launching CPU/space; fresh pipe/workers per pass\n"
      "Acceptance: descriptor writes; completion: consumer CALL acknowledgment; verification outside timing\n",
      stderr);
  uint64_t accepted[MAX_ROUNDS], completed[MAX_ROUNDS];
  bool success = true;
  for (size_t pass = 0; success && pass <= options->rounds; ++pass) {
    struct pipe_result result;
    success = pipe_pass(options, image, clock, pass != 0, &result);
    fprintf(stderr, "%s %zu: %s; requested=%u written=%llu consumed=%llu bytes\n"
        "  write_calls=%llu short_writes=%llu read_calls=%llu short_reads=%llu errors=%d/%d\n",
        pass ? "sample" : "warmup", pass ? pass : 1, success ? "OK" : "FAILED",
        FIXTURE_BYTES, (unsigned long long)result.producer.bytes,
        (unsigned long long)result.consumer.bytes, (unsigned long long)result.producer.calls,
        (unsigned long long)result.producer.shorts, (unsigned long long)result.consumer.calls,
        (unsigned long long)result.consumer.shorts, result.producer.error, result.consumer.error);
    if (pass && result.accepted_ns && result.completed_ns) {
      fprintf(stderr, "  acceptance=%llu ns completion=%llu ns",
          (unsigned long long)result.accepted_ns, (unsigned long long)result.completed_ns);
      if (success) {
        fprintf(stderr, "; %.3f/%.3f MiB/s", 1000000000.0 / result.accepted_ns,
            1000000000.0 / result.completed_ns);
      }
      fputc('\n', stderr);
      accepted[pass - 1] = result.accepted_ns;
      completed[pass - 1] = result.completed_ns;
    }
  }
  if (!close_owned(&image)) {
    success = false;
  }
  if (success) {
    print_summary("pipe acceptance", accepted, options->rounds, FIXTURE_BYTES);
    print_summary("pipe completion", completed, options->rounds, FIXTURE_BYTES);
  }
  return success;
}
