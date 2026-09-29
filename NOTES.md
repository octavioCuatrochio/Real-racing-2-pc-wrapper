# rr2emu notes

## Run
    make && ./rr2emu                       # launcher: game files + video options, then the game
    ./rr2emu --selftest                    # CPU/VFP checks + fast-path/JIT fuzz
    ./rr2emu --bench                       # interpreter MIPS on a fixed loop
    ./rr2emu --no-launcher --assets ../extracted/data --apk-assets ../extracted/assets \
             ../extracted/lib/armeabi/libEAMRealRacing2.so
Launcher (launcher.c, GT4-styled, keyboard/pad/mouse): Game APK takes the .apk or an extracted
folder (lib/armeabi + assets/); Game data takes the OBB/cache .zip or a folder holding
com.ea.game.realracing2_*/. Archives are unpacked once to ~/.cache/rr2emu/<kind>-<size>-<mtime>
(zlib via dlopen). Files can be dropped on the window, typed or pasted (Ctrl+V). Resolution
(up to the desktop size), fullscreen (native size = borderless desktop, else a mode switch),
anisotropic filtering (forced on mipmapped textures, overrides the game's own level), vsync,
the gameplay tweaks and the control bindings are saved to ~/.config/rr2emu.cfg on every change.
Gameplay tweaks (patches.c, host patch points: the game word becomes `svc #0x52nnnn` and
patch_svc emulates it): "Disable assists" forces steering assist, anti-skid and brake assist off
and "Horizon tilt: Off" forces the camera roll off, both applied right after the profile loads
(+0xe8c70; profile fields +0x1871d tilt, +0x1871e steer assist, +0x18756 anti-skid, +0x18738
brake assist float) so the game saves them back; "Cockpit FOV" adds degrees to the race camera's
FOV when the interior camera (racecam+0x1840 == 1) is active (+0x106924), and while it is active
pulls the near plane (racecam+0x1844, normally 20; far at +0x1848 = 32768) in to 4 so the cabin
isn't cut open at wide FOVs; the game's value is restored for the other cameras. CLI: --no-assists,
--no-tilt, --cockpit-fov D. Controls page: 2 keyboard + 2 controller bindings per action
(PlayStation names; sticks/triggers analog for steering/pedals). Change camera taps the HUD camera
button (top right; first tap shows it) and only acts while the race HUD is drawn. Text uses FreeType + Nimbus/Liberation/DejaVu Sans when
present, else a built-in pixel font. Skipped by --no-launcher, --headless, --max-frames, --shot;
CLI equivalents: --size WxH, --fullscreen, --aniso N, --vsync. RR2_LAUNCHER_SHOT=file.png dumps
each launcher frame.
`extracted/data` is Real-Racing-2-v000851-cache2.zip unpacked. Useful flags: `--headless`,
`--tap N:X,Y`, `--shot N:FILE.png`, `--stats`, `--watch ADDR`, `-v/-vv/--trace`.
Debug env: `RR2_DUMP_SHADERS=dir` (plaintext shaders), `RR2_DBG_ROAD=0..6` (road shader inputs).

## Layout
- jit.c: ARM -> x86-64 block JIT (default). Hot ALU/load-store/ldm-stm/branch/VFP-single forms
  are native; everything else calls the interpreter's handler. Blocks chain via patchable
  8-byte slots; returns do an inline table lookup. `--nojit` (and any debug mode) interprets.
- cpu.c / dec.c / fastops.h: pre-decoded icache + computed-goto interpreter; arm.c/vfp.c are the
  generic reference handlers. The selftest fuzzes fast ops AND single-instruction JIT blocks
  against them (~200k random instructions each).
- hle_*.c: Bionic libc/pthread/malloc (TLSF heap). jni.c: fake JavaVM/JNIEnv + Java behaviour.
- host.c: SDL2 via dlopen (window, GLES2 context, input, audio). glhost.c: GLES2 passthrough,
  ATC textures decoded to RGBA8.
- Boot order mirrors the APK's Java (see main.c). Java side studied with a small dex dumper.

## Controls (Method B: tilt steer, manual pedals — chosen in Options > Controls)
Defaults (remappable in the launcher): arrows/A-D steer (fed as accelerometer tilt), Up/W gas,
Down/S brake, C camera, Esc back. Pad: L-stick/D-pad steer, R2/Cross gas, L2/Square brake,
Triangle camera, Circle back. Mouse = touch. The game's Quit calls RealRacing2Activity.exitApp,
which ends the process (the game saves first).
Pedals are touch zones (gas bottom-right, brake bottom-left); their HUD images are hidden
(RR2_HIDE=list of image name prefixes, empty to show). Settings save on race start and on quit
(quit runs NativeOnPause on an emulated UI thread so the game can write SaveGameData.ini).

## Performance notes
- ATC textures are transcoded to S3TC (DXT1/3/5, same size) when the host has it; else RGBA8.
- glhost filters redundant GL state (uniform shadow per program, binds, enables, glUseProgram);
  glGetError is answered locally (lets Mesa's glthread run ahead).
- JIT fuses cmp/tst + conditional b into one x86 op + jcc and skips building ARM flags when a
  bounded scan shows both successors overwrite them (calls/returns count as overwrites per AAPCS).
- HLE calls are made from JIT code directly; guest munmap returns pages (MADV_DONTNEED);
  the interpreter's decode cache is never pre-touched.
- Audio: the game's AudioTrack is MONO 44.1 kHz (Java uses CHANNEL_CONFIGURATION_MONO); writes
  block above ~50 ms queued. RR2_AUDIO_DEBUG=1 prints per-second rate/underruns.
- Toggles: RR2_NO_S3TC, RR2_NO_GLCACHE, RR2_JIT_SAFEFLAGS, RR2_AUDIO_RATE=22050 (cheaper mixer).
- Measure with --prof (main-thread samples, RR2_PROF_DUMP=file for raw buckets).

## Windows port
- Build: `make win WINCC=x86_64-w64-mingw32-clang` (llvm-mingw), static, GUI subsystem; needs SDL2.dll.
- JIT ABI: generated code and its C callees use System V (`JITCALL` = `__attribute__((sysv_abi))` on
  dfn_t handlers, d_generic/d_hook/d_decode, jit_hle_call and the entry stub), so jit.c is unchanged.
- Memory: win/sys/mman.h shims reserve with VirtualAlloc; a vectored exception handler commits 64 KB
  on first touch (Linux MAP_NORESERVE semantics), madvise(DONTNEED) decommits. The kernel does NOT
  fault pages in during I/O (ReadFile into an uncommitted page just fails), so host I/O on guest memory
  calls emu_prefault() first (read/write/fread/fwrite/fgets/pread/asset reads). Missing that made a
  texture read fail and the game allocate 618 MB from a garbage header.
- GL: SDL asks for GLES2; without WGL_EXT_create_context_es2_profile it falls back to a desktop GL 2.1
  context (RR2_GL=desktop|es forces) and glhost rewrites GLSL ES to `#version 120` (precision
  qualifiers defined away, ES-only #extension lines dropped), reports ES version strings, enables
  program point size / point sprites, and emulates glClearDepthf/glDepthRangef if needed.
- Files: guest O_* bits are rebuilt for the host (+O_BINARY), guest fopen modes get "b", d_type comes
  from stat, rename replaces existing targets, stat's blksize/blocks are synthesized.
- Launcher: GDI rasterizes Arial; config in %APPDATA%\rr2emu, cache in %LOCALAPPDATA%\rr2emu.
- timeBeginPeriod(1) so the game's 1 ms sleeps don't round to 15.6 ms. --prof is Linux-only.
- Tested with Wine 11.18: selftest passes; the game boots and renders through the desktop GL path.

## Debug tools
`--hook PC` logs registers when a guest PC executes (RR2_HOOK_DUMP=reg:off dumps memory),
`--watch ADDR` logs writes, `--stats` shows generic-path instruction mix.

## Status
Boots, menus, races with correct lighting, input, audio, saves.
Race: steady ~62 fps (the game's own 60 fps limiter), main thread ~50% busy.
Bench: JIT ~1300 MIPS vs interpreter ~280. Crash dumps are less detailed under the JIT
(no branch ring); reproduce with --nojit for full diagnostics.
Bugs fixed that looked like "graphics" problems: sscanf %Nc (lightmaps never loaded),
Bionic struct stat layout (st_size at offset 48).
