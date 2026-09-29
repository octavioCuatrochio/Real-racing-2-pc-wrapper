# Real Racing 2 PC wrapper (rr2emu)

Run the Android version of **Real Racing 2** natively on **Linux** and **Windows**.

`rr2emu` is a small, performance-focused ARM user-mode emulator built for this one game. It loads the game's own native library (`libEAMRealRacing2.so`, armeabi / ARMv5 + VFP) and runs it with:

- a pre-decoded ARM interpreter plus an **ARM → x86-64 block JIT**
- high-level emulation of Android: Bionic libc, pthreads, malloc and a fake JNI / Java layer, so no Android system image is needed
- **GLES2 passed straight to the host GPU**, with Adreno ATC textures transcoded to S3TC
- SDL2 for the window, keyboard, gamepad and audio
- a Gran Turismo 4-style **launcher** for game files, video options, gameplay tweaks and control remapping

It boots to the menus, races with correct lighting, has input, audio and saving, and holds the game's own 60 fps cap.

> **Game files are not included.** You need your own copy of the Real Racing 2 APK and its data (OBB / cache zip).

## Download

Prebuilt packages are on the [Releases page](https://github.com/octavioCuatrochio/Real-racing-2-pc-wrapper/releases):

| Package | Contents |
|---|---|
| `rr2emu-windows-x64.zip` | `rr2emu.exe`, `SDL2.dll`, this README. Unzip anywhere and run `rr2emu.exe`. |
| `rr2emu-linux-x64.tar.gz` | the `rr2emu` binary and this README. Needs the runtime libraries listed below. |

Every push is built and self-tested on Linux and on a real Windows machine by GitHub Actions; tags named `v*` publish a release.

The Windows build isn't code-signed, so SmartScreen may warn on first run ("More info" → "Run anyway").

---

## Requirements

### Hardware

| | Minimum | Tested on |
|---|---|---|
| CPU | x86-64 with SSE2 | AMD Ryzen APU (Raven Ridge) |
| GPU | OpenGL ES 2.0, or desktop OpenGL 2.1 (used automatically when the driver has no ES) | AMD Radeon Vega 8, Mesa 25.2 (radeonsi) |
| RAM | ~1 GB free | |
| Disk | ~1.5 GB for the unpacked game data | |

S3TC texture support (`GL_EXT_texture_compression_s3tc`, present on all desktop GPUs) is used when available. Otherwise textures are decoded to RGBA8, which uses more VRAM.

### Software

**Windows:** Windows 10 or 11, 64-bit. Nothing to install: `SDL2.dll` ships in the zip and the launcher uses the system's Arial font. Rendering uses the GPU driver's OpenGL ES 2 context when it offers one (most NVIDIA and AMD drivers); drivers without one (often Intel iGPUs) get a desktop OpenGL 2.1 context with the game's shaders translated automatically.

**Linux:** x86-64, tested on Ubuntu 24.04. All libraries are loaded at run time with `dlopen`, so **no `-dev` packages are needed to build**.

| Library | Needed for | Ubuntu / Debian package |
|---|---|---|
| SDL2 (`libSDL2-2.0.so.0`) | window, GL context, input, audio | `libsdl2-2.0-0` |
| GLES2 / EGL or GL driver | rendering | `libgles2`, `libegl1` (Mesa) |
| FreeType (`libfreetype.so.6`), optional | launcher text; falls back to a built-in pixel font | `libfreetype6` |
| Nimbus Sans / Liberation Sans / DejaVu Sans, optional | launcher font | `fonts-urw-base35`, `fonts-liberation` or `fonts-dejavu-core` |

The APK / OBB unpacking uses a built-in DEFLATE decoder, so zlib isn't needed on either platform.

### Game files

| File | Notes |
|---|---|
| Real Racing 2 APK | the build tested here is `Real-Racing-2-v0008512.apk` (package `com.ea.game.realracing2_OTD_row`); it must contain `lib/armeabi/libEAMRealRacing2.so` |
| Game data | the OBB / cache zip (`Real-Racing-2-v000851-cache2.zip`), whose root holds `com.ea.game.realracing2_OTD_row/` |

The in-game patches check every instruction word before changing it. On a different game build they simply don't apply.

#### Real Racing 3 (experimental)

Set **Game** to Real Racing 3 in the launcher. **DOWNLOAD** fetches Real Racing 3 14.0.1 (APK, 105 MB, and the Adreno game data, 6.2 GB, 9 GB unpacked) from the Internet Archive, as listed by [Project_RR3](https://viduxsh.github.io/Project_RR3/#downloads). It shows progress in the launcher and in desktop notifications, resumes if interrupted (Esc pauses), checks every file's CRC while unpacking, and saves both paths. Files go to `~/.local/share/rr2emu/rr3-14.0.1` (Windows: `%LOCALAPPDATA%\rr2emu\rr3-14.0.1`). The game data is also mirrored on [Mega](https://mega.nz/file/Q2ggiKSR#GZ1CXOUCs4NoHBtOiGEWOT1wau1ebhBoz6yWWWxfQyI) for downloading by hand.

We do not own Real Racing 3 or any of its files; they belong to Electronic Arts and Firemonkeys Studios. The download is offered only because the game has been shut down, so that people can keep playing it.

The 14.0.1 build does not run in rr2emu yet; 7.6.0 does.

---

## Build

You need a C11 compiler and `make` (e.g. `build-essential`).

### Linux

```sh
git clone https://github.com/octavioCuatrochio/Real-racing-2-pc-wrapper.git
cd Real-racing-2-pc-wrapper
make                 # produces ./rr2emu, tuned for this CPU (-march=native)
./rr2emu --selftest  # CPU/VFP self-tests + interpreter/JIT fuzzing (should print "0 failed")
```

`make release` builds a portable binary that runs on any x86-64 CPU. The JIT only emits baseline SSE2, so the only cost is the C code's own tuning.

### Windows (cross-compiled from Linux)

The Windows build uses [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), a self-contained clang toolchain: unpack a release and put its `bin/` on `PATH`.

```sh
make win WINCC=x86_64-w64-mingw32-clang   # produces rr2emu.exe (statically linked)
```

Put [SDL2.dll](https://github.com/libsdl-org/SDL/releases) (the `win32-x64` zip) next to `rr2emu.exe`. The GitHub Actions workflow in `.github/workflows/build.yml` does exactly this and runs the self-test on Windows.

Other targets: `make asan` (AddressSanitizer build; needs clang), `make clean`.

---

## Run

```sh
./rr2emu
```

This opens the **launcher**:

- **Game APK**: the `.apk` file, or a folder already extracted from it (containing `lib/armeabi/` and `assets/`).
- **Game data (OBB)**: the OBB / cache `.zip`, or a folder containing `com.ea.game.realracing2_*/`.

  Drag and drop files onto the window, or select the row, press Enter and type or paste (Ctrl+V) a path. Archives are unpacked once into `~/.cache/rr2emu/` (Windows: `%LOCALAPPDATA%\rr2emu\`) and reused after that.
- **Display**: resolution (up to your desktop size), fullscreen, anisotropic filtering (up to 16×) and VSync.
- **Gameplay**:
  - **Disable assists**: forces steering assist, brake assist and anti-skid off.
  - **Horizon tilt → Off**: keeps the horizon level (no camera roll while steering).
  - **Cockpit FOV**: −10° to +40° added to the interior camera. Above about +30° you start to see the edges of the car interior model.
- **Controls**: remap every action. Each has two keyboard bindings and two controller bindings; sticks and triggers stay analog for steering and pedals.

Every change is saved to `~/.config/rr2emu.cfg` (Windows: `%APPDATA%\rr2emu\rr2emu.cfg`) right away, and **START** boots the game.

**Save games** are written to `./save/` in the directory you run `rr2emu` from, so start it from the same place each time. On Windows that's the folder containing `rr2emu.exe` when you double-click it.

On Windows, `rr2emu.exe` logs to `rr2emu.log` next to it, or to the console when started from one.

### In-game setup

For keyboard or gamepad play, choose **Method B** (tilt steering, manual pedals) in the game's *Options → Controls*. Steering is fed to the game as device tilt, and the pedals are virtual touch zones whose HUD images are hidden.

### Default controls

| Action | Keyboard | Controller (PlayStation names) |
|---|---|---|
| Steer | ← → / A D | Left stick / D-pad |
| Gas | ↑ / W | R2 / Cross |
| Brake | ↓ / S | L2 / Square |
| Change camera (in races) | C | Triangle |
| Back / pause | Esc / Backspace | Circle / Share |
| Touch | mouse | |

### Command line

Everything in the launcher can also be set from the command line, which skips the launcher:

```sh
./rr2emu --no-launcher --size 1920x1080 --fullscreen --aniso 16 \
         --no-assists --no-tilt --cockpit-fov 20 \
         --assets /path/to/data --apk-assets /path/to/apk/assets \
         /path/to/apk/lib/armeabi/libEAMRealRacing2.so
```

Run `./rr2emu --help` for the full list, which includes scripting (`--tap`, `--steer`, `--shot`, `--max-frames`) and debugging (`--nojit`, `--hook`, `--watch`, `--prof`, `-v`/`-vv`/`--trace`).

---

## Specs and performance

| | |
|---|---|
| Guest CPU | ARMv5TE + VFPv2 (armeabi), user mode |
| Execution | block JIT to x86-64 (default); computed-goto interpreter with a pre-decoded cache (`--nojit`) |
| JIT throughput | ~1300 MIPS on the bench loop (interpreter ~280 MIPS) |
| In race | steady ~60–62 fps (the game's own limiter), main emulated thread ~50% busy |
| Graphics | GLES2 passthrough; ATC → S3TC transcoding; filtering of redundant GL state (per-program uniform shadow, binds, enables); `glGetError` answered locally |
| Audio | the game's AudioTrack (mono, 44.1 kHz) queued to SDL with about 50 ms of buffering |
| Memory | flat 4 GB guest address space; TLSF guest heap; guest `munmap` returns pages to the OS |

Correctness is checked by `--selftest`, which fuzzes about 200k random instructions through each fast path and through single-instruction JIT blocks, comparing them against the reference ARM/VFP implementation.

---

## Source layout

| File | Contents |
|---|---|
| `main.c` | boot sequence (mirrors the APK's Java lifecycle), frame loop, input feed, CLI |
| `jit.c` | ARM → x86-64 block JIT |
| `cpu.c`, `dec.c`, `fastops.h` | decoder, pre-decoded icache, fast interpreter paths |
| `arm.c`, `vfp.c` | reference ARM and VFP instruction semantics |
| `elf.c`, `mem.c` | ELF loader and relocations, guest memory |
| `hle.c`, `hle_libc.c`, `hle_malloc.c`, `hle_sync.c` | Bionic libc, file system, malloc, pthreads |
| `jni.c` | fake JavaVM/JNIEnv and the Java-side behaviour the game relies on |
| `glhost.c`, `gles.c` | GLES2 passthrough to the host, and headless stubs |
| `host.c` | SDL2 window, GL context (ES or desktop fallback), input and remapping, audio (loaded with `dlopen`) |
| `launcher.c` | the launcher UI, config file, APK / OBB unpacking |
| `inflate.c` | DEFLATE decoder for the APK / OBB zips |
| `platform.h`, `win32.c`, `win/` | the host differences: on Windows, lazily committed guest memory, `dlopen` / `pread` / `rename` shims, crash handling |
| `patches.c` | optional game patches (assists, horizon tilt, cockpit FOV) |
| `test.c` | self-tests and benchmark |
| `NOTES.md` | developer notes: internals, debug switches, reverse-engineering findings |

---

## Disclaimer

This is an unofficial, non-commercial compatibility project with no affiliation with Electronic Arts or Firemonkeys. Real Racing 2 and its assets belong to their respective owners. This repository contains only the emulator's source code and no game code or data; use it with a copy of the game you legitimately own.
