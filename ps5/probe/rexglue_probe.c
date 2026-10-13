/*
 * rexglue_probe - what the ReXGlue runtime's PS5 port assumes, checked on a console.
 *
 * The PS5 build of the ReXGlue SDK compiles and every symbol it needs resolves
 * against the modules a title links (docs/PS5-PORTING.md, "Status"). What a
 * link cannot show is how the console behaves. This payload measures each
 * behaviour the port now relies on:
 *
 *   1. thread ids     pthread_getthreadid_np for the thread ids ReXGlue logs
 *   2. signal target  pthread_kill(SIGUSR1/SIGUSR2) runs the handler on the
 *                     chosen thread (ReXGlue's suspend and callback wakeups,
 *                     the macOS path, which the PS5 now takes)
 *   3. suspend        a thread parked in its signal handler stops, and resumes
 *   4. wakeup         SIGUSR2 cuts a sleeping thread's wait short
 *   5. TLS in handler a _Thread_local read inside a signal handler (emulated TLS)
 *   6. fibers         rex::thread::Fiber's PS5 stack switch (fiber_switch_x64.h, a
 *                     copy of rexglue-sdk's src/core/fiber_switch_x64.h): round
 *                     trips, callee-saved registers, stack alignment, each
 *                     fiber's own MXCSR. Then, for the record, makecontext/
 *                     swapcontext, which the first run showed never entering
 *                     the fiber (and the port no longer uses)
 *   7. faults         a write and a read to a protected page: which signal
 *                     (SIGSEGV or SIGBUS), si_addr, mc_rip, the write bit of
 *                     mc_err, and the handler running on its alternate stack
 *                     (rex::arch::ExceptionHandler's PS5 branch)
 *   8. FP state       MXCSR on a new thread (ReXGlue sets the guest's itself)
 *   9. realtime       whether SIGRTMIN+1 reaches a chosen thread at all
 *                     (last: the port does not need it)
 *
 * Every line goes to standard output and to /data/rexglue-probe.txt as it is
 * measured, so a crash keeps what came before it. Send it with the payload
 * SDK's loader: make test PS5_HOST=<console address>.
 *
 * Copyright (C) 2026 KongaTime
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <pthread.h>
#include <pthread_np.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <ps5platform/context.h>
#include <xmmintrin.h>

#include "fiber_switch_x64.h"

REX_FIBER_SWITCH_X64_DEFINE;

#define LOG_PATH "/data/rexglue-probe.txt"

static int failures;

static void say(const char *format, ...) {
  char line[512];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof line, format, args);
  va_end(args);
  printf("%s\n", line);
  fflush(stdout);
  FILE *file = fopen(LOG_PATH, "a");
  if (file) {
    fprintf(file, "%s\n", line);
    fclose(file);
  }
}

static void check(int ok, const char *what) {
  say("%s %s", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    failures++;
  }
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
}

static uint32_t read_mxcsr(void) {
  uint32_t value;
  __asm__ volatile("stmxcsr %0" : "=m"(value));
  return value;
}

/* ---- 2 to 5: signals aimed at one thread ---- */

static _Thread_local int tls_marker;
static pthread_t worker_thread;
static atomic_int worker_tid;
static atomic_int handler_ran_on_worker;
static atomic_int handler_tls_value;
static atomic_long worker_counter;
static atomic_int worker_stop;
static atomic_int worker_sleeping;
static atomic_int park_in_handler;
static sem_t resume_semaphore;
static double sleep_returned_after_ms = -1;

static void user_signal_handler(int signal, siginfo_t *info, void *context) {
  (void)info;
  (void)context;
  if (pthread_equal(pthread_self(), worker_thread)) {
    atomic_fetch_add(&handler_ran_on_worker, 1);
  }
  atomic_store(&handler_tls_value, tls_marker);
  /* ReXGlue's kThreadSuspend handler parks the thread until it is resumed */
  if (signal == SIGUSR1 && atomic_load(&park_in_handler)) {
    while (sem_wait(&resume_semaphore) == -1 && errno == EINTR) {
    }
  }
}

static void *worker_main(void *unused) {
  (void)unused;
  tls_marker = 0x5a5a;
  atomic_store(&worker_tid, pthread_getthreadid_np());
  pthread_set_name_np(pthread_self(), "probe-worker");
  while (!atomic_load(&worker_stop)) {
    atomic_fetch_add(&worker_counter, 1);
    if (atomic_load(&worker_sleeping) == 1) {
      /* 4: a long sleep that SIGUSR2 should cut short */
      double start = now_ms();
      struct timespec ts = {2, 0};
      nanosleep(&ts, NULL);
      sleep_returned_after_ms = now_ms() - start;
      atomic_store(&worker_sleeping, 2);
    }
  }
  return NULL;
}

