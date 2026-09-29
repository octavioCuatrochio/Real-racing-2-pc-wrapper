CC      ?= cc
CFLAGS  ?= -O2 -g -march=native -D_GNU_SOURCE -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fno-strict-aliasing
LDLIBS   = -lpthread -lm -ldl -rdynamic

# release builds: no -march=native, so they run on any x86-64 CPU (the JIT only emits baseline SSE2)
RELEASE_CFLAGS = -O2 -D_GNU_SOURCE -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fno-strict-aliasing

SRCS = main.c mem.c cpu.c arm.c arm7.c dec.c thumb.c neon.c jit.c vfp.c elf.c hle.c hle_libc.c hle_malloc.c hle_sync.c jni.c gles.c glhost.c host.c patches.c launcher.c inflate.c win32.c test.c difftest.c rr3.c stbtt.c
OBJS = $(SRCS:.c=.o)

rr2emu: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

hle_libc.o build-win/hle_libc.o: hle_libc2.inc
jni.o build-win/jni.o: jni_rr3.inc jni_rr3_text.inc

%.o: %.c emu.h fastops.h vfpops.h platform.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Windows build (cross, e.g. llvm-mingw or mingw-w64): make win WINCC=x86_64-w64-mingw32-clang
WINCC   ?= x86_64-w64-mingw32-gcc
WINFLAGS = -O2 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -std=c11 -Wall -Wno-unused-parameter -Wno-missing-field-initializers \
           -fno-strict-aliasing -Iwin
WINOBJS  = $(SRCS:%.c=build-win/%.o)

win: rr2emu.exe

rr2emu.exe: $(WINOBJS)
	$(WINCC) $(WINFLAGS) -mwindows -static -o $@ $(WINOBJS) -lpthread -lwinmm -lgdi32

build-win/%.o: %.c emu.h fastops.h vfpops.h platform.h
	@mkdir -p build-win
	$(WINCC) $(WINFLAGS) -c -o $@ $<

release:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(RELEASE_CFLAGS)" rr2emu

selftest: rr2emu
	./rr2emu --selftest

asan:
	$(MAKE) clean
	$(MAKE) CC=clang CFLAGS="-O1 -g -std=c11 -Wall -fno-strict-aliasing -fsanitize=address" rr2emu

clean:
	rm -f $(OBJS) rr2emu rr2emu.exe
	rm -rf build-win

.PHONY: selftest clean asan win release
