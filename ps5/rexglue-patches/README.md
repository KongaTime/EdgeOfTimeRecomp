# PS5_ReXGlue, until the fork exists

These patches are the `main` branch of **PS5_ReXGlue**, the ReXGlue SDK fork that adds
the PS5 as a target (`docs/PS5-PORTING.md`). They are here only because the fork
could not be created from the session that wrote them. Once
`KongaTime/PS5_ReXGlue` is on GitHub with these commits, delete this folder: forks
are not kept as patch series.

| Patch | What it does |
| --- | --- |
| 0001 | Builds the runtime with the payload SDK's toolchain: static `rexruntime`, no GPU plugin, codegen or CLI, no X11/Wayland, an FFmpeg configuration for the console |
| 0002 | Compiles the POSIX sources for the console: libc++ 18 shims, FreeBSD's file and clock calls |
| 0003 | Thread ids, names, affinity, suspend/wakeup signals and fibers with what the console exports |
| 0004 | The exception handler reads the console's machine context |
| 0005 | The fork's README section |

Base: upstream v0.10.0, `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`. `git am` gives the
commits new ids (it records whoever applies them as the committer); pin whatever ids
`main` has once it is pushed.

## Making the fork

1. On GitHub, fork `rexglue/rexglue-sdk` into KongaTime and name it `PS5_ReXGlue`
   (the fork page has a name field). Copy only the default branch.
2. Beside this repository:

   ```bash
   git clone https://github.com/KongaTime/PS5_ReXGlue.git ../PS5_ReXGlue
   cd ../PS5_ReXGlue
   git checkout -B main c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
   git am ../EdgeOfTimeRecomp/ps5/rexglue-patches/*.patch
   git push --force-with-lease origin main   # main becomes v0.10.0 plus the PS5 commits
   git remote add upstream https://github.com/rexglue/rexglue-sdk.git
   git remote set-url --push upstream DISABLED
   ```

   The force push replaces only the copy of upstream's latest `main` that forking made;
   from then on `main` is published and is never rewritten. Upstream is merged in.

3. `ps5/tools/build-rexglue.sh` builds from `../PS5_ReXGlue`.
