# Deus Ex: Human Revolution loader for Apple Silicon

Run the **Mac version of _Deus Ex: Human Revolution – Director's Cut_** (Feral
Interactive, 2014) on current Apple Silicon Macs, using **your own copy of the
game**.

The original Mac release is a 32-bit Intel (i386) application. macOS dropped
32-bit support in Catalina (10.15), and Rosetta 2 never ran 32-bit apps, so the
game no longer starts on any current Mac. Windows compatibility layers (Wine,
CrossOver, etc.) did not work reliably for this game either.

This project is a loader that runs the unmodified original Mac binary:

- it loads the i386 Mach-O executable and its libraries into a 32-bit guest
  address space inside a 64-bit process,
- executes the 32-bit x86 code with an own **x86 → arm64 JIT** (or Rosetta 2 as a
  development backend),
- and replaces every macOS API the 2014 game uses (Carbon, legacy Objective-C
  runtime, OpenGL/CGL, CoreAudio/OpenAL, AVFoundation, CoreFoundation, IOKit,
  Steamworks, …) with thin bridges to the current 64-bit frameworks.

**No game files are included or distributed.** You need to own the game (e.g. on
Steam) and point the loader at your installed copy.

## Status

Work in progress, but the game is playable:

- launcher (Feral options dialog with all tabs), intro and tutorial movies,
  loading screens, cutscenes and gameplay render correctly
- audio, keyboard/mouse input, windowed and fullscreen mode
- Steam: the 2014 `libsteam_api` is bridged to the current 64-bit Steam client
  (ownership check, achievements, cloud saves)
- localization (e.g. German) via Steam's language setting
- runs natively on arm64 with the own JIT (`make native-dxcpu`), from the
  launcher through gameplay; the Rosetta build remains as reference

Known issues: frame drops while the game streams in new areas (driver stalls
on its worker GL contexts), rendering at the "looks like" resolution of a
Retina display (no native 4K yet), and antialiasing options unverified. x86 memory
ordering costs about 11% frame rate (without it the game crashes on level loads).

## How it works

| Part | Where |
| --- | --- |
| Guest memory (4 GB window), Mach-O loader, object-file linker | `src/host/gmem.c`, `src/host/macho*.c` |
| CPU backends: native JIT (`dxcpu`), Rosetta 2 (LDT 32-bit code segment), Unicorn | `src/cpu/` (Rust), `src/host/cpu_*.c` |
| API layer ("thunks"): libc/POSIX, CoreFoundation, Carbon on AppKit, ObjC bridge, OpenGL/CGL, audio, video, Steam | `src/host/api/` |
| Guest runtime built for i386: libstdc++ (GCC 4.2.1 ABI), libunwind, fast libc paths | `src/guest/` |
| Guest test programs (run on every backend) | `tests/guest/` |

More detail: [docs/THUNKS.md](docs/THUNKS.md), [docs/JIT.md](docs/JIT.md).

## Building

Requirements: Apple Silicon Mac, Xcode (command line tools), Rust (stable);
Homebrew `unicorn` only for the optional Unicorn backend. The first build
fetches the sources of the i386 guest runtime (GCC 4.2.1 libstdc++, LLVM
libunwind, see `src/guest/libstdcxx/README.md`).

```sh
make native-dxcpu   # native arm64 build with the own JIT  -> build/arm64-dxcpu/dxhost
make                # Rosetta 2 development build           -> build/x86_64/dxhost
```

## Running

Install the Mac version of the game with Steam (or `steamcmd` with
`+@sSteamCmdForcePlatformType macos`), keep Steam running and logged in, then:

```sh
build/arm64-dxcpu/dxhost "/path/to/Deus Ex Human Revolution - Directors Cut.app"
```

Useful environment variables:

| Variable | Effect |
| --- | --- |
| `DXH_LOG=0..3`, `DXH_LOG_FILE=path` | log level (3 = trace every host call) and log file; `kill -USR1/-USR2 <pid>` toggles tracing at runtime |
| `DXH_LANGUAGE=german` | override the game language |
| `DXH_FPS=5` | frames/s, worst frame and frames over 33 ms every 5 s |
| `DXH_SLOW_MS=10` | log every host call slower than 10 ms (frame spikes), except calls that are meant to wait |
| `DXH_THUNK_STATS=10`, `DXH_GL_STATS=1` | periodic host call / buffer mapping statistics |
| `DXH_WATCHDOG=10` | if the main thread hangs for 10 s, log every guest thread's host call and guest frames |
| `DXH_GL_REALMAP=1` | map write-only GL buffers in the driver again (default: upload written ranges, no GPU wait) |
| `DXH_WINDUMP=1` | log all windows' visibility after window operations |
| `DXH_NO_PATCHES=1` | do not apply the game fixes in `src/host/patches.c` |

Native CPU core (`build/arm64-dxcpu`):

| Variable | Effect |
| --- | --- |
| `DXCPU_TSO=0` | turn off x86 memory ordering (on by default: barriers around guest loads/stores, stack and `[ebp-x]` locals excepted). Faster by ~11%, but HR then crashes in its job system on about every other level load; `tests/guest/litmus.c` shows the reordering. `DXCPU_TSO=rcpc` uses ldapr/stlr instead of barriers (not faster on M-series), `DXCPU_TSO_EBP=0` keeps barriers on `[ebp-x]` |
| `DXCPU_STATS=10` | every 10 s: dispatcher entries and the most interpreted instructions; `DXCPU_STATS_ADDR=Mnemonic` adds the addresses |
| `DXCPU_VERIFY=1` | run every block in the interpreter as well and report blocks whose results differ (slow; shared-memory races show up as noise) |
| `DXCPU_JIT_OFF=sse,fast,int,x87,all,m:Add+Shl` | interpret selected instruction groups (bisecting translation bugs) |
| `DXCPU_INTERP=1` | interpreter only |

## Unattended play testing

`DXH_AUTOPLAY=<script>` lets a bot drive the game: synthetic keyboard/mouse
input (the real cursor is never grabbed, real input is ignored), screenshots of
the presented frames and a hang watchdog. Scripts are plain text, see
`src/host/api/autoplay.m` for the commands and `tools/autoplay/` for examples
(`load.txt`: launcher, continue the last save, play randomly, quit).

```sh
tools/autoplay.sh tools/autoplay/load.txt 10 8 autoplay-out/run1   # 10 runs, 8 min each
```

`tools/autoplay.sh` sorts every run into ok / crash / hang / timeout, keeps the
log and screenshots per run and writes `summary.txt`. It backs up the Steam save
folder and the launcher preferences first and restores them after each run.
Further environment (e.g. `DXCPU_TSO=0`, `DXH_HOST=build/dev/dxhost`) is passed
through, which makes A/B comparisons a loop over two settings.

## Tests

```sh
tests/guest/build.sh tests/guest/libc_file.c build/t/libc_file.o
build/arm64-dxcpu/dxhost --test build/t/libc_file.o
```

Several tests print a hash of their results so the backends can be compared
with real x86 through the Rosetta build: `alu_ext`, `shift_flags`, `sse_nan`,
`x87_ops`. They must print the same hash on `build/x86_64` and
`build/arm64-dxcpu` (also with `DXCPU_INTERP=1`).

## Legal

This is an independent compatibility project, not affiliated with or endorsed by
Square Enix, Eidos-Montréal, Feral Interactive or Valve. "Deus Ex" is a trademark
of its respective owner. The repository contains no game code or assets; you
need a legitimately obtained copy of the game to use it.
