#include <console.h>
#include <errno.h>
#include <poll.h>
#include <process.h>
#include <pyxis/signal.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <wait.h>
#include "descriptor.h"
#include "errors.h"
#include "signal_internal.h"
#include "thread_platform.h"

#define SIGNAL_MASK ((1UL << (NSIG - 1)) - 1)
#define SUPPORTED_FLAGS (SA_RESTART | SA_RESETHAND | SA_NODEFER | SA_SIGINFO)
#define UNSUPPORTED_FLAGS (SA_NOCLDSTOP | SA_NOCLDWAIT | SA_ONSTACK)

/* Only the genuine initial thread accesses this state. Workers manipulate
 * caller-owned set values, but neither claim native events nor mutate actions. */
static struct sigaction actions[NSIG];
static sigset_t delivery_mask;
static unsigned long pending;
static int pending_code[NSIG];
static size_t inhibition;
static bool retired;
static handle_t interrupt_receiver;
static struct descriptor_signal_terminal terminal;
static bool terminal_override;
static struct descriptor_ticket override_ticket;

static int fail(int error)
{
  errno = error;
  return -1;
}

static bool signal_owner(void)
{
  return libc_thread_is_initial() && !retired;
}

static unsigned long signal_bit(int number)
{
  return 1UL << (number - 1);
}

static bool number_valid(int number)
{
  return number > 0 && number < NSIG;
}

static bool mask_valid(const sigset_t *set)
{
  return set && !(set->__opaque & ~SIGNAL_MASK);
}

static bool caught(const struct sigaction *action)
{
  return action->sa_handler != SIG_DFL && action->sa_handler != SIG_IGN;
}

static void discard_pending(int number)
{
  pending &= ~signal_bit(number);
  pending_code[number] = 0;
}

static void enqueue(int number, int code)
{
  if (actions[number].sa_handler == SIG_IGN ||
      (number == SIGWINCH && actions[number].sa_handler == SIG_DFL)) {
    return;
  }
  unsigned long bit = signal_bit(number);
  /* Coalescing retains the first pending event's actual origin. */
  if (!(pending & bit)) {
    pending |= bit;
    pending_code[number] = code;
  }
}

static void release_terminal(void)
{
  descriptor_signal_terminal_release(&terminal);
  if ((pending & signal_bit(SIGWINCH)) && pending_code[SIGWINCH] == SI_KERNEL) {
    discard_pending(SIGWINCH);
  }
}

static void release_receiver(void)
{
  handle_t receiver = interrupt_receiver;
  interrupt_receiver = HANDLE_INVALID;
  if (receiver != HANDLE_INVALID && descriptor_release_handle(receiver) < 0) {
    /* An uncertain release cannot be replayed or leave a hidden opt-in owner. */
    _Exit(EXIT_FAILURE);
  }
}

static int collect_sources(void)
{
  if (terminal.object) {
    if (!descriptor_signal_terminal_valid(&terminal)) {
      release_terminal();
    } else {
      struct wait_interest interest = {
        .handle = terminal.handle, .events = WAIT_RESIZED,
        .observed_generation = terminal.observed_generation,
      };
      uint64_t events;
      enum call_status status = wait_many(&interest, 1, 0, &events);
      if (status == CALL_BAD_HANDLE || status == CALL_ENDPOINT_CLOSED ||
          status == CALL_DENIED || status == CALL_UNAVAILABLE ||
          (status == CALL_OK && (events & WAIT_ERROR))) {
        /* SIZE can still succeed after hangup; readiness ends this binding. */
        release_terminal();
      } else if (status != CALL_OK) {
        return fail(libc_call_errno(status));
      } else if (events & WAIT_RESIZED) {
        struct console_size_reply size;
        status = console_size(terminal.handle, &size);
        if (!descriptor_signal_terminal_valid(&terminal) || status == CALL_BAD_HANDLE ||
            status == CALL_ENDPOINT_CLOSED || status == CALL_DENIED || status == CALL_UNAVAILABLE) {
          release_terminal();
        } else if (status != CALL_OK) {
          return fail(libc_call_errno(status));
        } else {
          terminal.observed_generation = size.generation;
          enqueue(SIGWINCH, SI_KERNEL);
        }
      }
    }
  }
  if (interrupt_receiver != HANDLE_INVALID) {
    enum call_status status = process_interrupt_take(interrupt_receiver);
    if (status == CALL_OK) {
      enqueue(SIGINT, SI_KERNEL);
    } else if (status != CALL_WOULD_BLOCK) {
      return fail(libc_call_errno(status));
    }
  }
  return 0;
}

