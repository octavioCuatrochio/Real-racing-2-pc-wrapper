# Real Racing 3 feasibility (branch `rr3`)

APK: `com.ea.games.r3_row` 14.0.1 (14001), minSdk 26, libs for arm64-v8a and armeabi-v7a.

## Native code (armeabi-v7a)

- `libRealRacing3.so`: 23.6 MB, 17.7 MB of `.text`, about 6.5M instructions. It is ARMv7-A, **mostly Thumb-2**
  (91 of 124 exported functions are Thumb), VFPv3-D16, and has about 60k NEON load/store instructions.
- Needs `libc++_shared`, `libNimble`, `libfmodex` (audio), `libfuelmetrics`, `libarcore_sdk_c/_jni`, plus libc, libm, libdl,
  libz, liblog, libandroid, EGL and GLESv2. It has 739 imports, including sockets, getaddrinfo, select/poll and POSIX semaphores.
- rr2emu decodes ARMv5 ARM-state instructions and VFPv2 only. Running RR3 needs:
  - a Thumb-2 decoder, interpreter and JIT
  - ARMv6/v7 instructions (ldrex/strex, movw/movt, ubfx/bfi, rev, etc.)
  - VFPv3 and NEON
  - loading several guest ELF libraries and linking them together
  - C++ exception unwinding via `.ARM.exidx`

## Game data

The APK holds **no game assets**; there is no OBB. At first launch `AssetDownloadService` / `CC_AssetManager` downloads the
tracks, cars and textures (`*.atc.dds.z`) from EA's CDN and Cloudcell servers, and the Java Nimble/Synergy SDK
(5 dex files, about 36 MB) handles login and the asset lists. Without a copy of that downloaded data, taken from a device's
`Android/data/com.ea.games.r3_row/`, the game can go no further than the download screen.
