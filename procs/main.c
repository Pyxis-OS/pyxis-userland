#include "../common/bootstrap_environment.h"
#include <inttypes.h>
#include <poll.h>
#include <process_view.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_MS UINT64_C(1000000)
#define NS_PER_SECOND UINT64_C(1000000000)
/* CPU % compares two listings this far apart. */
#define SAMPLE_INTERVAL_MS 250
#define LIVE_INTERVAL_MS 1000
#define KEY_CTRL_C 0x03

enum sort {
  SORT_TREE,
  SORT_CPU,
  SORT_MEM,
  SORT_NAME,
  SORT_AGE,
};

struct options {
  bool all, live;
  enum sort sort;
};

struct snapshot {
  void *buffer;
  struct process_list list;
};

static void usage(FILE *stream)
{
  fputs("usage: procs [--all] [--sort cpu|mem|name|age] [--live]\n"
        "  Lists this space's processes and execution groups.\n"
        "  --all   every space (needs the process_view grant)\n"
        "  --sort  a flat list ordered by CPU %, memory, name or age\n"
        "  --live  refresh every second; c m n a t change the order, q exits\n", stream);
}

static bool parse_sort(const char *text, enum sort *sort)
{
  static const struct {
    const char *name;
    enum sort sort;
  } sorts[] = {
    {"cpu", SORT_CPU}, {"mem", SORT_MEM}, {"name", SORT_NAME}, {"age", SORT_AGE},
  };
  for (size_t i = 0; i < sizeof(sorts) / sizeof(sorts[0]); ++i) {
    if (!strcmp(text, sorts[i].name)) {
      *sort = sorts[i].sort;
      return true;
    }
  }
  return false;
}

static enum call_status take(const struct options *options, handle_t view,
    struct snapshot *snapshot)
{
  return options->all ?
      process_list_all(view, snapshot->buffer, PROCESS_LIST_REPLY_MAX, &snapshot->list) :
      process_list_own_space(snapshot->buffer, PROCESS_LIST_REPLY_MAX, &snapshot->list);
}

static const struct process_list_record *find(const struct snapshot *snapshot, uint64_t key)
{
  if (!snapshot->list.header) {
    return NULL;
  }
  for (uint64_t i = 0; i < snapshot->list.header->returned; ++i) {
    if (snapshot->list.records[i].key == key) {
      return &snapshot->list.records[i];
    }
  }
  return NULL;
}

/* CPU % over the interval between the two listings, or -1 when unknown. */
static int cpu_percent(const struct snapshot *previous, const struct snapshot *current,
    const struct process_list_record *record)
{
  const struct process_list_record *before = find(previous, record->key);
  if (!before || (current->list.header->flags & PROCESS_LIST_CPU_UNAVAILABLE)) {
    return -1;
  }
  uint64_t elapsed = current->list.header->monotonic_ns - previous->list.header->monotonic_ns;
  uint64_t used = record->user_ns + record->kernel_ns - before->user_ns - before->kernel_ns;
  return elapsed ? (int)((used * 100 + elapsed / 2) / elapsed) : -1;
}

static void format_bytes(uint64_t bytes, char *text, size_t size)
{
  static const char units[] = "KMGT";
  uint64_t value = bytes / 1024;
  size_t unit = 0;
  while (value >= 10000 && unit + 1 < sizeof(units) - 1) {
    value /= 1024;
    ++unit;
  }
  snprintf(text, size, "%" PRIu64 "%c", value, units[unit]);
}

static void format_duration(uint64_t ns, char *text, size_t size)
{
  uint64_t seconds = ns / NS_PER_SECOND;
  if (seconds < 60) {
    snprintf(text, size, "%" PRIu64 ".%02" PRIu64 "s", seconds, ns % NS_PER_SECOND / (NS_PER_SECOND / 100));
  } else if (seconds < 3600) {
    snprintf(text, size, "%" PRIu64 "m%02" PRIu64 "s", seconds / 60, seconds % 60);
  } else if (seconds < 86400) {
    snprintf(text, size, "%" PRIu64 "h%02" PRIu64 "m", seconds / 3600, seconds / 60 % 60);
  } else {
    snprintf(text, size, "%" PRIu64 "d%02" PRIu64 "h", seconds / 86400, seconds / 3600 % 24);
  }
}

static const char *state_name(uint32_t state)
{
  switch (state) {
  case PROCESS_LIST_RUNNING:
    return "run";
  case PROCESS_LIST_WAITING:
    return "wait";
  case PROCESS_LIST_EXITING:
    return "exit";
  default:
    return "?";
  }
}

static void print_header(const struct options *options)
{
  printf("%s%8s %-4s %4s %5s %9s %7s %7s  %s\n", options->all ? "SPACE        " : "",
      "KEY", "STAT", "THR", "CPU%", "TIME", "MEM", "AGE", "NAME");
}

