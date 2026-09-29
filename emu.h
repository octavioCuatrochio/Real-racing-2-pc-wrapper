/*
 * rr2emu - headless user-mode ARM emulator for Real Racing 2 (Android armeabi)
 *
 * Design: flat 4GB guest address space in one host reservation, guest pointer
 * is u32, host pointer = mem + guest. Interpreter with 4096-entry decode table
 * keyed on instr bits 27-20 | 7-4. Imported functions bound at relocation time
 * to magic slot addresses >= 0xF0000000; fetch divert calls host C directly.
 */
#ifndef EMU_H
#define EMU_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include "platform.h"
#include <math.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef float    f32;
typedef double   f64;

typedef u32 gptr;   /* every guest pointer; char* never crosses the boundary */

/* ---------------- logging ---------------- */

extern int g_verbose;
#define LOG(...)   do { fprintf(stderr, __VA_ARGS__); } while (0)
#define VLOG(l, ...) do { if (g_verbose >= (l)) fprintf(stderr, __VA_ARGS__); } while (0)
#define LOG_ONCE(...) do { static bool _once; if (!_once) { _once = true; \
    fprintf(stderr, __VA_ARGS__); } } while (0)

[[noreturn]] void fatal(const char *fmt, ...);

/* ---------------- guest memory ---------------- */

extern u8 *g_mem;   /* base of the 4GB guest space */

#define GUEST_NULL_LIMIT   0x00100000u   /* low 1MB stays PROT_NONE */
#define GUEST_HLE_DATA     0x08000000u   /* HLE data page zone (guest objects) */
#define GUEST_MMAP_BASE    0x09000000u   /* guest mmap arena */
#define GUEST_LIB_BASE     0x40000000u   /* .so load base */
#define GUEST_HEAP_BASE    0x44000000u   /* malloc arena / brk (libraries below: 64MB) */
#define GUEST_HEAP_MAX     0x5C000000u   /* TLSF heap limit */
#define GUEST_SBRK_BASE    0x5C000000u   /* raw sbrk window, 64MB below thread stacks */
#define GUEST_SBRK_MAX     0x60000000u
#define GUEST_STACK_TOP    0x70800000u   /* main stack top, grows down */
#define GUEST_STACK_SIZE   0x00800000u   /* 8MB */
#define GUEST_TSTACK_BASE  0x6F000000u   /* extra thread stacks grow down, 1MB each */
#define HLE_SLOT_BASE      0xF0000000u   /* magic HLE/JNI divert addresses */
#define HLE_SLOT_STRIDE    16u

static inline void *g2h(gptr p) { return (void *)(g_mem + p); }
static inline gptr  h2g(const void *p) { return (gptr)((const u8 *)p - g_mem); }

static inline u32 ld32(gptr a) { u32 v;  memcpy(&v, g_mem + a, 4); return v; }
static inline u16 ld16(gptr a) { u16 v;  memcpy(&v, g_mem + a, 2); return v; }
static inline u8  ld8 (gptr a) { return g_mem[a]; }
static inline void st32(gptr a, u32 v) { memcpy(g_mem + a, &v, 4); }
static inline void st16(gptr a, u16 v) { memcpy(g_mem + a, &v, 2); }
static inline void st8 (gptr a, u8  v) { g_mem[a] = v; }
static inline f32 ldf32(gptr a) { f32 v; memcpy(&v, g_mem + a, 4); return v; }
static inline f64 ldf64(gptr a) { f64 v; memcpy(&v, g_mem + a, 8); return v; }
static inline void stf32(gptr a, f32 v) { memcpy(g_mem + a, &v, 4); }
static inline void stf64(gptr a, f64 v) { memcpy(g_mem + a, &v, 8); }

static inline const char *gstr(gptr p) { return p ? (const char *)g2h(p) : "(null)"; }

/* bounds-checked translation for HLE; fatals on wild guest pointer */
void *gchk(gptr p, u32 len);

/* ---------------- CPU ---------------- */

typedef struct emu emu_t;

