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

## Extracted data: v7.6.0 (Keroxea edition, Adreno)

`../rr3/v7.6.0/` was extracted from the VPhoneOS backup, `androidfs_7.1.2/data`:

- `sdcard/`: the app's external data folder, `Android/data/com.ea.games.r3_row/` (4.2 GB, ATC textures). It includes `files/.depot/`
  (the downloaded assets) and `files/doc/` (the save).
- `internal/`: the app's private data folder, `/data/data/com.ea.games.r3_row` (shared_prefs, Nimble persistence).
- `apk/`: the installed `base.apk` and `lib/arm/`.

The data belongs to **v7.6.0**, so the target is `apk/lib/arm/libRealRacing3.so` and not the 14.0.1 APK. That library is ARMv7,
Thumb-2, VFPv3 and NEONv1. It needs only libNimble, libfmodex, libc++_shared, GLESv2/EGL, libz, liblog, libc, libm, libstdc++ and libdl,
with no ARCore or fuelmetrics.

How the VPhoneOS store is laid out:
- File contents are stored as `data/%08x/%08x`, where the folder is `inode>>12` and the file name is the inode.
- `data/fscache.bin` is a journal of packed `ATIT` records. A type-1 record holds ino at +0x14, mode at +0x24 and xattrs; for a
  directory it also embeds a listing, `ATIT n` followed by n × {u64 ino, u32 mode, u32 namelen, name padded to 8 bytes}.

About 60 directories have no parent listing in the journal. Their files were placed using `asset_list_base.txtCache.txt`
(path and MD5 of every asset): first by MD5, then by filename plus the nearest inode among files known to be in that directory.
That placement is certain for the per-track `.m3g` meshes. It is a best guess for layouts that share a filename (`*.pvs`,
`sprites.android.atlas`). 7,345 listed assets are present but have a different MD5 from the stock list, which is expected for a modded build.