static void install(int signal) {
  struct sigaction action;
  memset(&action, 0, sizeof action);
  action.sa_sigaction = user_signal_handler;
  action.sa_flags = SA_SIGINFO | SA_RESTART; /* as ReXGlue installs them */
  sigemptyset(&action.sa_mask);
  check(sigaction(signal, &action, NULL) == 0, "sigaction for a thread signal");
}

static void probe_thread_signals(void) {
  say("-- thread ids and signals");
  sem_init(&resume_semaphore, 0, 0);
  install(SIGUSR1);
  install(SIGUSR2);
  pthread_create(&worker_thread, NULL, worker_main, NULL);
  while (!atomic_load(&worker_tid)) {
    sleep_ms(1);
  }
  int main_tid = pthread_getthreadid_np();
  say("main thread id %d, worker thread id %d", main_tid, atomic_load(&worker_tid));
  check(main_tid > 0 && atomic_load(&worker_tid) > 0 && main_tid != atomic_load(&worker_tid),
        "1 pthread_getthreadid_np gives distinct ids");

  /* 2: the wakeup signal lands on the worker */
  atomic_store(&handler_ran_on_worker, 0);
  check(pthread_kill(worker_thread, SIGUSR2) == 0, "pthread_kill(SIGUSR2) accepted");
  sleep_ms(50);
  check(atomic_load(&handler_ran_on_worker) == 1, "2 SIGUSR2's handler ran on the chosen thread");
  check(atomic_load(&handler_tls_value) == 0x5a5a, "5 a _Thread_local reads right inside the handler");

  /* 3: suspend, as ReXGlue's kThreadSuspend does it */
  atomic_store(&park_in_handler, 1);
  atomic_store(&handler_ran_on_worker, 0);
  check(pthread_kill(worker_thread, SIGUSR1) == 0, "pthread_kill(SIGUSR1) accepted");
  sleep_ms(50);
  long parked_at = atomic_load(&worker_counter);
  sleep_ms(200);
  long parked_after = atomic_load(&worker_counter);
  check(atomic_load(&handler_ran_on_worker) == 1 && parked_at == parked_after,
        "3 a thread parked in its SIGUSR1 handler stops running");
  sem_post(&resume_semaphore);
  sleep_ms(50);
  check(atomic_load(&worker_counter) > parked_after, "3 it runs again once resumed");
  atomic_store(&park_in_handler, 0);

  /* 4: SIGUSR2 interrupts a sleep */
  atomic_store(&worker_sleeping, 1);
  sleep_ms(100);
  pthread_kill(worker_thread, SIGUSR2);
  for (int i = 0; i < 300 && atomic_load(&worker_sleeping) != 2; i++) {
    sleep_ms(10);
  }
  say("the worker's 2 s sleep returned after %.1f ms", sleep_returned_after_ms);
  check(sleep_returned_after_ms >= 0 && sleep_returned_after_ms < 1500,
        "4 SIGUSR2 cuts a sleeping thread's wait short");

  atomic_store(&worker_stop, 1);
  pthread_join(worker_thread, NULL);
}

/* ---- 6: fibers ---- */

static void *switch_main_sp, *switch_a_sp, *switch_b_sp;
static long switch_a_count, switch_b_count;
static int switch_a_aligned;
static float switch_a_sse;
static uint32_t switch_a_mxcsr;

__attribute__((noinline)) static float aligned_sum(const float *p) {
  __m128 v = _mm_load_ps(p); /* movaps: faults unless p is 16-byte aligned */
  v = _mm_add_ps(v, _mm_shuffle_ps(v, v, 0x4e));
  v = _mm_add_ss(v, _mm_shuffle_ps(v, v, 0xb1));
  return _mm_cvtss_f32(v);
}

static void switch_fiber_a(void) {
  switch_a_aligned = ((uintptr_t)__builtin_frame_address(0) & 15) == 0;
  _Alignas(16) float v[4] = {1, 2, 3, 4};
  switch_a_sse = aligned_sum(v);
  /* Round toward zero in this fiber only: main's MXCSR must come back intact */
  _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u);
  for (;;) {
    switch_a_count++;
    switch_a_mxcsr = _mm_getcsr();
    rex_fiber_switch_x64(&switch_a_sp, switch_b_sp);
  }
}

static void switch_fiber_b(void) {
  for (;;) {
    switch_b_count++;
    rex_fiber_switch_x64(&switch_b_sp, switch_main_sp);
  }
}

static ucontext_t fiber_main_context, fiber_context;
static long fiber_count;