static void print_process(const struct options *options, const struct snapshot *previous,
    const struct snapshot *current, const struct process_list_record *record, unsigned indent)
{
  char cpu[8] = "-", time[16] = "-", memory[16], age[16];
  int percent = cpu_percent(previous, current, record);
  if (percent >= 0) {
    snprintf(cpu, sizeof(cpu), "%d", percent);
  }
  if (!(current->list.header->flags & PROCESS_LIST_CPU_UNAVAILABLE)) {
    format_duration(record->user_ns + record->kernel_ns, time, sizeof(time));
  }
  format_bytes(record->private_bytes, memory, sizeof(memory));
  uint64_t now = current->list.header->monotonic_ns;
  format_duration(now > record->start_ns ? now - record->start_ns : 0, age, sizeof(age));
  if (options->all) {
    printf("%-12.12s ", record->space);
  }
  printf("%8" PRIu64 " %-4s %4" PRIu32 " %5s %9s %7s %7s  %*s%s\n", record->key,
      state_name(record->state), record->threads, cpu, time, memory, age,
      (int)(indent * 2), "", record->label[0] ? record->label : "?");
}

static void print_group(const struct options *options, const struct process_list_record *group)
{
  if (options->all) {
    printf("%-12.12s ", group->space);
  }
  printf("%8" PRIu64 " %-4s %4s %5s %9s %7s %7s  %*sgroup, %" PRIu32 " member%s%s%s%s\n",
      group->key, "", "", "", "", "", "", (int)((group->depth - 1) * 2), "", group->threads,
      group->threads == 1 ? "" : "s",
      (group->state & PROCESS_LIST_GROUP_SEALED) ? ", sealed" : "",
      (group->state & PROCESS_LIST_GROUP_STOPPING) ? ", stopping" : "",
      (group->state & PROCESS_LIST_GROUP_COMPLETE) ? ", complete" : "");
}

static bool in_space(const struct process_list_record *record, const char *space)
{
  return !strncmp(record->space, space, sizeof(record->space));
}

/* One space: ungrouped processes first, then each group in the kernel's
 * preorder with its member processes beneath it. */
static void print_space_tree(const struct options *options, const struct snapshot *previous,
    const struct snapshot *current, const char *space)
{
  const struct process_list *list = &current->list;
  for (uint64_t i = 0; i < list->header->returned; ++i) {
    const struct process_list_record *record = &list->records[i];
    if (record->kind == PROCESS_LIST_PROCESS && !record->group_key && in_space(record, space)) {
      print_process(options, previous, current, record, 0);
    }
  }
  for (uint64_t i = 0; i < list->header->returned; ++i) {
    const struct process_list_record *group = &list->records[i];
    if (group->kind != PROCESS_LIST_GROUP || !in_space(group, space)) {
      continue;
    }
    print_group(options, group);
    for (uint64_t j = 0; j < list->header->returned; ++j) {
      const struct process_list_record *record = &list->records[j];
      if (record->kind == PROCESS_LIST_PROCESS && record->group_key == group->key) {
        print_process(options, previous, current, record, group->depth);
      }
    }
  }
}

/* Spaces in listing order; records of one space are contiguous. */
static void print_tree(const struct options *options, const struct snapshot *previous,
    const struct snapshot *current)
{
  const struct process_list *list = &current->list;
  for (uint64_t i = 0; i < list->header->returned; ++i) {
    if (!i || !in_space(&list->records[i - 1], list->records[i].space)) {
      print_space_tree(options, previous, current, list->records[i].space);
    }
  }
}

struct sort_context {
  enum sort sort;
  const struct snapshot *previous, *current;
};

static struct sort_context sort_context;

static int compare_rows(const void *left_pointer, const void *right_pointer)
{
  const struct process_list_record *left = *(const struct process_list_record *const *)left_pointer;
  const struct process_list_record *right = *(const struct process_list_record *const *)right_pointer;
  switch (sort_context.sort) {
  case SORT_CPU: {
    int a = cpu_percent(sort_context.previous, sort_context.current, left);
    int b = cpu_percent(sort_context.previous, sort_context.current, right);
    if (a != b) {
      return a > b ? -1 : 1;
    }
    uint64_t total_a = left->user_ns + left->kernel_ns, total_b = right->user_ns + right->kernel_ns;
    return total_a == total_b ? 0 : total_a > total_b ? -1 : 1;
  }
  case SORT_MEM:
    return left->private_bytes == right->private_bytes ? 0 :
        left->private_bytes > right->private_bytes ? -1 : 1;
  case SORT_NAME:
    return strcmp(left->label, right->label);
  case SORT_AGE:
  default:
    return left->start_ns == right->start_ns ? 0 : left->start_ns < right->start_ns ? -1 : 1;
  }
}