static unsigned long eligible(void)
{
  return pending & ~delivery_mask.__opaque;
}

int libc_signal_prepare(struct libc_signal_wait *wait)
{
  if (!wait) {
    return fail(EINVAL);
  }
  *wait = (struct libc_signal_wait){0};
  if (!signal_owner()) {
    return 0;
  }
  int saved = errno;
  if (collect_sources() < 0) {
    return -1;
  }
  wait->pending = eligible() != 0;
  if (terminal.object && caught(&actions[SIGWINCH]) && !(pending & signal_bit(SIGWINCH))) {
    wait->interests[wait->count++] = (struct wait_interest){
      .handle = terminal.handle, .events = WAIT_RESIZED,
      .observed_generation = terminal.observed_generation,
    };
  }
  if (interrupt_receiver != HANDLE_INVALID && caught(&actions[SIGINT]) &&
      !(pending & signal_bit(SIGINT))) {
    wait->interests[wait->count++] = (struct wait_interest){
      .handle = interrupt_receiver, .events = WAIT_READABLE,
    };
  }
  errno = saved;
  return 0;
}

void libc_signal_enter(void)
{
  if (signal_owner()) {
    ++inhibition;
  }
}

void libc_signal_leave(void)
{
  if (signal_owner()) {
    --inhibition;
  }
}

bool libc_signal_admitted(void)
{
  return signal_owner() && inhibition != 0;
}

static void reset_action(int number)
{
  actions[number] = (struct sigaction){0};
  discard_pending(number);
  if (number == SIGINT) {
    release_receiver();
  } else if (number == SIGWINCH) {
    release_terminal();
  }
}

struct libc_signal_delivery libc_signal_checkpoint(void)
{
  struct libc_signal_delivery delivered = {0};
  if (!signal_owner() || inhibition) {
    return delivered;
  }
  int saved = errno;
  /* Source failures are reported by prepare/sigpending, not substituted for
   * a completed operation. Already queued events still have real provenance. */
  (void)collect_sources();
  unsigned long snapshot = eligible();
  bool restart = true;
  for (int number = 1; number < NSIG; ++number) {
    unsigned long bit = signal_bit(number);
    if (!(snapshot & pending & bit) || (delivery_mask.__opaque & bit)) {
      continue;
    }
    struct sigaction action = actions[number];
    int code = pending_code[number];
    discard_pending(number);
    if (action.sa_handler == SIG_IGN) {
      continue;
    }
    if (action.sa_handler == SIG_DFL) {
      if (number == SIGINT) {
        _Exit(130);
      }
      continue;
    }
    if ((unsigned)action.sa_flags & SA_RESETHAND) {
      reset_action(number);
    }
    sigset_t previous = delivery_mask;
    delivery_mask.__opaque |= action.sa_mask.__opaque;
    if (!((unsigned)action.sa_flags & SA_NODEFER)) {
      delivery_mask.__opaque |= bit;
    }
    delivered.caught = true;
    restart &= ((unsigned)action.sa_flags & SA_RESTART) != 0;
    if ((unsigned)action.sa_flags & SA_SIGINFO) {
      siginfo_t information = {.si_signo = number, .si_code = code};
      action.sa_sigaction(number, &information, NULL);
    } else {
      action.sa_handler(number);
    }
    delivery_mask = previous;
    errno = saved;
  }
  delivered.restart = delivered.caught && restart;
  errno = saved;
  return delivered;
}

