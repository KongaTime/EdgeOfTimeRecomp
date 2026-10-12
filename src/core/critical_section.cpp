// core/critical_section.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <atomic>
#include <bit>
#include <cstdint>

#include <rex/hook.h>

#if defined(__cpp_lib_atomic_ref)
template <typename T> using AtomicRef = std::atomic_ref<T>;
#else
// The PS5 SDK's libc++ predates std::atomic_ref: what this file uses of it, on
// the compiler's atomic builtins
template <typename T> class AtomicRef {
public:
  explicit AtomicRef(T &value) : value_(&value) {}
  T load(std::memory_order order) const { return __atomic_load_n(value_, Order(order)); }
  T exchange(T desired, std::memory_order order) const {
    return __atomic_exchange_n(value_, desired, Order(order));
  }
  bool compare_exchange_weak(T &expected, T desired, std::memory_order order) const {
    return __atomic_compare_exchange_n(value_, &expected, desired, true, Order(order),
                                       order == std::memory_order_seq_cst ? __ATOMIC_SEQ_CST
                                                                          : __ATOMIC_ACQUIRE);
  }

private:
  static constexpr int Order(std::memory_order order) {
    switch (order) {
    case std::memory_order_relaxed: return __ATOMIC_RELAXED;
    case std::memory_order_acquire: return __ATOMIC_ACQUIRE;
    case std::memory_order_release: return __ATOMIC_RELEASE;
    case std::memory_order_acq_rel: return __ATOMIC_ACQ_REL;
    default: return __ATOMIC_SEQ_CST;
    }
  }
  T *value_;
};
#endif

#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_WorkBuf_Unlock);     // (section r3)
REX_EXTERN(__imp__eot_Semaphore_Signal);   // (semaphore r3, count r4, previous r5)

#if defined(__PROSPERO__)
#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include <rex/system/xthread.h>

#include "core/logging.h"

REX_EXTERN(__imp__eot_WorkBuf_Lock);

// While the game is brought up on the PS5: which thread holds which WorkBuf lock,
// since when, and what it had just called when it took it, for the stall dumps
// (DumpHeldWorkBufs). Its threads were found waiting on one of these locks with
// no owner in sight (run 7, 2026-10-12), and the fast path makes no kernel call
// that the runtime's own dumps would see.
namespace {
struct HeldWorkBuf {
  uint32_t owner = 0;
  std::string owner_name;
  std::string calls;
  std::chrono::steady_clock::time_point since;
};
std::mutex g_held_mutex;
std::map<uint32_t, HeldWorkBuf> g_held;
}  // namespace
#endif

namespace {

constexpr uint32_t kOwner = 0;
constexpr uint32_t kSemaphore = 8;
constexpr uint32_t kWaiters = 12;
constexpr uint32_t kRecursion = 16;

inline uint32_t Raw(uint32_t host_order) { return __builtin_bswap32(host_order); }

}

REX_HOOK_RAW(eot_WorkBuf_Unlock) {
  const uint32_t section = ctx.r3.u32;
  auto *owner = eot::mem::at<uint32_t>(section + kOwner);
  auto *waiters = eot::mem::at<uint32_t>(section + kWaiters);
  if (!owner || !waiters) {
    __imp__eot_WorkBuf_Unlock(ctx, base);
    return;
  }
  const uint32_t depth = eot::mem::load<uint32_t>(section + kRecursion) - 1;
  eot::mem::store<uint32_t>(section + kRecursion, depth);
  if (depth != 0)
    return;
#if defined(__PROSPERO__)
  {
    std::lock_guard lock(g_held_mutex);
    g_held.erase(section);
  }
#endif
  AtomicRef<uint32_t>(*owner).exchange(0, std::memory_order_seq_cst);
  AtomicRef<uint32_t> count(*waiters);
  uint32_t raw = count.load(std::memory_order_seq_cst);
  for (;;) {
    const int32_t n = static_cast<int32_t>(Raw(raw));
    if (n <= 0)
      return;
    if (count.compare_exchange_weak(raw, Raw(static_cast<uint32_t>(n - 1)),
                                    std::memory_order_seq_cst))
      break;
  }
  PPCContext call = ctx;
  call.r3.u32 = eot::mem::load<uint32_t>(section + kSemaphore);
  call.r4.u32 = 1;
  call.r5.u32 = 0;
  __imp__eot_Semaphore_Signal(call, base);
}

#if defined(__PROSPERO__)
REX_HOOK_RAW(eot_WorkBuf_Lock) {
  const uint32_t section = ctx.r3.u32;
  __imp__eot_WorkBuf_Lock(ctx, base);
  if (!(ctx.r3.u32 & 0xFF))
    return;
  auto *thread = rex::system::XThread::GetCurrentThread();
  std::lock_guard lock(g_held_mutex);
  auto [entry, added] = g_held.try_emplace(section);
  if (added || entry->second.owner != (thread ? thread->handle() : 0)) {
    entry->second.owner = thread ? thread->handle() : 0;
    entry->second.owner_name = thread ? thread->thread_name() : "(no thread)";
    entry->second.calls = thread ? thread->recent_calls() : std::string();
    entry->second.since = std::chrono::steady_clock::now();
  }
}

namespace eot::core {

void DumpHeldWorkBufs() {
  std::lock_guard lock(g_held_mutex);
  EOT_INFO("[workbuf] {} lock(s) held", g_held.size());
  const auto now = std::chrono::steady_clock::now();
  for (const auto &[section, held] : g_held) {
    EOT_INFO("[workbuf]   {:08X}: owner {:08X} '{}' for {} s, depth {}, waiters {}, semaphore {:08X}",
             section, held.owner, held.owner_name,
             std::chrono::duration_cast<std::chrono::seconds>(now - held.since).count(),
             eot::mem::load<uint32_t>(section + kRecursion),
             static_cast<int32_t>(eot::mem::load<uint32_t>(section + kWaiters)),
             eot::mem::load<uint32_t>(section + kSemaphore));
    EOT_INFO("[workbuf]     its calls before taking it: {}", held.calls);
  }
}

}  // namespace eot::core
#endif
