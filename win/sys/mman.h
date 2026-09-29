/* win/sys/mman.h - anonymous mmap/mprotect/madvise on top of VirtualAlloc (see win32.c) */
#ifndef WIN_SYS_MMAN_H
#define WIN_SYS_MMAN_H
#include <stddef.h>

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_FAILED ((void *)-1)
#define MADV_DONTNEED 4

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off);
int   munmap(void *addr, size_t len);
int   mprotect(void *addr, size_t len, int prot);
int   madvise(void *addr, size_t len, int advice);
int   mincore(void *addr, size_t len, unsigned char *vec);
#endif