static void fiber_entry(void) {
  for (;;) {
    fiber_count++;
    swapcontext(&fiber_context, &fiber_main_context);
  }
}

static void probe_fibers(void) {
  say("-- fibers");
  const size_t stack_size = 256 * 1024;
  void *stack_a = malloc(stack_size);
  void *stack_b = malloc(stack_size);
  switch_a_sp = rex_fiber_initial_sp_x64(stack_a, stack_size, switch_fiber_a);
  switch_b_sp = rex_fiber_initial_sp_x64(stack_b, stack_size, switch_fiber_b);
  uint32_t main_mxcsr = read_mxcsr();
  /* These live in callee-saved registers across the switches */
  long x = 7, y = 11, z = 13, acc = 0;
  const long rounds = 100000;
  double start = now_ms();
  for (long i = 0; i < rounds; i++) {
    rex_fiber_switch_x64(&switch_main_sp, switch_a_sp); /* main -> A -> B -> main */
    acc += x * y + z + i;
    x ^= i;
    y += 3;
    z -= 1;
  }
  double elapsed = now_ms() - start;
  long ex = 7, ey = 11, ez = 13, expected = 0;
  for (long i = 0; i < rounds; i++) {
    expected += ex * ey + ez + i;
    ex ^= i;
    ey += 3;
    ez -= 1;
  }
  say("%ld main->A->B->main round trips in %.1f ms (%.0f ns each); MXCSR in A 0x%x, in main 0x%x",
      rounds, elapsed, elapsed * 1e6 / rounds, switch_a_mxcsr, read_mxcsr());
  check(switch_a_count == rounds && switch_b_count == rounds, "6 the port's fiber switch round-trips");
  check(acc == expected, "6 callee-saved registers survive the switches");
  check(switch_a_aligned && switch_a_sse == 10.0f, "6 a new fiber's stack is 16-byte aligned (an SSE load works)");
  /* Compare the control bits only: the low six are sticky exception flags the
   * CPU sets by itself (main's own timing arithmetic sets the precision flag,
   * 0x20, between the two reads; the first console run tripped on exactly that) */
  const uint32_t control = ~0x3fu;
  check((read_mxcsr() & control) == (main_mxcsr & control) && (switch_a_mxcsr & 0x6000u) == 0x6000u,
        "6 each fiber keeps its own MXCSR");
  free(stack_a);
  free(stack_b);

  /* For the record: the console's makecontext/swapcontext, which the port does not use */
  void *stack = malloc(stack_size);
  getcontext(&fiber_context);
  fiber_context.uc_stack.ss_sp = stack;
  fiber_context.uc_stack.ss_size = stack_size;
  fiber_context.uc_link = NULL;
  makecontext(&fiber_context, fiber_entry, 0);
  int result = 0;
  for (int i = 0; i < 1000; i++) {
    result = swapcontext(&fiber_main_context, &fiber_context);
  }
  say("(not used by the port) makecontext/swapcontext: entered the fiber %ld times in 1000, "
      "last swapcontext returned %d (errno %d)", fiber_count, result, result ? errno : 0);
  free(stack);
}

/* ---- 7: faults on a protected page ---- */

static volatile char *protected_page;
static size_t page_size;
static char *alt_stack;
static size_t alt_stack_size = 256 * 1024;
static volatile int fault_signal;
static volatile uintptr_t fault_address;
static volatile uintptr_t fault_rip;
static volatile long fault_err;
static volatile int fault_on_alt_stack;

static void fault_handler(int signal, siginfo_t *info, void *context) {
  ucontext_t *uc = (ucontext_t *)context;
  char marker;
  fault_signal = signal;
  fault_address = (uintptr_t)info->si_addr;
  fault_rip = (uintptr_t)uc->uc_mcontext.mc_rip;
  fault_err = (long)uc->uc_mcontext.mc_err;
  fault_on_alt_stack = (&marker >= alt_stack && &marker < alt_stack + alt_stack_size);
  /* Let the faulting instruction run again, as ReXGlue's handlers resume it */
  mprotect((void *)protected_page, page_size, PROT_READ | PROT_WRITE);
}

__attribute__((noinline)) static void fault_write(void) { protected_page[64] = 1; }
__attribute__((noinline)) static int fault_read(void) { return protected_page[128]; }

