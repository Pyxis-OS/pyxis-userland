#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

enum cxx_failure {
  CXX_CREATE = 1 << 0,
  CXX_COORDINATION = 1 << 1,
  CXX_TLS = 1 << 2,
  CXX_ERRNO = 1 << 3,
  CXX_EXCEPTION = 1 << 4,
  CXX_STATIC_GUARD = 1 << 5,
  CXX_ONCE = 1 << 6,
  CXX_ALLOCATION = 1 << 7,
  CXX_LANGUAGE_CLEANUP = 1 << 8,
  CXX_KEY_CLEANUP = 1 << 9,
  CXX_NOTIFY_EXIT = 1 << 10,
  CXX_UNEXPECTED = 1 << 11,
};

static constexpr unsigned cxx_worker_count = 2;
static constexpr unsigned cxx_exception_rounds = 16;
static constexpr int cxx_tls_initial = 0x746c7308;
static constexpr unsigned cxx_guard_value = 0x67756172;
static constexpr unsigned cxx_once_value = 0x6f6e6365;
static constexpr size_t cxx_heap_string_length = 256;
using cxx_strings = std::vector<std::string>;

struct cxx_exception {
  int owner;
  unsigned round;
};

struct cxx_runtime_state {
  std::atomic<unsigned> failures{0};
  std::atomic<bool> abort{false};
  std::atomic<unsigned> ready{0};
  std::atomic<unsigned> start{0};
  std::atomic<unsigned> guard_contenders{0};
  std::atomic<unsigned> guard_attempts{0};
  std::atomic<unsigned> guard_throws{0};
  std::atomic<unsigned> once_contenders{0};
  std::atomic<unsigned> once_attempts{0};
  std::atomic<unsigned> once_throws{0};
  std::atomic<unsigned> exception_ready[cxx_worker_count]{};
  std::atomic<unsigned> exception_release{0};
  std::atomic<unsigned> errno_ready[cxx_worker_count]{};
  std::atomic<unsigned> errno_release{0};
  std::atomic<cxx_strings *> objects[cxx_worker_count]{};
  std::atomic<unsigned> object_ready[cxx_worker_count]{};
  std::atomic<uintptr_t> tls_addresses[cxx_worker_count]{};
  std::atomic<uintptr_t> errno_addresses[cxx_worker_count]{};
  std::atomic<unsigned> language_cleanup[cxx_worker_count]{};
  std::atomic<unsigned> key_cleanup[cxx_worker_count]{};
  std::once_flag once;
  unsigned once_value = 0;
  pthread_key_t key = 0;
  std::mutex completion_mutex;
  std::condition_variable completion_condition;
  unsigned finished = 0;
};

/* Kept in shared storage for inspection with the matching debug ELF. */
static cxx_runtime_state cxx_runtime;
static thread_local int cxx_tls_word = cxx_tls_initial;

static void fail(unsigned failure)
{
  cxx_runtime.failures.fetch_or(failure, std::memory_order_relaxed);
}

static void abort_coordination()
{
  fail(CXX_COORDINATION);
  cxx_runtime.abort.store(true, std::memory_order_release);
  cxx_runtime.completion_condition.notify_all();
}

