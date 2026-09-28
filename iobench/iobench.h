#ifndef IOBENCH_H
#define IOBENCH_H

#include <clock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIXTURE_BYTES (1024 * 1024)
#define MAX_BUFFER_BYTES 65536
#define MAX_ROUNDS 100
#define CLOCK_READS 1000

enum io_mode { IO_READ, IO_WRITE, IO_COPY, IO_PIPE };

struct options {
  enum io_mode mode;
  const char *source, *output;
  size_t buffer, rounds, bytes;
  bool prepared, sync;
};

void fill_fixture(unsigned char *bytes, size_t size, bool contrast);
bool verify_fixture(const unsigned char *bytes, size_t size);
bool measure_clock(handle_t clock, uint64_t *cost);
void print_summary(const char *name, uint64_t *times, size_t count, size_t bytes);
bool run_output(const struct options *options, unsigned char *scratch, handle_t clock);
bool run_pipe(const struct options *options, handle_t clock);
int pipe_worker(int argc, char **argv);

#endif