int libc_signal_mask_install(const sigset_t *temporary, sigset_t *previous)
{
  if (!previous) {
    return fail(EINVAL);
  }
  if (!signal_owner()) {
    if (temporary) {
      return fail(ENOTSUP);
    }
    *previous = (sigset_t){0};
    return 0;
  }
  if (temporary && !mask_valid(temporary)) {
    return fail(EINVAL);
  }
  sigset_t value = temporary ? *temporary : delivery_mask;
  *previous = delivery_mask;
  delivery_mask = value;
  return 0;
}

void libc_signal_mask_restore(const sigset_t *previous)
{
  if (signal_owner()) {
    delivery_mask = *previous;
  }
}

void libc_signal_retire(void)
{
  if (!signal_owner()) {
    return;
  }
  retired = true;
  release_terminal();
  release_receiver();
  pending = 0;
  delivery_mask = (sigset_t){0};
}

static int select_terminal(struct descriptor_signal_terminal *selected)
{
  if (terminal_override) {
    if (!descriptor_ticket_valid(&override_ticket)) {
      return fail(EBADF);
    }
    if (descriptor_signal_terminal_capture(override_ticket.descriptor, selected) < 0) {
      return -1;
    }
    if (selected->generation != override_ticket.generation) {
      descriptor_signal_terminal_release(selected);
      return fail(EBADF);
    }
    return 0;
  }
  static const int candidates[] = {STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO};
  for (size_t index = 0; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
    if (descriptor_signal_terminal_capture(candidates[index], selected) == 0) {
      return 0;
    }
    if (errno != EBADF && errno != ENOTTY) {
      return -1;
    }
  }
  return fail(ENOTTY);
}

int pyxis_signal_set_terminal(int descriptor)
{
  if (!signal_owner()) {
    return fail(ENOTSUP);
  }
  struct descriptor_signal_terminal selected = {0};
  if (descriptor_signal_terminal_capture(descriptor, &selected) < 0) {
    return -1;
  }
  struct descriptor_ticket ticket = {selected.descriptor, selected.generation};
  release_terminal();
  override_ticket = ticket;
  terminal_override = true;
  if (caught(&actions[SIGWINCH])) {
    terminal = selected;
  } else {
    descriptor_signal_terminal_release(&selected);
  }
  libc_signal_checkpoint();
  return 0;
}

int sigaction(int number, const struct sigaction *restrict action,
    struct sigaction *restrict previous)
{
  if (!number_valid(number)) {
    return fail(EINVAL);
  }
  if (!signal_owner()) {
    return fail(ENOTSUP);
  }
  if (number != SIGWINCH && number != SIGINT && number != SIGPIPE) {
    return fail(ENOTSUP);
  }
  if (!action) {
    if (previous) {
      *previous = actions[number];
    }
    return 0;
  }
  struct sigaction value = *action;
  unsigned flags = (unsigned)value.sa_flags;
  if (!mask_valid(&value.sa_mask) || value.sa_handler == SIG_ERR ||
      (flags & ~(SUPPORTED_FLAGS | UNSUPPORTED_FLAGS))) {
    return fail(EINVAL);
  }
  if (flags & UNSUPPORTED_FLAGS) {
    return fail(ENOTSUP);
  }
  if (number == SIGPIPE && caught(&value)) {
    return fail(ENOTSUP);
  }
  struct descriptor_signal_terminal selected = {0};
  if (number == SIGWINCH && caught(&value) && select_terminal(&selected) < 0) {
    return -1;
  }
  handle_t receiver = HANDLE_INVALID;
  if (number == SIGINT && value.sa_handler != SIG_DFL && interrupt_receiver == HANDLE_INVALID) {
    enum call_status status = process_interrupt_receiver_create(&receiver);
    if (status != CALL_OK) {
      return fail(libc_call_errno(status));
    }
  }
  if (number == SIGINT && actions[number].sa_handler == SIG_IGN &&
      caught(&value) && interrupt_receiver != HANDLE_INVALID) {
    enum call_status status = process_interrupt_take(interrupt_receiver);
    if (status != CALL_OK && status != CALL_WOULD_BLOCK) {
      return fail(libc_call_errno(status));
    }
  }
  struct sigaction old = actions[number];
  if (number == SIGINT) {
    if (receiver != HANDLE_INVALID) {
      interrupt_receiver = receiver;
    }
    if (value.sa_handler == SIG_DFL) {
      release_receiver();
    }
  } else if (number == SIGWINCH) {
    bool same = selected.object && terminal.object == selected.object &&
        terminal.descriptor == selected.descriptor && terminal.generation == selected.generation;
    if (same) {
      selected.observed_generation = terminal.observed_generation;
      descriptor_signal_terminal_release(&terminal);
    } else {
      release_terminal();
    }
    terminal = selected;
  }
  actions[number] = value;
  if (value.sa_handler == SIG_IGN || value.sa_handler == SIG_DFL) {
    discard_pending(number);
  }
  if (previous) {
    *previous = old;
  }
  libc_signal_checkpoint();
  return 0;
}

