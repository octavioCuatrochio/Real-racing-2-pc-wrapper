/*
 * platform.h - the few host differences between Linux and Windows (MinGW).
 * Windows gets small POSIX shims from win/ and win32.c.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <fcntl.h>
#include <sys/stat.h>
#include <stddef.h>
#include <stdio.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#ifdef _WIN32
/* the JIT calls C with the System V register convention on every host */
#define JITCALL __attribute__((sysv_abi))
#define SDL_LIBNAME "SDL2.dll"
#define PATH_SEP_CHARS "/\\"
int     emu_mkdir(const char *path, int mode);
char   *emu_realpath(const char *path);                 /* malloc'd absolute path or NULL */
long    emu_pread(int fd, void *buf, size_t n, long long off);
int     emu_rename(const char *from, const char *to);   /* replaces an existing target like POSIX */
void    emu_prefault(void *p, size_t n);                /* commit guest pages before the kernel touches them */
void    win_init(void);
#else
#include <unistd.h>
#include <stdlib.h>
#define JITCALL
#define SDL_LIBNAME "libSDL2-2.0.so.0"
#define PATH_SEP_CHARS "/"
static inline int   emu_mkdir(const char *path, int mode) { return mkdir(path, (mode_t)mode); }
static inline char *emu_realpath(const char *path) { return realpath(path, NULL); }
static inline long  emu_pread(int fd, void *buf, size_t n, long long off) { return (long)pread(fd, buf, n, (off_t)off); }
static inline int   emu_rename(const char *from, const char *to) { return rename(from, to); }
static inline void  emu_prefault(void *p, size_t n) { (void)p; (void)n; }
#endif

#endif