static void print_sorted(const struct options *options, enum sort sort,
    const struct snapshot *previous, const struct snapshot *current)
{
  const struct process_list *list = &current->list;
  const struct process_list_record **rows = calloc(list->header->returned + 1, sizeof(*rows));
  if (!rows) {
    fputs("procs: out of memory\n", stderr);
    return;
  }
  size_t count = 0;
  for (uint64_t i = 0; i < list->header->returned; ++i) {
    if (list->records[i].kind == PROCESS_LIST_PROCESS) {
      rows[count++] = &list->records[i];
    }
  }
  sort_context = (struct sort_context){sort, previous, current};
  qsort(rows, count, sizeof(*rows), compare_rows);
  for (size_t i = 0; i < count; ++i) {
    print_process(options, previous, current, rows[i], 0);
  }
  free(rows);
}

static void print_listing(const struct options *options, enum sort sort,
    const struct snapshot *previous, const struct snapshot *current)
{
  print_header(options);
  if (sort == SORT_TREE) {
    print_tree(options, previous, current);
  } else {
    print_sorted(options, sort, previous, current);
  }
  const struct process_list_header *header = current->list.header;
  if (header->flags & PROCESS_LIST_TRUNCATED) {
    printf("(truncated: %" PRIu64 " of %" PRIu64 " rows)\n", header->returned, header->total);
  }
}

static bool sleep_ms(unsigned milliseconds)
{
  struct timespec delay = {milliseconds / 1000, (long)(milliseconds % 1000) * (long)NS_PER_MS};
  return nanosleep(&delay, NULL) == 0;
}

/* Raw input, polled once per refresh; restores the terminal on every exit. */
static int run_live(const struct options *options, handle_t view, struct snapshot snapshots[2])
{
  struct termios saved, raw;
  if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved) != 0) {
    fputs("procs: --live needs a terminal\n", stderr);
    return EXIT_FAILURE;
  }
  raw = saved;
  cfmakeraw(&raw);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
    perror("procs: terminal");
    return EXIT_FAILURE;
  }
  enum sort sort = options->sort;
  size_t current = 0;
  int result = EXIT_SUCCESS;
  for (;;) {
    enum call_status status = take(options, view, &snapshots[current]);
    if (status != CALL_OK) {
      fprintf(stderr, "procs: cannot list processes (status %u)\r\n", status);
      result = EXIT_FAILURE;
      break;
    }
    fputs("\x1b[H\x1b[2J", stdout);
    print_listing(options, sort, &snapshots[current ^ 1], &snapshots[current]);
    fflush(stdout);
    current ^= 1;
    struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&input, 1, LIVE_INTERVAL_MS) <= 0) {
      continue;
    }
    char key;
    if (read(STDIN_FILENO, &key, 1) != 1 || key == 'q' || key == KEY_CTRL_C) {
      break;
    }
    if (key == 'c') {
      sort = SORT_CPU;
    } else if (key == 'm') {
      sort = SORT_MEM;
    } else if (key == 'n') {
      sort = SORT_NAME;
    } else if (key == 'a') {
      sort = SORT_AGE;
    } else if (key == 't') {
      sort = SORT_TREE;
    }
  }
  tcsetattr(STDIN_FILENO, TCSANOW, &saved);
  return result;
}

int main(int argc, char **argv)
{
  struct options options = {.sort = SORT_TREE};
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--all")) {
      options.all = true;
    } else if (!strcmp(argv[i], "--live")) {
      options.live = true;
    } else if (!strcmp(argv[i], "--sort") && i + 1 < argc && parse_sort(argv[i + 1], &options.sort)) {
      ++i;
    } else if (!strcmp(argv[i], "--help")) {
      usage(stdout);
      return EXIT_SUCCESS;
    } else {
      usage(stderr);
      return EXIT_FAILURE;
    }
  }
  handle_t view = HANDLE_INVALID;
  if (options.all) {
    view = bootstrap_capability("process_view");
    if (view == HANDLE_INVALID) {
      fputs("procs: --all needs the process_view grant\n", stderr);
      return EXIT_FAILURE;
    }
  }
  struct snapshot snapshots[2] = {
    {.buffer = malloc(PROCESS_LIST_REPLY_MAX)}, {.buffer = malloc(PROCESS_LIST_REPLY_MAX)},
  };
  if (!snapshots[0].buffer || !snapshots[1].buffer) {
    fputs("procs: out of memory\n", stderr);
    return EXIT_FAILURE;
  }
  if (options.live) {
    return run_live(&options, view, snapshots);
  }
  enum call_status status = take(&options, view, &snapshots[0]);
  if (status == CALL_OK) {
    if (!sleep_ms(SAMPLE_INTERVAL_MS)) {
      perror("procs: sleep");
      return EXIT_FAILURE;
    }
    status = take(&options, view, &snapshots[1]);
  }
  if (status != CALL_OK) {
    fprintf(stderr, "procs: cannot list processes (status %u)\n", status);
    return EXIT_FAILURE;
  }
  print_listing(&options, options.sort, &snapshots[0], &snapshots[1]);
  return fflush(stdout) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
