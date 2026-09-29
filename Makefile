CC      ?= cc
CFLAGS  ?= -O2 -g -march=native -D_GNU_SOURCE -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fno-strict-aliasing
LDLIBS   = -lpthread -lm -ldl -rdynamic

SRCS = main.c mem.c cpu.c arm.c dec.c jit.c vfp.c elf.c hle.c hle_libc.c hle_malloc.c hle_sync.c jni.c gles.c glhost.c host.c patches.c launcher.c test.c
OBJS = $(SRCS:.c=.o)

rr2emu: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c emu.h fastops.h
	$(CC) $(CFLAGS) -c -o $@ $<

selftest: rr2emu
	./rr2emu --selftest

asan:
	$(MAKE) clean
	$(MAKE) CC=clang CFLAGS="-O1 -g -std=c11 -Wall -fno-strict-aliasing -fsanitize=address" rr2emu

clean:
	rm -f $(OBJS) rr2emu

.PHONY: selftest clean asan