typedef struct cpu {
    u32 r[16];          /* r[13]=sp r[14]=lr r[15]=pc (pre-incremented by dispatcher) */
    u32 cpsr;           /* NZCV 31-28, Q 27, GE 19-16, T 5; user mode hardwired */
    /* VFP/NEON: d<n> aliases s<2n>,s<2n+1>; q<n> = d<2n>,d<2n+1>. q[] is a D register as u64 */
    union { u32 w[64]; f32 f[64]; f64 d[32]; u64 q[32]; u16 h[128]; u8 b[256]; } v;
    u32 fpscr;          /* NZCV 31-28, QC 27, DN 25, FZ 24, RMode 23-22 */
    u32 excl_addr;      /* ldrex/strex monitor: address, or ~0 when clear */

    int   tid;
    char  name[16];
    emu_t *emu;

    int   exit_loop;    /* set to break cpu_run */
    u32   span;         /* fetch window for the fast path; 0 forces the slow path */
    int   call_depth;   /* emu_call reentrancy depth */
    u32   call_ret;     /* r0 captured at magic return */
    u64   insn_count;
    void *jit_link;     /* JIT: exit slot to patch once its target is compiled */

    gptr  errno_addr;   /* per-thread guest errno word */
    gptr  tls_base;     /* fake TLS pointer for mrc p15,c13 reads */
    gptr  tls_values[128]; /* pthread_setspecific */

    gptr  stack_lo, stack_hi;   /* guard diagnostic */
    void *host_thread;          /* pthread_t, if a guest-spawned thread */
    int   detached;
    u32   thread_ret;

    /* trace ring of last executed PCs */
    u32   ring[64];
    int   ring_pos;
} cpu_t;

/* condition codes */
#define FLAG_N 0x80000000u
#define FLAG_Z 0x40000000u
#define FLAG_C 0x20000000u
#define FLAG_V 0x10000000u
#define FLAG_Q 0x08000000u
#define FLAG_T 0x00000020u   /* Thumb state */
#define FLAG_GE 0x000F0000u

static inline bool cond_ok(u32 cond, u32 cpsr)
{
    switch (cond & 0xF) {
    case 0x0: return  (cpsr & FLAG_Z);
    case 0x1: return !(cpsr & FLAG_Z);
    case 0x2: return  (cpsr & FLAG_C);
    case 0x3: return !(cpsr & FLAG_C);
    case 0x4: return  (cpsr & FLAG_N);
    case 0x5: return !(cpsr & FLAG_N);
    case 0x6: return  (cpsr & FLAG_V);
    case 0x7: return !(cpsr & FLAG_V);
    case 0x8: return  (cpsr & FLAG_C) && !(cpsr & FLAG_Z);
    case 0x9: return !(cpsr & FLAG_C) ||  (cpsr & FLAG_Z);
    case 0xA: return  !!(cpsr & FLAG_N) == !!(cpsr & FLAG_V);
    case 0xB: return  !!(cpsr & FLAG_N) != !!(cpsr & FLAG_V);
    case 0xC: return !(cpsr & FLAG_Z) && (!!(cpsr & FLAG_N) == !!(cpsr & FLAG_V));
    case 0xD: return  (cpsr & FLAG_Z) || (!!(cpsr & FLAG_N) != !!(cpsr & FLAG_V));
    default:  return true;   /* 0xE = AL; 0xF handled per-class */
    }
}

#define DEC_KEY(i) ((((i) >> 16) & 0xFF0u) | (((i) >> 4) & 0xFu))

typedef void (*handler_t)(cpu_t *c, u32 insn);
extern handler_t dec_table[4096];      /* generic handlers: the reference implementation */

/* pre-decoded instruction cache (dec.c): one di_t per text word */
typedef struct di di_t;
typedef void (JITCALL *dfn_t)(cpu_t *c, const di_t *d);
struct di {
    dfn_t h;
    u32   insn;
    u32   a, b;          /* handler-specific pre-computed operands */
    u8    cond, rd, rn, rm;
    u16   op;            /* fastops.h op id: computed-goto dispatch index (0 = decode) */
    u8    cx;            /* cond ^ 0xE: 0 = always, so an untouched zero page is "decode me" */
    u8    len;           /* 4 ARM / Thumb-2 wide, 2 Thumb narrow; 0 = not decoded yet */
    u16   b0op;          /* OP_hook: the fast op the hooked instruction would have used (0 = generic) */
};
extern di_t *g_icache;
extern u16 cond_tab[16];
extern u16 condx_tab[16];                        /* indexed by cx */
void cpu_icache_reset(void);
void cpu_add_hook(u32 pc);
extern int g_jit;                                /* 0: interpreter only */
void jit_reset(void);
bool jit_run(cpu_t *c, bool (*slow)(cpu_t *, u32));
bool jit_test_one(cpu_t *c, u32 pc, u32 insn);
bool jit_test_pair(cpu_t *c, u32 pc, u32 i1, u32 i2);
u32  jit_guest_pc_of(const void *host);
bool jit_owns(const void *host);                       /* debug: log registers when pc executes */                     /* after text range changes / code rewritten */
bool cpu_decode_one(di_t *out, u32 pc, u32 insn);
void cpu_decode_at(di_t *out, u32 pc, bool thumb);   /* decode guest code at pc (ARM or Thumb) */
void thumb_decode(di_t *d, u32 pc);                   /* thumb.c */
void thumb_it_reset(void);