void (*signal(int number, void (*handler)(int)))(int)
{
  struct sigaction action = {.sa_handler = handler, .sa_flags = SA_RESTART};
  struct sigaction previous;
  if (sigaction(number, &action, &previous) < 0) {
    return SIG_ERR;
  }
  return previous.sa_handler;
}

int raise(int number)
{
  if (!number_valid(number)) {
    return fail(EINVAL);
  }
  if (!signal_owner() || (number != SIGINT && number != SIGWINCH)) {
    return fail(ENOTSUP);
  }
  enqueue(number, SI_USER);
  libc_signal_checkpoint();
  return 0;
}

int sigemptyset(sigset_t *set)
{
  if (!set) {
    return fail(EINVAL);
  }
  *set = (sigset_t){0};
  return 0;
}

int sigfillset(sigset_t *set)
{
  if (!set) {
    return fail(EINVAL);
  }
  *set = (sigset_t){SIGNAL_MASK};
  return 0;
}

int sigaddset(sigset_t *set, int number)
{
  if (!mask_valid(set) || !number_valid(number)) {
    return fail(EINVAL);
  }
  set->__opaque |= signal_bit(number);
  return 0;
}

int sigdelset(sigset_t *set, int number)
{
  if (!mask_valid(set) || !number_valid(number)) {
    return fail(EINVAL);
  }
  set->__opaque &= ~signal_bit(number);
  return 0;
}

int sigismember(const sigset_t *set, int number)
{
  if (!mask_valid(set) || !number_valid(number)) {
    return fail(EINVAL);
  }
  return (set->__opaque & signal_bit(number)) != 0;
}

int sigprocmask(int how, const sigset_t *restrict set, sigset_t *restrict previous)
{
  if (!signal_owner()) {
    return fail(ENOTSUP);
  }
  if (set && (!mask_valid(set) || (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK))) {
    return fail(EINVAL);
  }
  sigset_t value = delivery_mask;
  if (set) {
    if (how == SIG_BLOCK) {
      value.__opaque |= set->__opaque;
    } else if (how == SIG_UNBLOCK) {
      value.__opaque &= ~set->__opaque;
    } else {
      value = *set;
    }
  }
  sigset_t old = delivery_mask;
  delivery_mask = value;
  if (previous) {
    *previous = old;
  }
  libc_signal_checkpoint();
  return 0;
}

int sigpending(sigset_t *set)
{
  if (!set) {
    return fail(EINVAL);
  }
  if (!signal_owner()) {
    return fail(ENOTSUP);
  }
  if (collect_sources() < 0) {
    return -1;
  }
  *set = (sigset_t){pending};
  return 0;
}

int sigsuspend(const sigset_t *mask)
{
  if (!mask_valid(mask)) {
    return fail(EINVAL);
  }
  if (!signal_owner()) {
    return fail(ENOTSUP);
  }
  return ppoll(NULL, 0, NULL, mask);
}