static void probe_faults(void) {
  say("-- faults");
  page_size = (size_t)getpagesize();
  say("getpagesize %zu, sysconf(_SC_PAGESIZE) %ld", page_size, sysconf(_SC_PAGESIZE));

  alt_stack = malloc(alt_stack_size);
  stack_t ss = {.ss_sp = alt_stack, .ss_size = alt_stack_size, .ss_flags = 0};
  check(sigaltstack(&ss, NULL) == 0, "sigaltstack");

  struct sigaction action;
  memset(&action, 0, sizeof action);
  action.sa_sigaction = fault_handler;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&action.sa_mask);
  check(sigaction(SIGSEGV, &action, NULL) == 0 && sigaction(SIGBUS, &action, NULL) == 0,
        "sigaction for SIGSEGV and SIGBUS");

  protected_page = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  check(protected_page != MAP_FAILED, "mmap of one page");
  if (protected_page == MAP_FAILED) {
    return;
  }

  check(mprotect((void *)protected_page, page_size, PROT_NONE) == 0, "mprotect to no access");
  fault_signal = 0;
  fault_write();
  say("write fault: signal %d (%s), si_addr %p (expected %p), mc_rip %p (fault_write at %p), mc_err 0x%lx",
      fault_signal, fault_signal == SIGSEGV ? "SIGSEGV" : fault_signal == SIGBUS ? "SIGBUS" : "?",
      (void *)fault_address, (void *)(protected_page + 64), (void *)fault_rip, (void *)fault_write,
      fault_err);
  check(fault_signal == SIGSEGV || fault_signal == SIGBUS, "7 a write to a protected page raises a signal");
  check(fault_address == (uintptr_t)(protected_page + 64), "7 si_addr is the faulting address");
  check(fault_rip >= (uintptr_t)fault_write && fault_rip < (uintptr_t)fault_write + 64,
        "7 mc_rip points into the faulting function");
  check((fault_err & 2) != 0, "7 mc_err has the write bit for a write");
  check(fault_on_alt_stack, "7 the handler ran on the alternate signal stack");
  check(protected_page[64] == 1, "7 the write completed after the handler returned");

  check(mprotect((void *)protected_page, page_size, PROT_NONE) == 0, "mprotect to no access again");
  fault_signal = 0;
  int value = fault_read();
  say("read fault: signal %d, si_addr %p, mc_err 0x%lx, value %d", fault_signal,
      (void *)fault_address, fault_err, value);
  check((fault_err & 2) == 0, "7 mc_err has no write bit for a read");
}

/* ---- 8: the floating-point state a thread starts with ---- */

static uint32_t thread_start_mxcsr;

static void *mxcsr_thread(void *unused) {
  (void)unused;
  thread_start_mxcsr = read_mxcsr();
  return NULL;
}

static void probe_fp_state(void) {
  say("-- floating point");
  pthread_t thread;
  pthread_create(&thread, NULL, mxcsr_thread, NULL);
  pthread_join(thread, NULL);
  say("MXCSR on the main thread 0x%x, on a new thread 0x%x (FTZ 0x8000, DAZ 0x40)", read_mxcsr(),
      thread_start_mxcsr);
}

/* ---- 9: realtime signals (informational) ---- */

static atomic_int realtime_on_worker;
static pthread_t realtime_worker;
static atomic_int realtime_stop;

static void realtime_handler(int signal, siginfo_t *info, void *context) {
  (void)signal;
  (void)info;
  (void)context;
  if (pthread_equal(pthread_self(), realtime_worker)) {
    atomic_store(&realtime_on_worker, 1);
  }
}

static void *realtime_main(void *unused) {
  (void)unused;
  while (!atomic_load(&realtime_stop)) {
    sleep_ms(1);
  }
  return NULL;
}

static void probe_realtime(void) {
  say("-- realtime signals (not needed by the port)");
  struct sigaction action;
  memset(&action, 0, sizeof action);
  action.sa_sigaction = realtime_handler;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  int installed = sigaction(SIGRTMIN + 1, &action, NULL) == 0;
  say("sigaction(SIGRTMIN+1 = %d): %s", SIGRTMIN + 1, installed ? "accepted" : strerror(errno));
  if (!installed) {
    return;
  }
  pthread_create(&realtime_worker, NULL, realtime_main, NULL);
  sleep_ms(20);
  int sent = pthread_kill(realtime_worker, SIGRTMIN + 1);
  sleep_ms(50);
  say("pthread_kill(SIGRTMIN+1): %d; handler ran on the chosen thread: %s", sent,
      atomic_load(&realtime_on_worker) ? "yes" : "no");
  atomic_store(&realtime_stop, 1);
  pthread_join(realtime_worker, NULL);
}

int main(void) {
  say("==== rexglue_probe ====");
  probe_thread_signals();
  probe_fibers();
  probe_faults();
  probe_fp_state();
  probe_realtime();
  say("==== %d failed ====", failures);
  return failures ? 1 : 0;
}
