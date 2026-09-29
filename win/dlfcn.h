/* win/dlfcn.h - dlopen/dlsym on top of LoadLibrary (see win32.c) */
#ifndef WIN_DLFCN_H
#define WIN_DLFCN_H
#define RTLD_NOW   2
#define RTLD_LOCAL 0
void       *dlopen(const char *name, int flags);
void       *dlsym(void *lib, const char *sym);
const char *dlerror(void);
#endif