static bool wait_for(std::atomic<unsigned> &word, unsigned minimum)
{
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (word.load(std::memory_order_acquire) < minimum) {
    if (cxx_runtime.abort.load(std::memory_order_acquire)) {
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      abort_coordination();
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

struct cxx_tls_object {
  unsigned owner = cxx_worker_count;

  ~cxx_tls_object()
  {
    if (owner < cxx_worker_count) {
      if (cxx_tls_word != cxx_tls_initial + static_cast<int>(owner) + 1) {
        fail(CXX_TLS);
      }
      cxx_runtime.language_cleanup[owner].fetch_add(1, std::memory_order_release);
    }
  }
};

static thread_local cxx_tls_object cxx_language_object;

struct cxx_key_value {
  unsigned owner;
};

static cxx_key_value cxx_key_values[cxx_worker_count] = {{0}, {1}};

static void key_cleanup(void *argument)
{
  auto value = static_cast<cxx_key_value *>(argument);
  unsigned owner = value->owner;
  if (owner >= cxx_worker_count) {
    fail(CXX_KEY_CLEANUP);
    return;
  }
  if (pthread_getspecific(cxx_runtime.key) != nullptr ||
      cxx_runtime.language_cleanup[owner].load(std::memory_order_acquire) != 1 ||
      errno != 101 + static_cast<int>(owner)) {
    fail(CXX_KEY_CLEANUP);
  }
  cxx_runtime.key_cleanup[owner].fetch_add(1, std::memory_order_release);
}

struct cxx_guarded_object {
  unsigned value;

  cxx_guarded_object()
  {
    unsigned attempt = cxx_runtime.guard_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (attempt == 1) {
      if (!wait_for(cxx_runtime.guard_contenders, cxx_worker_count)) {
        throw cxx_exception{-1, 0};
      }
      /* Give the other caller an opportunity to reach the actual guard wait.
       * Enrollment is established by debugger inspection, not this delay. */
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      throw cxx_exception{-1, 0};
    }
    value = cxx_guard_value;
  }
};

static cxx_guarded_object &guarded_object()
{
  static cxx_guarded_object object;
  return object;
}

static void qualify_guard()
{
  cxx_runtime.guard_contenders.fetch_add(1, std::memory_order_release);
  try {
    if (guarded_object().value != cxx_guard_value) {
      fail(CXX_STATIC_GUARD);
    }
  } catch (const cxx_exception &exception) {
    if (exception.owner != -1 || exception.round) {
      fail(CXX_STATIC_GUARD);
    }
    cxx_runtime.guard_throws.fetch_add(1, std::memory_order_relaxed);
    if (guarded_object().value != cxx_guard_value) {
      fail(CXX_STATIC_GUARD);
    }
  }
}

static void initialize_once()
{
  unsigned attempt = cxx_runtime.once_attempts.fetch_add(1, std::memory_order_relaxed) + 1;
  if (attempt == 1) {
    if (!wait_for(cxx_runtime.once_contenders, cxx_worker_count)) {
      throw cxx_exception{-2, 0};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    throw cxx_exception{-2, 0};
  }
  cxx_runtime.once_value = cxx_once_value;
}

static void qualify_once()
{
  cxx_runtime.once_contenders.fetch_add(1, std::memory_order_release);
  try {
    std::call_once(cxx_runtime.once, initialize_once);
  } catch (const cxx_exception &exception) {
    if (exception.owner != -2 || exception.round) {
      fail(CXX_ONCE);
    }
    cxx_runtime.once_throws.fetch_add(1, std::memory_order_relaxed);
    std::call_once(cxx_runtime.once, initialize_once);
  }
  if (cxx_runtime.once_value != cxx_once_value) {
    fail(CXX_ONCE);
  }
}

static bool qualify_exceptions(unsigned owner)
{
  for (unsigned round = 1; round <= cxx_exception_rounds; round++) {
    try {
      throw cxx_exception{static_cast<int>(owner), round};
    } catch (const cxx_exception &outer) {
      if (outer.owner != static_cast<int>(owner) || outer.round != round) {
        fail(CXX_EXCEPTION);
      }
      cxx_runtime.exception_ready[owner].store(round, std::memory_order_release);
      if (!wait_for(cxx_runtime.exception_release, round)) {
        return false;
      }
      /* Both threads retain an active exception while the parent releases this
       * round. Rethrow must select this thread's own caught-exception stack. */
      try {
        throw;
      } catch (const cxx_exception &inner) {
        if (&inner != &outer || inner.owner != static_cast<int>(owner) ||
            inner.round != round || std::uncaught_exceptions()) {
          fail(CXX_EXCEPTION);
        }
      }
    }
  }
  return true;
}

static bool qualify_allocation(unsigned owner)
{
  cxx_strings *objects = nullptr;
  try {
    objects = new cxx_strings;
    objects->emplace_back(cxx_heap_string_length, static_cast<char>('A' + owner));
    objects->emplace_back("allocated by a different managed thread");
  } catch (...) {
    delete objects;
    objects = nullptr;
    fail(CXX_ALLOCATION);
  }
  cxx_runtime.objects[owner].store(objects, std::memory_order_release);
  cxx_runtime.object_ready[owner].store(1, std::memory_order_release);
  unsigned peer = 1 - owner;
  if (!wait_for(cxx_runtime.object_ready[peer], 1)) {
    return false;
  }
  objects = cxx_runtime.objects[peer].exchange(nullptr, std::memory_order_acq_rel);
  if (!objects || objects->size() != 2 || (*objects)[0].size() != cxx_heap_string_length ||
      (*objects)[0].find_first_not_of(static_cast<char>('A' + peer)) != std::string::npos ||
      (*objects)[1] != "allocated by a different managed thread") {
    fail(CXX_ALLOCATION);
  }
  delete objects;
  return true;
}

static void run_worker(unsigned owner)
{
  try {
    cxx_runtime.ready.fetch_add(1, std::memory_order_release);
    if (!wait_for(cxx_runtime.start, 1) || cxx_runtime.abort.load(std::memory_order_acquire)) {
      return;
    }
    if (cxx_tls_word != cxx_tls_initial) {
      fail(CXX_TLS);
    }
    cxx_language_object.owner = owner;
    cxx_tls_word += static_cast<int>(owner) + 1;
    cxx_runtime.tls_addresses[owner].store(reinterpret_cast<uintptr_t>(&cxx_tls_word),
        std::memory_order_release);
    cxx_runtime.errno_addresses[owner].store(reinterpret_cast<uintptr_t>(&errno),
        std::memory_order_release);
    if (pthread_setspecific(cxx_runtime.key, &cxx_key_values[owner])) {
      fail(CXX_KEY_CLEANUP);
    }
    qualify_guard();
    qualify_once();
    if (!qualify_exceptions(owner) || !qualify_allocation(owner)) {
      return;
    }
    errno = 101 + static_cast<int>(owner);
    cxx_runtime.errno_ready[owner].store(1, std::memory_order_release);
    if (!wait_for(cxx_runtime.errno_release, 1)) {
      return;
    }
    if (errno != 101 + static_cast<int>(owner)) {
      fail(CXX_ERRNO);
    }
    std::unique_lock<std::mutex> lock(cxx_runtime.completion_mutex);
    cxx_runtime.finished++;
    std::notify_all_at_thread_exit(cxx_runtime.completion_condition, std::move(lock));
  } catch (...) {
    fail(CXX_UNEXPECTED);
    abort_coordination();
  }
}

extern "C" int threadprobe_runtime_cxx(void)
{
  if (pthread_key_create(&cxx_runtime.key, key_cleanup)) {
    return CXX_KEY_CLEANUP;
  }
  std::thread workers[cxx_worker_count];
  unsigned created = 0;
  try {
    for (; created < cxx_worker_count; created++) {
      workers[created] = std::thread(run_worker, created);
    }
  } catch (...) {
    fail(CXX_CREATE);
    abort_coordination();
  }
  if (created == cxx_worker_count && wait_for(cxx_runtime.ready, cxx_worker_count)) {
    cxx_runtime.start.store(1, std::memory_order_release);
    for (unsigned round = 1; round <= cxx_exception_rounds; round++) {
      if (!wait_for(cxx_runtime.exception_ready[0], round) ||
          !wait_for(cxx_runtime.exception_ready[1], round)) {
        break;
      }
      cxx_runtime.exception_release.store(round, std::memory_order_release);
    }
    if (wait_for(cxx_runtime.errno_ready[0], 1) && wait_for(cxx_runtime.errno_ready[1], 1)) {
      cxx_runtime.errno_release.store(1, std::memory_order_release);
    }
    std::unique_lock<std::mutex> lock(cxx_runtime.completion_mutex);
    if (!cxx_runtime.completion_condition.wait_for(lock, std::chrono::seconds(5), [] {
        return cxx_runtime.finished == cxx_worker_count ||
            cxx_runtime.abort.load(std::memory_order_acquire);
      }) || cxx_runtime.finished != cxx_worker_count) {
      fail(CXX_NOTIFY_EXIT);
      abort_coordination();
    }
  } else {
    abort_coordination();
  }
  for (unsigned index = 0; index < created; index++) {
    workers[index].join();
  }
  for (unsigned index = 0; index < cxx_worker_count; index++) {
    delete cxx_runtime.objects[index].exchange(nullptr, std::memory_order_acq_rel);
  }
  if (pthread_key_delete(cxx_runtime.key)) {
    fail(CXX_KEY_CLEANUP);
  }
  if (created == cxx_worker_count) {
    uintptr_t tls[2], errors[2];
    for (unsigned index = 0; index < cxx_worker_count; index++) {
      tls[index] = cxx_runtime.tls_addresses[index].load(std::memory_order_acquire);
      errors[index] = cxx_runtime.errno_addresses[index].load(std::memory_order_acquire);
      if (cxx_runtime.language_cleanup[index].load(std::memory_order_acquire) != 1) {
        fail(CXX_LANGUAGE_CLEANUP);
      }
      if (cxx_runtime.key_cleanup[index].load(std::memory_order_acquire) != 1) {
        fail(CXX_KEY_CLEANUP);
      }
    }
    if (!tls[0] || !tls[1] || tls[0] == tls[1] || tls[0] == reinterpret_cast<uintptr_t>(&cxx_tls_word) ||
        tls[1] == reinterpret_cast<uintptr_t>(&cxx_tls_word) || cxx_tls_word != cxx_tls_initial) {
      fail(CXX_TLS);
    }
    if (!errors[0] || !errors[1] || errors[0] == errors[1] ||
        errors[0] == reinterpret_cast<uintptr_t>(&errno) || errors[1] == reinterpret_cast<uintptr_t>(&errno)) {
      fail(CXX_ERRNO);
    }
    if (cxx_runtime.guard_attempts.load(std::memory_order_relaxed) != 2 ||
        cxx_runtime.guard_throws.load(std::memory_order_relaxed) != 1) {
      fail(CXX_STATIC_GUARD);
    }
    if (cxx_runtime.once_attempts.load(std::memory_order_relaxed) != 2 ||
        cxx_runtime.once_throws.load(std::memory_order_relaxed) != 1 ||
        cxx_runtime.once_value != cxx_once_value) {
      fail(CXX_ONCE);
    }
  }
  return cxx_runtime.failures.load(std::memory_order_relaxed);
}
