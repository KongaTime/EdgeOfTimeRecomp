# Porting EdgeOfTimeRecompiled to the PS5 (homebrew)

> [!IMPORTANT]
> This is a plan, and the work is early: the ReXGlue runtime builds for the PS5 and its
> guest memory works on the console, but the game does not run yet ([Status](#status)). It covers an **unofficial homebrew title** for a jailbroken
> PS5, built on my PS5 homebrew stack. It is not a PlayStation Store release, and no
> game files are or will be included.

Written 2026-10-11 against these revisions:

| What | Revision |
| --- | --- |
| This repository (EdgeOfTimeRecomp) | `340144e` |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) | `c94f5eb` (v0.10.0, the version `reeot_manifest.toml` pins) |
| [KongaTime/rexglue-sdk](https://github.com/KongaTime/rexglue-sdk) (the PS5 fork) | `main` at `b84a414` |
| [KongaTime/PS5_EdgeOfTimeTitle](https://github.com/KongaTime/PS5_EdgeOfTimeTitle) (the title) | `main` at `6f90a64` |
| [plume](https://github.com/zolaware/plume) (submodule) | `de0f70f` |
| [PS5_RPCS3Title](https://github.com/KongaTime/PS5_RPCS3Title) | `620af91` (build 131) |
| [PS5_RPCS3](https://github.com/KongaTime/PS5_RPCS3) | `main` at `48f034b` |
| [PS5_VulkanTemplate](https://github.com/KongaTime/PS5_VulkanTemplate) | `f827f0a` |
| [PS5_PayloadSDK](https://github.com/KongaTime/PS5_PayloadSDK) | `1de8b37` |
| [PS5_Vulkan](https://github.com/KongaTime/PS5_Vulkan) | `5b5e4fc` |
| [prospero-win](https://github.com/KongaTime/prospero-win) | `650d4de` |

## Contents

- [Status](#status)
- [Why this is feasible](#why-this-is-feasible)
- [Step 0: the desktop build must show a picture](#step-0-the-desktop-build-must-show-a-picture)
- [Optional shortcut: the Windows build under prospero-win](#optional-shortcut-the-windows-build-under-prospero-win)
- [The native title](#the-native-title)
    - [Repositories](#repositories)
    - [1. Build](#1-build)
    - [2. Guest memory](#2-guest-memory)
    - [3. The fault handler](#3-the-fault-handler)
    - [4. Threads](#4-threads)
    - [5. Files](#5-files)
    - [6. Display, controller and sound (SDL3)](#6-display-controller-and-sound-sdl3)
    - [7. EdgeOfTimeRecomp's desktop code](#7-edgeoftimerecomps-desktop-code)
    - [8. The title shell](#8-the-title-shell)
- [Milestones](#milestones)
- [Risks and open questions](#risks-and-open-questions)
- [Licences and sharing](#licences-and-sharing)
- [First steps](#first-steps)

## Status

As of 2026-10-11. Everything here was built on a Linux PC; the probe and the memory test have run on the console, the runtime has not.

**Done**

- **The ReXGlue runtime compiles for the PS5.** The fork's commits on v0.10.0
  ([KongaTime/rexglue-sdk](https://github.com/KongaTime/rexglue-sdk/commits/main), `main`
  at `b84a414`) build `librexruntime.a` and its libraries (SDL3, FFmpeg, fmt, spdlog and the rest) with
  PS5_PayloadSDK `1de8b37` and clang 20.
  [`ps5/tools/build-rexglue.sh`](../ps5/tools/build-rexglue.sh) does it from a clean
  checkout.
- **Every symbol the runtime imports resolves** against the modules a title links by
  default and the SDK's static libraries. The script checks this after each build.
- **Unknowns resolved from the SDK's stub libraries** (what the console exports):

  | ReXGlue needs | On the console | What the fork does |
  | --- | --- | --- |
  | `pthread_sigqueue`, realtime signals | no `pthread_sigqueue`; `sigqueue` is FreeBSD's (aims at a process); `SIGRTMIN` is defined, delivery unproven | suspend and callback wakeups use `SIGUSR1`/`SIGUSR2` with `pthread_kill`, ReXGlue's macOS path |
  | `timer_create` with `SIGEV_THREAD_ID` | missing | not needed: ReXGlue uses it only on Linux, and its own timer queue elsewhere |
  | `syscall(SYS_gettid)` | missing | `pthread_getthreadid_np` (exported) |
  | `pthread_setname_np` / `pthread_getname_np` | only in a module a title does not load / undeclared | `pthread_set_name_np` (exported); the thread keeps its own name |
  | `pthread_setaffinity_np` with `cpu_set_t` | `ERANGE` for FreeBSD's 32-byte set | the platform layer's `ps5_pthread_{get,set}affinity_np` |
  | `getcontext` / `makecontext` / `swapcontext` | exported, but a made context is never entered (console run, below) | fibers are a stack switch of the fork's own (`src/core/fiber_switch_x64.h`) |
  | the machine context in a fault | FreeBSD's `mc_*`, 48 bytes later than FreeBSD says | the exception handler reads it through the SDK fork's `ucontext_t` |
  | floating-point `std::from_chars`, `clock_time_conversion`, `std::jthread` | missing from libc++ 18 (the last behind `-fexperimental-library`) | the macOS shims; `-fexperimental-library` |

- **A console probe**, [`ps5/probe/`](../ps5/probe/rexglue_probe.c), for what only
  hardware can answer. Two runs on the console (2026-10-11) showed:

  | Checked | On the console | What changed |
  | --- | --- | --- |
  | `pthread_kill` with `SIGUSR1`/`SIGUSR2` | reaches the chosen thread; a thread parks in its handler and resumes; `SIGUSR2` cut a 2 s sleep to 100 ms; emulated TLS reads right inside a handler | nothing: the fork's thread changes hold |
  | page faults | `SIGSEGV` for reads and writes; `si_addr`, `mc_rip` and `mc_err` (0x6 write, 0x4 read) right through the SDK fork's context; the handler on its alternate stack | `SIGBUS` left alone, as on Linux |
  | page size | 16 KiB (`getpagesize`, `sysconf`) | — |
  | a new thread's MXCSR | 0x9fc0: flush-to-zero and denormals-are-zero on | guest threads start with both off, as on desktop (`FPSCRRegister::InitHost`) |
  | `makecontext` + `swapcontext` | the fiber never ran: `swapcontext` rejects the made context (`EINVAL`) | fibers switch stacks themselves |
  | the fork's fiber switch (second run) | 100000 main→A→B→main round trips, 43 ns each; callee-saved registers intact; new stacks 16-byte aligned; each fiber keeps its own rounding mode | — |
  | realtime signals | `SIGRTMIN+1` reaches a chosen thread | not needed |


- **Guest memory in direct memory** (section 2): the fork's `src/core/memory_ps5.cpp`.
  Its test, `ps5/tests/memory_ps5_test.cpp` in the fork, lays the arena out as
  `Memory::Initialize` does and passes all 42 checks on PS5_PayloadSDK's host model of
  the kernel, failure injection included. **On the console, in the title, all 36 checks
  that can run there passed** (2026-10-11; the fork's
  [`ps5/tests/results/2026-10-11-title.txt`](https://github.com/KongaTime/rexglue-sdk/blob/main/ps5/tests/results/2026-10-11-title.txt)):

  | Checked | In the title |
  | --- | --- |
  | memory | 12288 MiB of direct memory, 12268 MiB of it in one free block; 417 MiB of flexible memory free |
  | the physical heap's 512 MiB commit | 25.5 ms, 256 direct-memory objects |
  | the nine views at `0x10_0000_0000` | map, and see the same memory through every alias |
  | protection per view | a page made read-only through one view faults on write there, and stays writable through the raw view |
  | no access on shared views (MMIO windows) | holds: such a page faults |
  | decommit, then recommit | takes the access away, then gives it back with the contents kept |
  | `sceKernelQueryMemoryProtection` on a view page | answers the whole 512 MiB view as one read-write range (`memory_ps5.cpp` keeps its own table) |

  Two earlier runs as a payload had no direct memory to work with
  (`sceKernelGetDirectMemorySize` answers 0 there, beside 8158 MiB of flexible memory),
  so the guest's memory has to live in a title, which is where the game runs.
- **The title**, [KongaTime/PS5_EdgeOfTimeTitle](https://github.com/KongaTime/PS5_EdgeOfTimeTitle)
  (`PPSA99300`), made with PS5_VulkanTemplate's `new-title.py` and pinned to
  PS5_PayloadSDK `1de8b37` (section 8). For now its program is the memory test: the
  fork's `memory_ps5.cpp` and its test, compiled into the title
  (`MEMORY_PS5_TEST_EMBEDDED`), writing `/app0/rexglue-memory-test.txt`. Built here with
  PS5_Vulkan `5b5e4fc`'s RADV (PS5_Mesa `7b59ef2`), installed in `/data/homebrew`, and
  launched from the home screen, it ran the test above.

**Not done**

- **The MMIO handler's instruction reads** (section 3): still read execute-only code.
- **SDL3's PS5 backends**, plume's volk start, this repository's own PS5 changes, and the
  title's real program, which starts the game (sections 6–8).
- **Step 0** (the desktop build with game files), and running the runtime itself on the
  console.

**Waiting on you**

- Nothing right now. M2 will need the game's files from your own copy (Step 0).

## Why this is feasible

EdgeOfTimeRecomp is easier to port than RPCS3:

- **No JIT.** The Xbox 360 code is recompiled to C++ on the PC ahead of time
  (`rexglue codegen`). The console only runs ordinary compiled x86-64 code. There's no
  LLVM to build for the console, no executable memory to manage and no first-boot
  compile. Those were the hardest parts of PS5_RPCS3.
- **Vulkan already.** The renderer goes through plume, which loads Vulkan with volk. On
  the console, volk points at the RADV that PS5_Vulkan links into the title, the same
  way PS5_RPCS3Title does it.
- **Same CPU family.** The PS5 is a Zen 2. The desktop build targets `x86-64-v2` with
  AVX, and the console supports all of that.

What's left is platform code, in three places:

1. **The ReXGlue SDK's runtime:** memory, threads, faults and files, all written for
   Linux, macOS and Windows.
2. **SDL3:** the window, controller and sound for ReXGlue, plume and this repository.
3. **This repository's desktop features:** the installer, file dialogs, desktop
   shortcuts, the update check and so on.

Most of these problems have already been solved somewhere in my stack. The sections
below name where.

## Step 0: the desktop build must show a picture

`docs/BUILDING.md` warns that the public repository "doesn't contain the native
renderer yet" and that a build "won't display a picture". `src/gpu/` now has about 14,000
lines of renderer code, so that warning may be out of date. Still, before any console
work:

- Build `linux-amd64-release` from this fork with my own game files, and confirm the
  game reaches gameplay with a picture.
- Keep that Linux build as the **reference**. Every console milestone below gets
  compared with it, the way `host-reference.sh` compares the template's frames.

If the Linux build doesn't draw, the console port can't draw either. Fix that first.

## Optional shortcut: the Windows build under prospero-win

prospero-win runs 64-bit Windows programs natively through Wine. Wine's Vulkan
(`winevulkan`) is wired to RADV (`docs/WINE_INTEGRATION.md` there). This repository can
build a Windows version that uses Vulkan instead of D3D12
(`option(REEOT_D3D12 ...)`, `CMakeLists.txt:20`):

```bat
cmake --preset win-amd64-release -DREEOT_D3D12=OFF -DREXSDK_DIR=...
```

Install the game with that build on a PC, copy the folder to the console and launch it
through prospero-win. This needs no code changes. It answers one question cheaply:
**does the game logic run on the console's hardware at all?**

Don't make it the destination:

- prospero-win's DXVK path covers D3D 8–11, not D3D12. Only the Vulkan build can work.
- ReXGlue reserves a 4.5 GiB file mapping (`src/system/xmemory.cpp:139`) and handles
  access faults with vectored exception handlers. Both go through Wine and then the
  console's memory rules. It may simply fail.
- Wine adds a layer between the game and the console, and its licence (LGPL) and
  experimental status come along with it.

Spend a few evenings on it at most. If it runs, you have a playable baseline to measure
the native title against. If it doesn't, the native route below is unaffected.

## The native title

### Repositories

This mirrors how PS5_RPCS3Title is put together:

| Repository | What it is | New? |
| --- | --- | --- |
| [`KongaTime/PS5_EdgeOfTimeTitle`](https://github.com/KongaTime/PS5_EdgeOfTimeTitle) | The title: the PS5_VulkanTemplate foundation (launch, pad, klog, packaging, build/deploy/run tools) plus `reeot/title_main.cpp`, which will start the game (for now it runs the memory test). Made with `ps5/tools/new-title.py`. | made |
| `KongaTime/EdgeOfTimeRecomp` (this one) | `__PROSPERO__` changes and a `ps5` CMake preset. | existing |
| [`KongaTime/rexglue-sdk`](https://github.com/KongaTime/rexglue-sdk) | Fork of `rexglue/rexglue-sdk` at v0.10.0. PS5 backends for memory, threads, faults and the app entry. Checked out as `../rexglue-sdk`, where `docs/BUILDING.md` already expects the SDK. | forked |
| `KongaTime/PS5_SDL3` | Fork of `libsdl-org/SDL` (3.x). PS5 video, joystick and audio backends. | new fork |
| `KongaTime/plume` | Fork of `zolaware/plume`. A small `__PROSPERO__` change to how volk is started (or a patch applied from this repo). | new fork |
| PS5_PayloadSDK, PS5_Vulkan, PS5_Mesa | Unchanged; pinned by revision as the other titles pin them. | existing |

Following the template's porting skill: keep each fork's PS5 changes behind
`__PROSPERO__`, in as few files as possible. Gaps in the console itself (libc, memory,
threads) go into the platform layer, not into the forks.

### 1. Build

**ReXGlue codegen stays on the PC.** `rexglue codegen reeot_manifest.toml` produces
`generated/`. Only the generated C++ is cross-compiled.

**Static libraries only.** The console has no `dlopen`, and the native tool refuses a
title that exports symbols.

- ReXGlue builds its runtime as a shared library (`src/system/CMakeLists.txt:51`,
  `add_library(rexruntime SHARED ...)`) and its Xenos GPU plugin likewise
  (`src/graphics/CMakeLists.txt:99`). On PS5, build `rexruntime` as `STATIC`. Leave out
  the GPU plugin and `src/system/gpu_plugin_loader.cpp`: this game uses its own renderer
  and doesn't pass `GPU_PLUGINS` to `rexglue_setup_target`.
- This repository's `CMakeLists.txt` sets `ENABLE_EXPORTS ON` and copies the runtime
  `.so` next to the program for every `UNIX` build (lines 435–452). Add a `PS5` branch
  beside `WIN32`/`APPLE`/`UNIX` that does neither, and doesn't link GTK,
  `Threads::Threads` or `${CMAKE_DL_LIBS}`.

**Toolchain and preset.** Add `ps5-release` to `CMakePresets.json` with:

- the SDK's `toolchain/prospero.cmake` (as `rpcs3/build-rpcs3.sh` uses it);
- `-march=znver2` in place of `-march=x86-64-v2`;
- clang 20 or newer (BUILDING.md asks for 20; the SDK takes the newest `llvm-config` it
  finds).

**Link.** Copy the shape of PS5_RPCS3Title's `rpcs3/rpcs3.cmake`:

- the game's archives in one `--start-group`/`--end-group`;
- `--defsym=<name>=ps5_<name>` for the libc functions the native tool reports missing;
- `--defsym=_ZTH...=0` for weak `thread_local` initialiser references;
- a version script that keeps all of those local.

The native tool's errors name each new symbol.

**Size.** The recompiled code is large, but RPCS3's `eboot.bin` was 60 MB and loaded
fine.

### 2. Guest memory

ReXGlue's memory works like Xenia's. It creates one 0x120000000-byte (4.5 GiB) shared
mapping with `shm_open` (`src/core/memory_posix.cpp:477`). It then maps many views of it
(the Xbox 360's virtual ranges and physical mirrors), trying base addresses `1 << 32`
to `1 << 63` until one fits (`src/system/xmemory.cpp:163-170`).

That won't work on the console as written:

- Every view of a POSIX shared-memory object is charged to the **448 MiB flexible
  budget** (PS5_PayloadSDK `platform/docs/PROBE.md`, "The pools").
- A mapping with no fixed address can land in the GPU driver's window.
- Large ranges are only granted from `0x10_0000_0000` upward, exactly at the address
  asked for or not at all (PROBE.md, "Virtual space").

**Done in the fork: `src/core/memory_ps5.cpp`**, on PS5_PayloadSDK's platform layer, in
direct memory:

- **The arena is backed as it is committed.** The 4.5 GiB mapping is cut into 2 MiB
  chunks. Each is a direct-memory object (`ps5_shm_create`) made the first time a commit
  touches it, then mapped into every view that covers it (`ps5_shm_map`), so the aliases
  see the same bytes. The physical heap's 512 MiB, committed at start-up and seen through
  five views, is 256 chunks and 1280 mappings; the rest costs what the game commits, not
  4.5 GiB. Uncommitted parts of a view stay reserved with no access, as on Windows.
- **The arena sits at `0x10_0000_0000`.** Each view reserves its range exactly there
  (`ps5_vrange_reserve_at`), and `Memory::Initialize` starts its base search at `1 << 36`
  on the PS5.
- **Protection is tracked per view page.** ReXGlue reads a page's protection and restores
  it later (`xmemory.cpp:1155`, the MMIO handler's race check), and the console's
  `sceKernelQueryMemoryProtection` does not always describe views (PS5_RPCS3's notes).
  A wrong answer there would leave guest memory with no access.
- **Failures unwind.** A chunk whose mapping into some view fails is backed out
  completely.
- **Anonymous allocations** are reservations committed in 64 KiB units
  (`ps5_vrange_commit`).

Measured on the console, in the title (Status has the numbers):

- **No access on shared views holds.** PS5_RPCS3 saw the kernel refuse some protections
  on views; these views accept it, so MMIO windows (`Memory::AddVirtualMappedRange`) and
  decommits work as written.

Still open:

- **16 KiB pages.** The console's pages are 16 KiB, against the Xbox 360's 4 KiB.
  `xmemory.cpp` already has a path for host pages bigger than guest pages (around line
  918, there for Apple Silicon). Write-watches may fire more often at 16 KiB granularity
  (a performance cost, not a correctness one).

The link recipe already moves `malloc` into the direct-memory heap
(`ps5platform/heap.h`).

### 3. The fault handler

ReXGlue catches accesses to memory-mapped hardware registers with a fault handler that
**reads the instruction that faulted** to decode it
(`src/system/mmio_handler.cpp:426-429`). The console maps a title's code
**execute-only**. Reading it ends the title immediately with `SYSTEM_XO_VIOLATION` and
no signal. RPCS3 hit exactly this.

Reuse PS5_RPCS3's fix:

- PS5_RPCS3Title's `rpcs3/code-copy.py` writes a readable copy of the code segment
  beside `eboot.bin` after the link (`ps5_title_post_link` in `rpcs3/rpcs3.cmake`).
- PS5_RPCS3's `Utilities/Thread.cpp` (`ps5_readable_code`, `ps5_code_copy_anchor`) maps
  the faulting address into that copy.
- Point `MMIOHandler::ExceptionCallback` at the copy, under `__PROSPERO__`.

Also:

- `src/core/exception_handler_posix.cpp` reads registers through Linux's `gregs[REG_*]`
  or macOS's `__ss`. Add a PS5 branch using `uc_mcontext.mc_rip` and the other `mc_*`
  fields. Include `ps5platform/context.h`: the console's context sits 48 bytes later than
  FreeBSD's headers say.
- Give each thread its own signal stack (`sigaltstack`), as PS5_RPCS3 does. A stack
  overrun otherwise faults again inside the handler and the title dies with nothing
  logged.
- Chain to the previous handler for faults ReXGlue doesn't own, so the console's crash
  report still names the real fault.

### 4. Threads

`src/core/threading_posix.cpp` relied on Linux-specific pieces. The fork now handles
each one (its commit "PS5: thread ids, names, affinity, signals and fibers"); what is
left is to see them work on the console (`ps5/probe`):

| ReXGlue uses (on Linux) | On the console | In the fork |
| --- | --- | --- |
| Real-time signals (`SIGRTMIN + n`) with `pthread_sigqueue` to suspend threads and wake them for callbacks | no `pthread_sigqueue`; real-time delivery to a thread unproven | `SIGUSR1`/`SIGUSR2` with `pthread_kill`, ReXGlue's macOS path. The handlers need no payload. |
| `timer_create` with `SIGEV_THREAD_ID` | missing | not used: Linux only; elsewhere ReXGlue's own timer queue |
| `syscall(SYS_gettid)` | missing | `pthread_getthreadid_np` |
| `pthread_setaffinity_np` with `cpu_set_t` | `ERANGE` for FreeBSD's 32-byte set | `ps5_pthread_{get,set}affinity_np`. A title runs on CPUs 0–12; put the Xbox 360's six hardware threads on six separate cores. |
| `makecontext` / `swapcontext` (fibers) | exported, but a made context is never entered (the probe) | a stack switch of its own, `fiber_ps5.cpp` |

More thread rules from PROBE.md and the porting skill:

- **Stacks.** Guest threads already ask for 16 MiB host stacks (`xthread.cpp:433`).
  Every other thread (SDL audio, the render thread, ReXGlue's helpers) gets **64 KiB**
  unless it asks for more. Give each one an explicit size.
- **Floating point.** Console threads start flush-to-zero and denormals-are-zero
  (MXCSR 0x9fc0, measured). Guest threads now start with both off, as on desktop
  (the fork's `FPSCRRegister::InitHost`); set the IEEE state (`ps5platform/fp.h`) on
  this repository's own threads that do float maths.
- **Thread-local storage.** It's emulated, about 3.4 ns per access. ReXGlue has 19
  `thread_local`s, including `thread_state_` and `current_xthread_tls_`. Watch for these
  in hot paths when profiling.
- **Thread exit.** Use `ps5_pthread_exit` (as PS5_RPCS3's `Utilities/Thread.cpp` does),
  so `thread_local` destructors run before emulated TLS is freed.

### 5. Files

- **Install on the PC, not the console.** Run the normal installer on a PC, then copy
  the installed game folder (about 10 GB) into the title's folder,
  `/data/homebrew/<TITLE_ID>/`, over FTP. On the console, a title's writes drop to about
  2 MiB/s after its first 1.3 GiB, and the installer needs desktop file dialogs anyway.
- **Paths.** `src/platform/user_dirs.cpp` builds paths from `HOME` and `XDG_*`. The
  simplest fix is for `title_main.cpp` to `setenv` those to folders under `/app0/` before
  the game starts, the same trick PS5_RPCS3Title uses for
  `MESA_DISK_CACHE_DATABASE_NUM_PARTS`. There is no `getcwd`. Give ReXGlue's virtual
  file system the game folder explicitly.
- **Permissions.** Create folders 0777 and files 0666 explicitly, so FTP can read logs
  and saves back.
- **Open files.** A title can hold about **249** files open by path (PROBE.md, "Open
  files"). Set `MESA_DISK_CACHE_DATABASE_NUM_PARTS=4` as PS5_RPCS3Title does. Count what
  the game keeps open (its `.pkz` packages, saves, logs) with the `--wrap=open/openat/fopen`
  census PS5_RPCS3 uses (`rpcs3/ps5/ps5_fdtrack.cpp`). Use `ps5platform/fdv.h` if it
  gets close.
- **Missing libc.** The directory and `*at` families, `access` and `statvfs` come from
  the platform layer through the link recipe. `realpath` is refused (EPERM), which
  breaks `std::filesystem::canonical`. PS5_RPCS3Title binds it to `ps5_realpath`. Do the
  same.
- **Logs.** Write a trace file (`/app0/reeot-trace.txt`) one line at a time, like
  `rpcs3-trace.txt`, so a crash keeps it. Send warnings to klog.

### 6. Display, controller and sound (SDL3)

SDL3 is used in four places:

- ReXGlue's app shell and window (`src/ui/windowed_app_main_sdl.cpp`, `window_sdl.cpp`),
  sound (`src/audio/sdl/`) and controllers (`src/input/sdl/`, with `gamecontrollerdb.txt`);
- plume's Vulkan surface (`PLUME_SDL_VULKAN_ENABLED`, forced on in
  `thirdparty/CMakeLists.txt:3`; `plume_vulkan.cpp:2116`);
- 11 files in this repository's `src/`.

**Recommended: give SDL3 PS5 backends, once.** Then ReXGlue, plume and this repository
build almost unchanged, and any other ReXGlue recompilation gets the same backends for
free.

| SDL3 part | PS5 backend | Start from |
| --- | --- | --- |
| Video | One fullscreen "window" that is the display (3840×2160, fixed). Vulkan surface through `VK_KHR_display`. `SDL_Vulkan_GetVkGetInstanceProcAddr` returns `vk_icdGetInstanceProcAddr`. | SDL's `SDL_Vulkan_Display_CreateSurface` (`src/video/SDL_vulkan_utils.c`, used by the Vivante driver); the template's `vulkan-on-radv.md`, "The display" |
| Joystick / gamepad | DualSense through the template's pad API (`ps5/src/platform.h`): sticks, triggers, rumble, light bar. Reported as a PS5-type gamepad so the game shows PlayStation button icons. A reading flagged `PAD_INTERCEPTED` is no input. | SDL's Vita joystick driver (`src/joystick/vita/`); PS5_RPCS3's `rpcs3/ps5/ps5_pad_handler.cpp` |
| Audio | `libSceAudioOut`, 48 kHz stereo 16-bit. | SDL's Vita audio driver (`src/audio/vita/`, the same `sceAudioOut` design); PS5_RPCS3's `rpcs3/ps5/ps5_audio_backend.cpp` |
| Message boxes, file dialogs, URLs, loading libraries | Message box goes to klog and the trace. The rest report "unsupported". | SDL's dummy implementations |
| Base path, locale | `/app0/`; the console's system language | — |

The alternative, which PS5_RPCS3 took with `USE_SDL=OFF`, is to skip SDL. That means
writing ReXGlue `ps5` drivers beside its `nop`/`sdl` audio and input drivers, a PS5 app
entry and window, a plume surface path, and `#ifdef`-ing out SDL in 11 files here. That's
more changes spread across three projects, and the result only helps this game.

Either way:

- **plume** calls `volkInitialize()` (`plume_vulkan.cpp:4480`), which looks for a Vulkan
  loader. Under `__PROSPERO__`, call `volkInitializeCustom(vk_icdGetInstanceProcAddr)`
  instead, as the template's `ps5/src/main.cpp` does.
- **Display limits.** The swapchain is fixed at 3840×2160 and presents with FIFO (wait
  for the display). The game already renders at its own internal resolution and scales
  when presenting, so pick an internal resolution that holds full speed and let the
  present scale it. There's no asynchronous compute (the game only uses a `DIRECT`
  queue: `src/gpu/device.cpp:1126`).
- **120 Hz.** It needs the high-frame-rate bits in `param.json`. The display drops back
  to 59.94 Hz by itself on a 60 Hz TV.
- **Entry point.** `REX_DEFINE_APP` uses the `main()` in ReXGlue's
  `src/ui/windowed_app_main_sdl.cpp:129`. On PS5, rename it to a callable function
  (for example `rex_app_main`) for `title_main.cpp` to call, as PS5_RPCS3Title calls
  `rpcs3_ps5_run`.

### 7. EdgeOfTimeRecomp's desktop code

| File(s) | On PS5 |
| --- | --- |
| `src/installer/*` (the installation wizard, disc and DLC installs, installer music) | Leave out. Install on the PC (section 5). Have the game check its files are present at start and say clearly what's missing. |
| `src/platform/file_dialog.cpp`, `fatal_dialog.cpp` | Fatal errors go to the trace, klog and an on-screen ImGui message; no file dialogs. |
| `src/platform/desktop_shortcut.cpp`, `update_check.cpp`, `moltenvk.cpp` | Leave out. |
| `src/platform/process.cpp`, `src/core/quit.cpp` | Leave through the shell exit a title must use (template, `platform-contracts.md`), never `exit()`. A "restart to apply" setting asks the player to relaunch. |
| `src/platform/crash_handler.cpp` | A PS5 version that writes the faulting address and a backtrace to the trace, using `ps5platform/context.h`. |
| `src/platform/display.cpp`, the Video settings page | One fixed display. Hide windowed modes and display resolution; keep internal resolution, anti-aliasing and the graphics presets. |
| `src/platform/user_dirs.cpp` | `/app0/` paths (section 5). Take the "Auto" language from the console's system language. |
| `src/mods/mod_manager.cpp` | Leave out (mods are unavailable upstream anyway). |
| `reeot.toml` | It sets `mnk_mode = true`. Default to controller mode on PS5 and hide keyboard-and-mouse settings, unless USB keyboard and mouse support is added later. |

### 8. The title shell

1. `ps5/tools/new-title.py` in PS5_VulkanTemplate makes the title. Pick a local title
   ID nothing else uses (PPSA99200 is PS5_RPCS3Title, PPSA99130 is Vulkan Template,
   PPSA99995 is prospero-win).
2. `title_main.cpp`, modelled on PS5_RPCS3Title's `rpcs3/title_main.cpp`:
   - `platform_init`, the trace file, the build number;
   - `setenv` for the `XDG_*` paths and `MESA_DISK_CACHE_DATABASE_NUM_PARTS`;
   - then call the game's entry.
3. A CMake include like `rpcs3/rpcs3.cmake` links the game's archives and runs the
   code-copy step after the link.
4. Title art: use the project's own icon (`res/app/icon/`), not the game's box art or
   logos.
5. Keep the template's tools: `build.sh`, `deploy.sh`, `run.sh`, and
   `host-reference.sh` for comparing against the Linux reference build.

## Milestones

Prove each one on the console, and record its numbers in the commit message, as
PS5_RPCS3 does.

| # | Milestone | Done when |
| --- | --- | --- |
| M0 | Desktop reference | The Linux build reaches gameplay with a picture (Step 0). |
| M1 | Cross-compiles | ReXGlue, SDL3, plume, this repository and `generated/` build as static archives with the SDK toolchain, and the native tool's missing-symbol list is empty. |
| M2 | Boots | The title launches. ReXGlue reserves guest memory at `0x10_0000_0000`, loads `Default.xex` and `GameLogic.dll` from the title folder, and the guest's main thread runs. The trace shows kernel calls. No picture yet. |
| M3 | First frame | plume creates a device on RADV and a `VK_KHR_display` swapchain, and the title screen draws. It matches the Linux reference frame. |
| M4 | Controller and sound | The DualSense drives the menus. Music and effects play through `libSceAudioOut`. |
| M5 | Playable | The first level plays. Saving and loading work. Frame rate, shader-compile stalls and memory use are measured and written down. |
| M6 | Polish | Console settings page, PlayStation button icons, a 120 Hz option, the upscaled textures within the memory budget, packaging. |

## Risks and open questions

- **ReXGlue is in early development.** Its docs expect breaking API changes. Stay on
  v0.10.0 (what `reeot_manifest.toml` pins), and rebase the fork deliberately when this
  repository moves to a newer SDK.
- **Memory budget.** The 4.5 GiB guest mapping, RADV, textures and the heap all have to
  fit in about 12 GiB of direct memory (section 2).
- **Thread signals.** The fork no longer needs real-time signals (section 4), but
  suspending a thread from its signal handler, and emulated TLS read inside one, are
  unproven until `ps5/probe` runs.
- **Code model.** ReXGlue compiles everything with `-mcmodel=large` on x86-64 outside
  macOS (for executables over 35 MB). Whether the console's link recipe and native
  tool accept that is untested; drop it on the PS5 if they don't.
- **Shader-compile stalls.** RADV compiles pipelines when they're first used. This
  repository already has a pipeline pre-cache (`src/gpu/pipeline/pso_precache.cpp`).
  Make sure it runs during loading on the console, and keep RADV's disk cache in
  `/app0/radv-shader-cache/`.
- **Firmware.** Reports say current jailbreaks cover firmware 7.00 to 13.60, have to be
  redone after every reboot, and that 14.00 blocks them. Don't update a console you
  test on.
- **Performance.** Probably fine: the desktop minimum is a Haswell-class CPU and a
  GeForce GT 730-class GPU, both well below a PS5. That stays unproven until M5.

## Licences and sharing

| Part | Licence |
| --- | --- |
| EdgeOfTimeRecomp | BSD 3-Clause |
| ReXGlue SDK | BSD-style (Xenia-derived) |
| plume | MIT |
| SDL3 | zlib |
| PS5_VulkanTemplate foundation | MIT |
| Mesa / RADV | MIT (mostly) |
| PS5_PayloadSDK platform layer, PS5_Vulkan link recipe | GPL-3.0-or-later |

All of these can be combined, so the built title as a whole is GPL-3.0-or-later. This
is unlike RPCS3, whose GPL-2.0-only licence can't be combined with the platform layer.

Two things the licences don't settle:

- **Game files.** They're never included. Players dump their own copy, exactly as on
  desktop.
- **The built title contains code recompiled from the game.** Upstream ships desktop
  executables built the same way, so follow upstream's practice. The safest course is
  PS5_RPCS3Title's: build it for your own console from source.

Upstream's `docs/CONTRIBUTING.md` says the project is strict about AI-generated code.
Keep this work in the fork unless upstream agrees to take it.

## First steps

1. **M0.** Build `linux-amd64-release` here and play to the first level.
2. **Optional.** Try the Windows `REEOT_D3D12=OFF` build under prospero-win.
3. **Fork** `rexglue/rexglue-sdk` at `c94f5eb` (v0.10.0) as `KongaTime/rexglue-sdk`,
   and `libsdl-org/SDL` as `KongaTime/PS5_SDL3`. *(ReXGlue: forked, with the PS5
   commits on `main`. SDL3: not yet.)*
4. **Make the title** with `new-title.py`, and pin PS5_PayloadSDK `1de8b37` and
   PS5_Vulkan as PS5_RPCS3Title does. *(Made: PS5_EdgeOfTimeTitle, `PPSA99300`,
   running the memory test for now.)*
5. **Cross-build ReXGlue unchanged** (`rexcore`, then `rexsystem` as a static library)
   with the SDK toolchain. Collect the errors: they're the list of platform code to
   replace. *(Done, and fixed: [Status](#status).)*
6. **Probe the console** for what section 4 doesn't know yet: real-time signals,
   `sigqueue`, `makecontext`/`swapcontext`. Use a probe like PS5_PayloadSDK's
   `src/probe.c`. *(Done: two console runs, [Status](#status).)*
7. **Write `memory_ps5.cpp`** (section 2). Use PS5_RPCS3's `vm_native_ps5.cpp` as the
   model, and get M2. *(Done: all checks pass on the host model and, in the title, on
   the console. M2 also needs the game.)*
