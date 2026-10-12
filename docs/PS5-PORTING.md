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
| [KongaTime/rexglue-sdk](https://github.com/KongaTime/rexglue-sdk) (the PS5 fork) | `main` at `f1b5117` |
| [KongaTime/PS5_EdgeOfTimeTitle](https://github.com/KongaTime/PS5_EdgeOfTimeTitle) (the title) | `main` at `6934c3c` |
| [KongaTime/TheSimpsonsGame-PS5-Builder](https://github.com/KongaTime/TheSimpsonsGame-PS5-Builder) (the reference port, a fork of LoZazaMastro's) | `59e7086` |
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
- [A working reference: The Simpsons Game on the PS5](#a-working-reference-the-simpsons-game-on-the-ps5)
- [Step 0: the desktop build must show a picture](#step-0-the-desktop-build-must-show-a-picture)
- [Optional shortcut: the Windows build under prospero-win](#optional-shortcut-the-windows-build-under-prospero-win)
- [The native title](#the-native-title)
    - [Repositories](#repositories)
    - [1. Build](#1-build)
    - [2. Guest memory](#2-guest-memory)
    - [3. The fault handler](#3-the-fault-handler)
    - [4. Threads](#4-threads)
    - [5. Files](#5-files)
    - [6. Display, controller and sound](#6-display-controller-and-sound)
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
  at `7f74d76`) build `librexruntime.a` and its libraries (FFmpeg, fmt, spdlog and the rest; no SDL3) with
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
  (`MEMORY_PS5_TEST_EMBEDDED`). Built here with PS5_Vulkan `5b5e4fc`'s RADV (PS5_Mesa
  `7b59ef2`), installed in `/data/homebrew`, and launched from the home screen, it ran
  the test above. Its second build (`b142389`) adds the code read check (section 3);
  both write `/app0/reeot-test-results.txt`.
- **The title's code made readable** (section 3). The code read check's console run
  (2026-10-12, the title's
  [`reeot/results/2026-10-12.txt`](https://github.com/KongaTime/PS5_EdgeOfTimeTitle/blob/main/reeot/results/2026-10-12.txt)):

  | Checked | On the console |
  | --- | --- |
  | what the kernel says of the title's code (`0x400000`–`0xd3c000`) and a system module's | `0x4`, execute only |
  | reading the title's untouched code | ended the title (the file stops at that read) |
  | `sceKernelMprotect` read+execute on a page of code | succeeds; the page still runs and reads back its real bytes |
  | what the kernel says of that page afterwards | still `0x4`: the query cannot tell readable code |

  The fork's `ExceptionHandler::Install` now makes the code segment readable before any
  handler runs (`2f4a3ef`), so the MMIO handler reads the instruction where it is, with
  no code copy. The title's next run checks the same on the whole segment.

- **The Simpsons port's PS5 app layer, ported** (first steps, 9; the fork's
  [`ps5/PORTED.md`](https://github.com/KongaTime/rexglue-sdk/blob/main/ps5/PORTED.md),
  `7f74d76`): the app context and window (one 3840×2160 display surface), the entry as
  `rex_ps5_app_main` for the title's `main` to call, the DualSense through `scePad` in
  v0.10.0's device-based input model, and `sceAudioOut` sound. `ReXApp` uses them on the
  PS5, finds the game in `/app0/gamedata` and logs each start-up stage. SDL3 is neither
  built nor linked for the PS5. It builds, every import resolves, and the desktop build
  still configures and compiles the shared files; none of it has run on the console yet.

**Not done**

- **The ReXGlue runtime runs on the console** (test app 3, 2026-10-12, the title's
  [`reeot/results/2026-10-12-test3.txt`](https://github.com/KongaTime/PS5_EdgeOfTimeTitle/blob/main/reeot/results/2026-10-12-test3.txt)).
  Linked whole into the title (28.7 MB; `isatty`, `pathconf` and `realpath` bound to the
  platform layer, the fork's own `ffs` and `timegm`), it started through
  `rex_ps5_app_main` and quit cleanly:

  | Checked | On the console |
  | --- | --- |
  | the window | open, 3840×2160 |
  | `Memory::Initialize` | passed; the app then read its own code, made readable by the fork's fault handler |
  | sound (`sceAudioOut`) | the tone was heard; 375 frames in 1663 ms: 64 queued, then 311 at AudioOut's 5.33 ms pace |
  | the controller (`scePad`) | a gamepad; the rumble was felt; the D-pad, face buttons, Options, touch pad, L3 and R3 read as their XInput buttons; full stick range; keystrokes with repeats |

  Not yet checked: L1, R1, L2 and R2 (not pressed before the test's 400-line cap); the
  mapping is the Simpsons port's.
- **The crash and trace reports** from the Simpsons port, plume's volk start and
  surface, this repository's own PS5 changes, and the title's real program, which starts
  the game (sections 5–8).
- **Step 0** (the desktop build with game files), and running the runtime itself on the
  console.

**Waiting on you**

- Later, M2 will need the game's files from your own copy (Step 0).

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
2. **The window, controller and sound.** ReXGlue's are SDL3's on the desktop. The
   Simpsons Game's PS5 port has PS5 ones for ReXGlue already (next section); this
   repository's own SDL3 uses and plume's surface remain.
3. **This repository's desktop features:** the installer, file dialogs, desktop
   shortcuts, the update check and so on.

Most of these problems have already been solved somewhere in my stack. The sections
below name where.

## A working reference: The Simpsons Game on the PS5

[TheSimpsonsGame-PS5-Builder](https://github.com/LoZazaMastro/TheSimpsonsGame-PS5-Builder)
(LoZazaMastro; forked as
[KongaTime/TheSimpsonsGame-PS5-Builder](https://github.com/KongaTime/TheSimpsonsGame-PS5-Builder),
read at `59e7086`) is a native PS5 port of another Xbox 360 recompilation,
[TheSimpsonsGameRecomp](https://github.com/YesterMester/TheSimpsonsGameRecomp), on
**ReXGlue** too. It runs on the same stack: PS5_PayloadSDK's platform layer, RADV from
PS5_Mesa with its VideoOut display, ShadowMountPlus. Its README reports 4K output,
120 Hz and an in-game settings menu. A Windows builder turns the player's own disc
image into a finished `PPSA74207` folder.

Its copy of ReXGlue (`tools/rexglue-sdk`) is v0.8.0 with 83 new files and 177 changed
ones. The PS5 part of that is a complete app layer for ReXGlue, under ReXGlue's own
BSD-3-Clause licence, which is what this repository is built on (`rex::ui::WindowedApp`,
`WindowedAppContext`, `Window`, ReXGlue's audio and input systems):

| What this plan needs | In the Simpsons port (`tools/rexglue-sdk/`) |
| --- | --- |
| The app's entry and window (section 6) | `src/ui/windowed_app_main_prospero.cpp` (ReXGlue's `main()` on the PS5), `windowed_app_context_prospero.cpp`, `window_prospero.cpp`, `startup_prospero.h` |
| The display: a Vulkan surface on RADV (section 6) | `src/ui/surface_prospero.cpp` (a fixed 3840×2160 surface), `src/ui/vulkan/vulkan_display_prospero.cpp` (`VK_KHR_display` on PS5_Mesa's VideoOut WSI) |
| Sound (section 6) | `src/audio/prospero/` (`sceAudioOut`) |
| The controller (section 6) | `src/input/prospero/` (`scePad`, with a pad mapping) |
| Files, settings, saves (section 5) | `src/ui/prospero_paths.cpp`, `src/core/settings_store_prospero.cpp`, the PS5 branch of `host_path_file.cpp` |
| Fault and crash reports (section 3) | `src/core/exception_handler_prospero.cpp`, `fault_prospero.cpp`, `abort_prospero.cpp`, `trace_prospero.cpp`, `signal_thread_prospero.cpp` |
| An in-game settings menu (M6) | `src/ui/overlay/settings_prospero.cpp` (L1 + R1 + Options) |

The plan takes those, ported to v0.10.0, in place of writing them. What it does not take:

- **Guest memory and fibers.** Its `memory_prospero.cpp` backs the whole 4.5 GiB arena
  with one direct-memory object up front ("the full 4.5 GiB budget remain[s]
  unvalidated", its header says). This fork's `memory_ps5.cpp` backs it as it is
  committed and passed on the console (section 2). The fork's fibers and floating-point
  start state are console-checked too.
- **The graphics emulation.** Much of its change is to ReXGlue's emulated Xenos GPU
  (Vulkan and D3D11 back ends, tessellation, render-target transfers). This game draws
  with its own renderer through plume, so none of that applies.
- **Its build and packaging scripts** (`prospero/`) are GPL-3.0-or-later; they show how
  its title is linked and packaged, next to PS5_VulkanTemplate's tools, which this
  title uses.

Two things differ from what this plan assumed:

- **The fault handler reads the code where it is** (section 3). A title cannot read its
  own code unless it makes it readable (the code read check), and that port does not,
  so its game presumably never takes that path. This fork makes the code readable.
- **No SDL.** Its ReXGlue drivers talk to the console directly, which is the route
  section 6 now takes.

The version gap is the work: upstream ReXGlue changed 378 files between v0.8.0 and
v0.10.0, so its PS5 files are ported, not copied, with their copyright notices kept.

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
| `KongaTime/PS5_SDL3` | Not needed: the Simpsons port's PS5 drivers for ReXGlue replace SDL3 there (section 6). | — |
| [`KongaTime/TheSimpsonsGame-PS5-Builder`](https://github.com/KongaTime/TheSimpsonsGame-PS5-Builder) | The reference port: its ReXGlue's PS5 app layer is ported into the fork. Read only. | forked |
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
(`src/system/mmio_handler.cpp:426-429`). PS5_RPCS3 found the console maps a title's
code **execute-only**: reading it ended the title immediately with
`SYSTEM_XO_VIOLATION` and no signal. The Simpsons Game's PS5 port reads the code where
it is, with no copy.

**Done: the title makes its code readable.** The title's code read check
(PS5_EdgeOfTimeTitle `reeot/code_read_check.cpp`, console run 2026-10-12) found that
`sceKernelMprotect` read+execute makes a page of the title's code readable, and it still
runs; untouched code still ends the title when read, and the kernel's query answers
execute-only either way. So the fork's `ExceptionHandler::Install` makes the whole code
segment readable (its bounds from `sceKernelQueryMemoryProtection` on a function in it)
before any handler runs, and `MMIOHandler` reads the instruction where it is.

System modules' code (`0x800000000` up) stays execute-only. A fault in an MMIO range
from inside one (a libc `memcpy`, say) would still end the title when the handler reads
the instruction. The game's MMIO accesses are expected to come from its recompiled
code, which is readable now.

PS5_RPCS3's fix, not needed here, for the record:

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

### 6. Display, controller and sound

SDL3 is used in four places:

- ReXGlue's app shell and window (`src/ui/windowed_app_main_sdl.cpp`, `window_sdl.cpp`),
  sound (`src/audio/sdl/`) and controllers (`src/input/sdl/`, with `gamecontrollerdb.txt`);
- plume's Vulkan surface (`PLUME_SDL_VULKAN_ENABLED`, forced on in
  `thirdparty/CMakeLists.txt:3`; `plume_vulkan.cpp:2116`);
- 11 files in this repository's `src/`.

**Taken: the Simpsons Game port's route.** Its ReXGlue has PS5 drivers that talk to the
console directly (previous section's table): the app entry and window, a 3840×2160
`VK_KHR_display` surface on PS5_Mesa's VideoOut, `sceAudioOut` sound and `scePad`
input. Ported into this fork, they replace SDL3 under ReXGlue. What stays here:

- **plume's surface.** `Video::CreateHostDevice` (`src/gpu/device.cpp:1063`) builds a
  `plume::RenderWindow` from the ReXGlue window, and plume makes its Vulkan surface
  through SDL (`PLUME_SDL_VULKAN_ENABLED`). On the PS5 it takes the display surface the
  ported `vulkan_display_prospero.cpp` makes instead.
- **This repository's own SDL3 uses**, in 11 files (display modes, message boxes,
  windows): each gets a PS5 branch or is left out (section 7).

The SDL3 backends this plan first proposed are not needed. For the record, they were:

| SDL3 part | PS5 backend | Start from |
| --- | --- | --- |
| Video | One fullscreen "window" that is the display (3840×2160, fixed). Vulkan surface through `VK_KHR_display`. `SDL_Vulkan_GetVkGetInstanceProcAddr` returns `vk_icdGetInstanceProcAddr`. | SDL's `SDL_Vulkan_Display_CreateSurface` (`src/video/SDL_vulkan_utils.c`, used by the Vivante driver); the template's `vulkan-on-radv.md`, "The display" |
| Joystick / gamepad | DualSense through the template's pad API (`ps5/src/platform.h`): sticks, triggers, rumble, light bar. Reported as a PS5-type gamepad so the game shows PlayStation button icons. A reading flagged `PAD_INTERCEPTED` is no input. | SDL's Vita joystick driver (`src/joystick/vita/`); PS5_RPCS3's `rpcs3/ps5/ps5_pad_handler.cpp` |
| Audio | `libSceAudioOut`, 48 kHz stereo 16-bit. | SDL's Vita audio driver (`src/audio/vita/`, the same `sceAudioOut` design); PS5_RPCS3's `rpcs3/ps5/ps5_audio_backend.cpp` |
| Message boxes, file dialogs, URLs, loading libraries | Message box goes to klog and the trace. The rest report "unsupported". | SDL's dummy implementations |
| Base path, locale | `/app0/`; the console's system language | — |

Skipping SDL, as PS5_RPCS3 did with `USE_SDL=OFF`, meant writing ReXGlue `ps5` drivers
beside its `nop`/`sdl` ones and a PS5 app entry and window. The Simpsons port has written
those, for ReXGlue, so that is now the shorter route.

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
  `src/ui/windowed_app_main_sdl.cpp:129`. The Simpsons port's
  `windowed_app_main_prospero.cpp` is the PS5 one, and is the title's `main()` there. In
  PS5_EdgeOfTimeTitle, rename it to a callable function (for example `rex_app_main`)
  for `title_main.cpp` to call, as PS5_RPCS3Title calls `rpcs3_ps5_run`.

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
- **The reference port's ReXGlue is older.** The Simpsons port is on v0.8.0, this
  repository on v0.10.0; upstream changed 378 files between them. Port its PS5 files
  one area at a time, and keep this fork's console-checked memory and fibers.
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
| The Simpsons Game port's PS5 changes to ReXGlue (`tools/rexglue-sdk`) | BSD-3-Clause, as ReXGlue; keep their notices when porting them |
| The Simpsons Game port's build scripts and builder (`prospero/`) | GPL-3.0-or-later |

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
   *(Forked, with the PS5 commits on `main`. The SDL3 fork first planned here is not
   needed: section 6.)*
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
8. **The code read check** (section 3). *(Done: `sceKernelMprotect` makes the title's
   code readable; the fork does it at start, `2f4a3ef`. No code copy.)*
9. **Port the Simpsons port's PS5 app layer** into the fork, at v0.10.0: the app entry,
   window and display surface, sound, the controller, paths and settings, and the fault
   and crash reports ([the table](#a-working-reference-the-simpsons-game-on-the-ps5)).
   Keep the fork's memory, fibers and floating-point start. Build it with
   `ps5/tools/build-rexglue.sh`; check it on the console with the title. *(The entry,
   window, display surface, controller and sound are ported, `7f74d76`, and ran on the
   console in test app 3; the crash and trace reports remain.)*
10. **This repository on the PS5:** plume on the display surface, the PS5 branches of
    its SDL3 uses, its desktop-only code (section 7), and `title_main.cpp` calling the
    game (section 8). That is M1 once `generated/` builds too.
11. **The game's code and files** (Step 0, then M2): `rexglue codegen` from your own
    copy, built for the PS5, and the game folder copied into the title's. Later, a
    builder like the Simpsons port's could do this from a disc image.