#define EXIT_WATCH 0x100   /* exit_loop bit: re-arm the write watchpoint, keep running */
void watch_rearm(cpu_t *c);

void cpu_init_decode(void);
void cpu_run(cpu_t *c);

/* call a guest function from host; reentrant. args r0..r3 order, rest spilled. */
u32 emu_call(cpu_t *c, u32 fn, int argc, const u32 *args);

/* interworking branch (BX / LoadWritePC / ARM ALUWritePC): bit0 selects Thumb */
void cpu_branch(cpu_t *c, u32 addr);
static inline void cpu_set_pc(cpu_t *c, u32 addr)
{
    if (addr & 1) { c->cpsr |= FLAG_T; addr &= ~1u; } else c->cpsr &= ~FLAG_T;
    c->r[15] = addr;
}

[[noreturn]] void emu_trap(cpu_t *c, const char *fmt, ...);

/* ---------------- emulator top level ---------------- */

#define MAX_THREADS 64

typedef struct emu {
    u32   text_lo, text_span;    /* fetch validation */
    u32   lib_base, lib_size;
    u32   exidx_base; int exidx_count;

    cpu_t *threads[MAX_THREADS];
    int    nthreads;
    pthread_mutex_t threads_lock;

    /* options */
    const char *assets_dir;         /* RR2: OBB data root; RR3: extracted data root (sdcard/, internal/, apk/) */
    const char *apk_assets_dir;
    int   game;                     /* 2 = Real Racing 2, 3 = Real Racing 3 */
    const char *save_dir;           /* write overlay ("./save" for RR2) */
    int   max_frames;
    int   width, height;
    int   headless, vsync, fullscreen, aniso;
    int   no_assists, no_tilt, cockpit_fov;   /* game tweaks (patches.c) */
    float dpi;
    int   fake_clock;
    u64   fake_now_ns;
} emu_t;

extern emu_t G;
extern __thread cpu_t *tls_cpu;

/* ---------------- ELF loader ---------------- */

int  elf_load(emu_t *e, const char *path);
u32  elf_lookup(const char *name);       /* exported dynsym addr, 0 if none */
u32  elf_lookup_in(const char *lib, const char *name);
u32  elf_exidx_for(u32 pc, u32 *count);
const char *elf_lib_of(u32 addr, u32 *base);
void elf_run_init_array(cpu_t *c);
bool elf_run_init_lib(cpu_t *c, const char *name);
void elf_run_fini_array(cpu_t *c);

/* ---------------- HLE ---------------- */

typedef void (*hle_fn)(cpu_t *c);

void hle_init(void);
u32  hle_bind(const char *name);    /* slot address for an import (creates stub if unknown) */
bool hle_has(const char *name);     /* a real implementation is registered */
void hle_register(const char *name, hle_fn fn);
const char *hle_slot_name(u32 pc);
gptr hle_data_alloc(u32 size, u32 align);  /* carve guest memory in HLE data zone */
gptr guest_mmap_pages(u32 npages);
gptr guest_malloc(u32 n);
void guest_free(gptr p);
gptr guest_realloc(gptr p, u32 n);
u32  guest_usable_size(gptr p);
u64  guest_heap_in_use(void);
u64  guest_mmap_live(void);
gptr guest_memalign(u32 align, u32 n);
void guest_munmap_pages(gptr addr, u32 npages);

/* arg/return helpers (AAPCS soft-float: everything through core regs + stack) */
static inline u32 harg(cpu_t *c, int i) { return i < 4 ? c->r[i] : ld32(c->r[13] + 4 * (i - 4)); }
static inline void hret(cpu_t *c, u32 v) { c->r[0] = v; }
static inline void hret64(cpu_t *c, u64 v) { c->r[0] = (u32)v; c->r[1] = (u32)(v >> 32); }

