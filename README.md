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

Known issues: occasional stutter (graphics path not optimized yet; a Metal
renderer is planned), antialiasing options, and the native arm64 backend is
being brought up for the full game (all test suites pass on it).

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
| `DXH_THUNK_STATS=10`, `DXH_GL_STATS=1` | periodic performance statistics |
| `DXCPU_INTERP=1` | interpreter instead of JIT (debugging) |

## Tests

```sh
tests/guest/build.sh tests/guest/libc_file.c build/t/libc_file.o
build/arm64-dxcpu/dxhost --test build/t/libc_file.o
```

## Legal

This is an independent compatibility project, not affiliated with or endorsed by
Square Enix, Eidos-Montréal, Feral Interactive or Valve. "Deus Ex" is a trademark
of its respective owner. The repository contains no game code or assets; you
need a legitimately obtained copy of the game to use it.