/* 64-bit argument at AAPCS position: even-reg pairs, then 8-aligned stack */
typedef struct { cpu_t *c; int reg; u32 stk; } argwalk_t;
static inline void argwalk_init(argwalk_t *w, cpu_t *c) { w->c = c; w->reg = 0; w->stk = 0; }
static inline u32 argwalk_u32(argwalk_t *w) {
    if (w->reg < 4) return w->c->r[w->reg++];
    u32 v = ld32(w->c->r[13] + w->stk); w->stk += 4; return v;
}
static inline u64 argwalk_u64(argwalk_t *w) {
    if (w->reg & 1) w->reg++;               /* align to even register */
    if (w->reg + 1 < 4) { u32 lo = w->c->r[w->reg], hi = w->c->r[w->reg + 1]; w->reg += 2;
        return lo | ((u64)hi << 32); }
    if (w->reg < 4) w->reg = 4;             /* r3 partially used -> rest on stack */
    if (w->stk & 7) w->stk += 4;            /* 8-byte stack alignment */
    u64 v = ld32(w->c->r[13] + w->stk) | ((u64)ld32(w->c->r[13] + w->stk + 4) << 32);
    w->stk += 8; return v;
}

/* ---------------- subsystems ---------------- */

void libc_init(void);     /* registers libc/libm/liblog/libstdc++ HLE fns */
void sync_init(void);     /* pthreads, semaphores, atomics, cxa guards, setjmp */
void gles_init(void);     /* GLES/EGL stubs */
void jni_init(void);      /* fake JavaVM/JNIEnv */
u32  jni_env_ptr(void);   /* guest JNIEnv* */
u32  jni_vm_ptr(void);    /* guest JavaVM* */
u32  jni_activity(void);  /* the MainActivity jobject */
u32  jni_object(const char *cls);
u32  jni_class(const char *name);
u32  jni_take_video_completion(void);
void jni_push_frame(void);           /* local reference frame around a host->guest native call */
void jni_pop_frame(void);
u32  jni_string(const char *s);
void gles_tick_frame(void); /* FPS meter hook from eglSwapBuffers-equivalents */

/* host window / GL (host.c, glhost.c) */
typedef void (*host_input_fn)(int action, int x, int y);   /* 0 down, 1 up, 2 move */
bool  host_video_init(int w, int h, int vsync);
extern int g_gl_desktop;                 /* 1: desktop GL context, GLSL ES translated */
void *host_gl_proc(const char *name);
bool  host_present(void);
void  host_swap(void);
void  host_shutdown(void);
void  host_desktop_size(int *w, int *h);
bool  host_video_mode(int *w, int *h, int fullscreen, int vsync);
enum { MENU_NONE, MENU_UP, MENU_DOWN, MENU_LEFT, MENU_RIGHT, MENU_OK, MENU_BACK, MENU_QUIT,
       MENU_MOVE, MENU_CLICK, MENU_CHAR, MENU_ERASE, MENU_PASTE, MENU_DROP, MENU_RAW_KEY, MENU_RAW_PAD };
typedef struct { int x, y, key, pad, capture; char text[1024]; } menu_event_t;   /* capture: report raw key/pad */
enum { ACT_LEFT, ACT_RIGHT, ACT_GAS, ACT_BRAKE, ACT_CAMERA, ACT_BACK, ACT_COUNT };
extern int g_binds[ACT_COUNT][4];          /* [0..1] SDL keycodes (0 none), [2..3] pad button or 0x100+axis*2+neg (-1 none) */
extern const char *const act_names[ACT_COUNT];
void  host_binds_default(void);
const char *host_bind_name(int slot, int code);
int   host_menu_poll(int wait_ms, menu_event_t *ev);
void  host_text_input(int on);
float glhost_max_aniso(void);
bool  launcher_run(const char **so_path);
int   rr3_main(const char *so_path);
void  patches_setup(void);
int   inflate_raw(const u8 *in, size_t n, u8 *out, size_t outn);
void  patch_svc(cpu_t *c, u32 id);
typedef struct { float steer_target; int gas, brake; int back, camera; } host_input_t;
extern volatile int g_hide_next_tex;     /* set by the VFS when a HUD image is opened: 1 hidden, 2 race marker */
extern volatile int g_race_hud_drawn;    /* set by glhost when the race HUD marker is drawn */
extern host_input_t g_input;
void  host_set_input(host_input_fn fn);
bool  host_audio_open(int rate, int channels);
void  host_audio_write(const void *pcm, u32 bytes);
bool  glhost_init(void);
bool  glhost_screenshot(const char *path, int w, int h);
void  glhost_report(long frames);

cpu_t *emu_new_cpu(void);
bool   guest_async_call(u32 fn, int argc, const u32 *args, volatile int *done);
void   emu_free_cpu(cpu_t *c);
gptr   emu_guest_scratch(cpu_t *c);   /* per-thread guest word block (errno etc) */

#endif
