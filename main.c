/*
 * main.c - rr2emu driver: CLI, boot sequence, frame loop.
 */
#include "emu.h"
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>

long g_frame;
static u32 g_watch;         /* guest address of a debug write watchpoint, 0 = off */
static int g_watch_read;    /* 1: log reads (page PROT_NONE) instead of writes */
static struct { u32 pc, n; } g_readers[64];

static void watch_protect(int prot)
{
    mprotect(g_mem + (g_watch & ~4095u), 4096, prot);
}
static int watch_armed_prot(void) { return g_watch_read ? PROT_NONE : PROT_READ; }

/* arm at runtime (from a --hook): read or write watch on one word */
void watch_arm(u32 addr, int read)
{
    if (g_watch) return;
    g_watch = addr & ~3u;
    g_watch_read = read;
    LOG("[watch] armed %s watch at %08x\n", read ? "read" : "write", g_watch);
    watch_protect(watch_armed_prot());
}

void watch_rearm(cpu_t *c)
{
    if (c->exit_loop & 0x200) { c->exit_loop &= ~0x200; watch_protect(watch_armed_prot()); return; }
    u32 pc = c->r[15] - 4;
    if (g_watch_read) {
        int i;
        for (i = 0; i < 64 && g_readers[i].pc && g_readers[i].pc != pc; i++) {}
        if (i < 64) { if (!g_readers[i].pc) LOG("[watch] new reader pc %08x (lr %08x)\n", pc, c->r[14]); g_readers[i].pc = pc; g_readers[i].n++; }
    } else
        fprintf(stderr, "[watch] %08x = %08x (tid %d, pc %08x, lr %08x)\n",
                g_watch, ld32(g_watch), c->tid, pc, c->r[14]);
    watch_protect(watch_armed_prot());
}

/* --prof: CPU-time sampling of the main thread; buckets by JIT guest pc or host symbol (Linux) */
#ifndef _WIN32
#include <dlfcn.h>
#include <ucontext.h>
#define PROF_N 8192
static struct { uintptr_t key; u32 n; } g_prof[PROF_N];
static u32 g_prof_total, g_prof_jit;
static void on_prof(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = ucv;
    void *rip = (void *)uc->uc_mcontext.gregs[REG_RIP];
    uintptr_t key;
    if (jit_owns(rip)) { key = 1 | ((uintptr_t)jit_guest_pc_of(rip) << 1); g_prof_jit++; }
    else key = (uintptr_t)rip & ~(uintptr_t)1;     /* resolved at the end */
    g_prof_total++;
    u32 h = (u32)((key * 0x9E3779B97F4A7C15ULL) >> 51) & (PROF_N - 1);
    for (u32 k = 0; k < PROF_N; k++, h = (h + 1) & (PROF_N - 1))
        if (g_prof[h].key == key || !g_prof[h].key) { g_prof[h].key = key; g_prof[h].n++; return; }
}
static void prof_start(void)
{
    struct sigaction sa = { .sa_sigaction = on_prof, .sa_flags = SA_SIGINFO | SA_RESTART };
    sigaction(SIGPROF, &sa, NULL);
    timer_t t;
    struct sigevent ev = { .sigev_notify = SIGEV_THREAD_ID, .sigev_signo = SIGPROF };
    ev._sigev_un._tid = gettid();
    timer_create(CLOCK_THREAD_CPUTIME_ID, &ev, &t);
    struct itimerspec it = { { 0, 500000 }, { 0, 500000 } };   /* every 0.5 ms of CPU */
    timer_settime(t, 0, &it, NULL);
}
static void prof_report(void)
{
    /* fold host samples to their symbol / library */
    static struct { char name[96]; u32 n; } agg[512];
    int na = 0;
    for (int i = 0; i < PROF_N; i++) {
        if (!g_prof[i].n) continue;
        char name[96];
        if (g_prof[i].key & 1) snprintf(name, sizeof(name), "JIT guest %08x", (u32)(g_prof[i].key >> 1));
        else {
            Dl_info di;
            if (dladdr((void *)g_prof[i].key, &di) && di.dli_sname) snprintf(name, sizeof(name), "%s", di.dli_sname);
            else if (di.dli_fname && strstr(di.dli_fname, "rr2emu"))   /* static fn: offset for addr2line */
                snprintf(name, sizeof(name), "@%lx", (unsigned long)((uintptr_t)g_prof[i].key - (uintptr_t)di.dli_fbase));
            else if (di.dli_fname) { const char *b = strrchr(di.dli_fname, '/'); snprintf(name, sizeof(name), "[%s]", b ? b + 1 : di.dli_fname); }
            else snprintf(name, sizeof(name), "?");
        }
        int j;
        for (j = 0; j < na && strcmp(agg[j].name, name); j++) {}
        if (j == na && na < 512) { snprintf(agg[na].name, sizeof(agg[0].name), "%s", name); agg[na++].n = 0; }
        if (j < 512) agg[j].n += g_prof[i].n;
    }
    LOG("[prof] %u samples, %.1f%% in JIT code\n", g_prof_total, 100.0 * g_prof_jit / (g_prof_total + !g_prof_total));
    const char *dump = getenv("RR2_PROF_DUMP");            /* raw buckets for offline aggregation */
    FILE *df = dump ? fopen(dump, "w") : NULL;
    for (int i = 0; df && i < PROF_N; i++) {
        if (!g_prof[i].n) continue;
        Dl_info di;
        if (g_prof[i].key & 1) fprintf(df, "jit %08x %u\n", (u32)(g_prof[i].key >> 1), g_prof[i].n);
        else if (dladdr((void *)g_prof[i].key, &di) && di.dli_fname && strstr(di.dli_fname, "rr2emu"))
            fprintf(df, "emu %lx %u\n", (unsigned long)(g_prof[i].key - (uintptr_t)di.dli_fbase), g_prof[i].n);
        else fprintf(df, "lib %s %u\n", di.dli_fname ? di.dli_fname : "?", g_prof[i].n);
    }
    if (df) fclose(df);
    for (int r = 0; r < 30; r++) {
        int best = -1;
        for (int j = 0; j < na; j++) if (agg[j].n && (best < 0 || agg[j].n > agg[best].n)) best = j;
        if (best < 0) break;
        LOG("  %5.1f%%  %s\n", 100.0 * agg[best].n / g_prof_total, agg[best].name);
        agg[best].n = 0;
    }
}

#else
static void prof_start(void) { LOG("[prof] --prof is only available on Linux\n"); }
static void prof_report(void) {}
#endif

/* a host memory fault: 1 = a watchpoint hit (retry the access), 0 = a crash (reported) */
int emu_fault(void *addr)
{
    cpu_t *c = tls_cpu;
    u8 *a = addr;
    if (g_watch && c && a >= g_mem + (g_watch & ~4095u) && a < g_mem + (g_watch & ~4095u) + 4096) {
        watch_protect(PROT_READ | PROT_WRITE);   /* let the write through */
        if ((u32)(a - g_mem) - g_watch < 4) c->exit_loop |= EXIT_WATCH;
        else c->exit_loop |= EXIT_WATCH | 0x200;  /* same page, other address: re-arm silently */
        c->span = 0;                              /* next fetch takes the slow path */
        return 1;
    }
    if (a >= g_mem && a < g_mem + 0x100000000ULL)
        fprintf(stderr, "\n*** guest fault at guest addr %08x\n", (u32)(a - g_mem));
    else
        fprintf(stderr, "\n*** host fault at %p (not guest memory)\n", (void *)a);
    if (c) {
        fprintf(stderr, "  tid %d pc=%08x lr=%08x sp=%08x\n ", c->tid, c->r[15], c->r[14], c->r[13]);
        for (int k = 0; k < 13; k++) fprintf(stderr, " r%d=%08x", k, c->r[k]);
        fprintf(stderr, "\n");
        for (int k = 4; k < 12; k++) {             /* object dumps help find the null member */
            u32 p = c->r[k];
            if (p < GUEST_NULL_LIMIT || p >= HLE_SLOT_BASE) continue;
            fprintf(stderr, "  [r%d]:", k);
            for (int w = 0; w < 24; w++) fprintf(stderr, " %08x", ld32(p + 4 * w));
            fprintf(stderr, "\n");
        }
        if (c->r[15] >= HLE_SLOT_BASE) fprintf(stderr, "  in HLE %s\n", hle_slot_name(c->r[15]));
        fprintf(stderr, "  branch targets:");
        for (int k = 0; k < 16; k++) fprintf(stderr, " %08x", c->ring[(c->ring_pos - k) & 63]);
        fprintf(stderr, "\n");
    }
    return 0;
}

#ifndef _WIN32
static void on_segv(int sig, siginfo_t *si, void *uc)
{
    if (emu_fault(si->si_addr)) return;
    signal(SIGSEGV, SIG_DFL);
    abort();
}
#endif

static void dump_threads(int sig)
{
    fprintf(stderr, "\n[sig] %d: guest thread states\n", sig);
    for (int i = 0; i < G.nthreads; i++) {
        cpu_t *t = G.threads[i];
        fprintf(stderr, "  tid %d %-15s pc=%08x lr=%08x sp=%08x insns=%llu depth=%d\n   ring:",
                t->tid, t->name, t->r[15], t->r[14], t->r[13],
                (unsigned long long)t->insn_count, t->call_depth);
        if (t->r[15] >= HLE_SLOT_BASE) fprintf(stderr, " [in %s]", hle_slot_name(t->r[15]));
        for (int k = 0; k < 12; k++) fprintf(stderr, " %08x", t->ring[(t->ring_pos - k) & 63]);
        fprintf(stderr, "\n");
    }
    _exit(128 + sig);
}

void mem_init(void);
extern int g_stats;
void stats_dump(void);
int  selftest_main(void);
int  difftest_main(int argc, char **argv);
int  bench_main(void);

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options] [FILE.so]   (no FILE.so: pick the game files in the launcher)\n"
        "  --no-launcher  skip the settings screen (implied by --headless/--max-frames/--shot)\n"
        "  --fullscreen   fullscreen at --size\n"
        "  --aniso N      anisotropic filtering level (0 = off)\n"
        "  --no-assists   force steering/brake assist and anti-skid off\n"
        "  --no-tilt      force horizon tilt (camera roll) off\n"
        "  --cockpit-fov D  add D degrees to the interior camera FOV\n"
        "  --selftest     run CPU/VFP self-tests\n"
        "  --assets DIR   game data root: holds com.ea.game.realracing2_*/ (default: ./assets)\n"
        "  --apk-assets DIR  APK assets/ served through AssetManager (default: ./apk_assets)\n"
        "  --max-frames N stop after N draw frames (0 = run forever)\n"
        "  --size WxH     surface size (default 800x480)\n"
        "  --headless     no window: GL calls hit stubs (tests, benchmarks)\n"
        "  --vsync        sync buffer swaps to the display\n"
        "  --shot N:FILE  save frame N as a PNG (repeatable)\n"
        "  --tap N:X,Y    scripted touch at frame N (repeatable)\n"
        "  --steer A:B:V  scripted steering V in [-1,1] for frames A..B\n"
        "  --back N       scripted Back key at frame N (repeatable)\n"
        "  --cam N        scripted camera change at frame N (repeatable)\n"
        "controls (defaults, remappable in the launcher): arrows/AD steer, Up/W gas, Down/S brake,\n"
        "          C camera, Esc back; pad L-stick/D-pad steer, R2/Cross gas, L2/Square brake,\n"
        "          Triangle camera, Circle back\n"
        "  --watch ADDR   log every guest write to the word at ADDR (hex)\n"
        "  --hook PC      log registers each time guest PC (hex) executes\n"
        "  --nojit        interpreter only (debug modes imply it)\n"
        "  --patch NAME   apply a game patch (repeatable; also RR2_PATCHES=a,b)\n"
        "  --prof         sample the main thread: JIT guest pcs / host symbols\n",
        argv0);
    exit(2);
}

static char g_patch_list[512];
void patches_apply(const char *list);
static struct { long frame; char path[256]; } g_shots[32];
static int g_nshots;
static cpu_t *g_ui_cpu;
static struct { long frame; int x, y; } g_taps[32];
static long g_backs[8], g_cams[8];
static int g_nbacks, g_ncams;
static int g_ntaps;
static struct { long from, to; float v; } g_steers[8];
static struct { long from, to; int x, y; } g_holds[8];
static int g_nholds;
static int g_nsteers;
static u32 g_touch_fn, g_touch_cls;
static u32 g_video_done_fn;
static u32 g_accel_fn, g_accel_obj, g_keydown_fn, g_keyup_fn, g_key_obj;

/* keyboard/pad -> device tilt (accelerometer, m/s^2) as a landscape phone held ~30 deg back */
static void feed_accel(float steer)
{
    if (!g_accel_fn) return;
    static int map = -1;
    if (map < 0) { const char *m = getenv("RR2_ACCEL"); map = m ? atoi(m) : 3; }
    const float g = 9.81f, maxang = 0.45f, back = 0.52f;
    float a = steer * maxang;
    float ux = g * cosf(back) * cosf(a), uy = g * cosf(back) * sinf(a), uz = g * sinf(back);
    float v[3] = { ux, uy, uz };
    switch (map) {                                         /* axis conventions to try */
    case 1: v[1] = -uy; break;
    case 2: v[0] = uy; v[1] = ux; break;
    case 3: v[0] = -uy; v[1] = ux; break;
    }
    u32 a5[5] = { jni_env_ptr(), g_accel_obj, 0, 0, 0 };
    memcpy(&a5[2], &v[0], 4); memcpy(&a5[3], &v[1], 4); memcpy(&a5[4], &v[2], 4);
    emu_call(g_ui_cpu, g_accel_fn, 5, a5);
}

static void send_key(int keycode, int down)
{
    u32 fn = down ? g_keydown_fn : g_keyup_fn;
    if (!fn) return;
    u32 a[5] = { jni_env_ptr(), g_key_obj, 600 /* PhysicalKeyboard */, (u32)keycode, 0 };
    emu_call(g_ui_cpu, fn, 5, a);
}

/* mouse -> TouchSurfaceAndroid.NativeOnPointerEvent(event, module, pointer, x, y) */
static void touch_ptr(int id, int action, int x, int y);
static void on_touch(int action, int x, int y) { touch_ptr(0, action, x, y); }
static void touch_ptr(int id, int action, int x, int y)
{
    static const u32 ev[3] = { 0x6000E, 0x8000E, 0x4000E };   /* down, up, move */
    if (!g_touch_fn) return;
    f32 fx = (f32)x, fy = (f32)y;
    u32 bx, by;
    memcpy(&bx, &fx, 4); memcpy(&by, &fy, 4);
    u32 a[7] = { jni_env_ptr(), g_touch_cls, ev[action], 1000 /* TouchScreen */, (u32)id, bx, by };
    emu_call(g_ui_cpu, g_touch_fn, 7, a);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    win_init();
#endif
    const char *so_path = NULL;
    int do_selftest = 0, do_prof = 0, no_launcher = 0, difft_arg = 0;
    G.assets_dir = "./assets";
    G.apk_assets_dir = "./apk_assets";
    G.max_frames = 0;
    G.width = 800; G.height = 480; G.dpi = 240.0f;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--difftest")) { do_selftest = 3; difft_arg = i + 1; break; }
        if (!strcmp(argv[i], "--selftest")) do_selftest = 1;
        else if (!strcmp(argv[i], "--bench")) do_selftest = 2;
        else if (!strcmp(argv[i], "--assets") && i + 1 < argc) G.assets_dir = argv[++i];
        else if (!strcmp(argv[i], "--apk-assets") && i + 1 < argc) G.apk_assets_dir = argv[++i];
        else if (!strcmp(argv[i], "--max-frames") && i + 1 < argc) G.max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fake-clock")) G.fake_clock = 1;
        else if (!strcmp(argv[i], "--stats")) g_stats = 1;
        else if (!strcmp(argv[i], "--headless")) G.headless = 1;
        else if (!strcmp(argv[i], "--nojit")) g_jit = 0;
        else if (!strcmp(argv[i], "--patch") && i + 1 < argc) {
            size_t l = strlen(g_patch_list);
            snprintf(g_patch_list + l, sizeof(g_patch_list) - l, "%s%s", l ? "," : "", argv[++i]);
        }
        else if (!strcmp(argv[i], "--prof")) do_prof = 1;
        else if (!strcmp(argv[i], "--hook") && i + 1 < argc) { cpu_add_hook((u32)strtoul(argv[++i], NULL, 16)); g_jit = 0; }
        else if (!strcmp(argv[i], "--vsync")) G.vsync = 1;
        else if (!strcmp(argv[i], "--no-launcher")) no_launcher = 1;
        else if (!strcmp(argv[i], "--fullscreen")) G.fullscreen = 1;
        else if (!strcmp(argv[i], "--no-assists")) G.no_assists = 1;
        else if (!strcmp(argv[i], "--no-tilt")) G.no_tilt = 1;
        else if (!strcmp(argv[i], "--cockpit-fov") && i + 1 < argc) G.cockpit_fov = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aniso") && i + 1 < argc) G.aniso = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--back") && i + 1 < argc && g_nbacks < 8) g_backs[g_nbacks++] = atol(argv[++i]);
        else if (!strcmp(argv[i], "--cam") && i + 1 < argc && g_ncams < 8) g_cams[g_ncams++] = atol(argv[++i]);
        else if (!strcmp(argv[i], "--tap") && i + 1 < argc && g_ntaps < 32) {
            sscanf(argv[++i], "%ld:%d,%d", &g_taps[g_ntaps].frame, &g_taps[g_ntaps].x, &g_taps[g_ntaps].y);
            g_ntaps++;
        }
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc && g_nholds < 8) {
            sscanf(argv[++i], "%ld:%ld:%d,%d", &g_holds[g_nholds].from, &g_holds[g_nholds].to, &g_holds[g_nholds].x, &g_holds[g_nholds].y);
            g_nholds++;
        }
        else if (!strcmp(argv[i], "--steer") && i + 1 < argc && g_nsteers < 8) {
            sscanf(argv[++i], "%ld:%ld:%f", &g_steers[g_nsteers].from, &g_steers[g_nsteers].to, &g_steers[g_nsteers].v);
            g_nsteers++;
        }
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc && g_nshots < 32) {
            char *colon = strchr(argv[++i], ':');
            g_shots[g_nshots].frame = atol(argv[i]);
            snprintf(g_shots[g_nshots].path, sizeof(g_shots[0].path), "%s", colon ? colon + 1 : "shot.png");
            g_nshots++;
        }
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) g_watch = (u32)strtoul(argv[++i], NULL, 16) & ~3u;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &G.width, &G.height);
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "-vv")) g_verbose = 2;
        else if (!strcmp(argv[i], "--trace")) g_verbose = 3;
        else if (argv[i][0] != '-') so_path = argv[i];
        else usage(argv[0]);
    }

#ifndef _WIN32
    struct sigaction sa = { .sa_sigaction = on_segv, .sa_flags = SA_SIGINFO };
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif
    signal(SIGINT, dump_threads);
    signal(SIGTERM, dump_threads);
    host_binds_default();
    mem_init();
    cpu_init_decode();
    hle_init();

    if (do_selftest == 3)
        return difftest_main(argc - difft_arg, argv + difft_arg);
    if (do_selftest)
        return do_selftest == 2 ? bench_main() : selftest_main();

    int ui = !G.headless && !no_launcher && !G.max_frames && !g_nshots;
    if (!so_path && !ui)
        usage(argv[0]);

    if (!G.headless) {
        if (!host_video_init(ui ? 800 : G.width, ui ? 640 : G.height, ui || G.vsync)) {
            if (!so_path) fatal("the launcher needs SDL2 with GLES2");
            G.headless = 1;
        } else {
            if (ui && !launcher_run(&so_path)) return 0;
            if (ui || G.fullscreen) host_video_mode(&G.width, &G.height, G.fullscreen, G.vsync);
            if (!glhost_init()) G.headless = 1;
        }
        if (G.headless) LOG("[boot] no host GL: continuing headless\n");
    }
    LOG("[boot] loading %s\n", so_path);
    if (elf_load(&G, so_path) != 0)
        fatal("failed to load %s", so_path);
    patches_setup();
    patches_apply(g_patch_list);
    patches_apply(getenv("RR2_PATCHES"));
    cpu_icache_reset();
    if (g_verbose >= 3 || g_watch || g_stats || getenv("RR2_NOJIT")) g_jit = 0;   /* debug modes interpret */
    jit_reset();
    LOG("[boot] %s\n", g_jit ? "JIT on" : "interpreter only");

    cpu_t *c = emu_new_cpu();
    c->r[13] = GUEST_STACK_TOP & ~7u;
    snprintf(c->name, sizeof(c->name), "main");

    if (g_watch) watch_protect(watch_armed_prot());
    LOG("[boot] running static constructors\n");
    elf_run_init_array(c);
    LOG("[boot] INIT_ARRAY done\n");

    jni_init();
    u32 onload = elf_lookup("JNI_OnLoad");
    if (onload) {
        u32 args[2] = { jni_vm_ptr(), 0 };
        u32 ver = emu_call(c, onload, 2, args);
        LOG("[boot] JNI_OnLoad -> 0x%x\n", ver);
    }

    /* Android lifecycle as the APK's Java drives it (blast MainActivity.onCreate,
     * RealRacing2Activity.onCreate, onResume, then the GLSurfaceView thread). */
    const char *files = "/data/data/com.ea.game.realracing2_OTD_row/files", *ext = "/sdcard";
    u32 assets = jni_object("android/content/res/AssetManager");
    struct { const char *sym; u32 thiz; int n; u32 a[3]; } seq[] = {
        { "Java_com_ea_EAThread_EAThread_Init",                    jni_class("com/ea/EAThread/EAThread"), 0, {0} },
        { "Java_com_ea_EAIO_EAIO_StartupNativeImpl",               jni_class("com/ea/EAIO/EAIO"), 3,
          { assets, jni_string(files), jni_string(ext) } },
        { "Java_com_ea_EAMIO_StorageDirectory_StartupNativeImpl",  jni_class("com/ea/EAMIO/StorageDirectory"), 0, {0} },
        { "Java_com_ea_blast_MainActivity_NativeOnCreate",         jni_activity(), 0, {0} },
        { "Java_com_ea_EAMAudio_EAMAudioCoreWrapper_NativeStartup", jni_class("com/ea/EAMAudio/EAMAudioCoreWrapper"), 0, {0} },
        { "Java_com_ea_rwfilesystem_rwfilesystem_StartupNativeImpl", jni_class("com/ea/rwfilesystem/rwfilesystem"), 3,
          { assets, jni_string(files), jni_string(ext) } },
        { "Java_com_ea_VideoPlayer_PlayerAndroid_StartupNativeImpl", jni_class("com/ea/VideoPlayer/PlayerAndroid"), 0, {0} },
        { "Java_com_ea_blast_MainActivity_NativeOnResume",         jni_activity(), 1, { 1 } },
        { "Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceCreated", jni_object("com/ea/blast/AndroidRenderer"), 0, {0} },
        { "Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceChanged", jni_object("com/ea/blast/AndroidRenderer"), 2,
          { (u32)G.width, (u32)G.height } },
    };
    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        u32 fn = elf_lookup(seq[i].sym);
        if (!fn) { LOG("[boot] %s not found, skipping\n", seq[i].sym); continue; }
        u32 a[5] = { jni_env_ptr(), seq[i].thiz, seq[i].a[0], seq[i].a[1], seq[i].a[2] };
        LOG("[boot] %s\n", seq[i].sym + 5);
        emu_call(c, fn, 2 + seq[i].n, a);
    }

    g_ui_cpu = c;
    g_touch_fn = elf_lookup("Java_com_ea_blast_TouchSurfaceAndroid_NativeOnPointerEvent");
    g_touch_cls = jni_class("com/ea/blast/TouchSurfaceAndroid");
    host_set_input(on_touch);
    g_accel_fn = elf_lookup("Java_com_ea_blast_AccelerometerAndroidDelegate_NativeOnAcceleration");
    g_accel_obj = jni_object("com/ea/blast/AccelerometerAndroidDelegate");
    g_keydown_fn = elf_lookup("Java_com_ea_blast_KeyboardAndroid_NativeOnKeyDown");
    g_keyup_fn = elf_lookup("Java_com_ea_blast_KeyboardAndroid_NativeOnKeyUp");
    g_key_obj = jni_object("com/ea/blast/PhysicalKeyboardAndroid");
    g_video_done_fn = elf_lookup("Java_com_ea_VideoPlayer_PlayerAndroid_OnCompletionNativeImpl");
    float steer = 0;
    int braking = 0, gassing = 0, backs_sent = 0;

    if (do_prof) prof_start();
    struct timespec t0, t1, c0, c1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
    u64 i0 = c->insn_count;
    u32 draw = elf_lookup("Java_com_ea_blast_AndroidRenderer_NativeOnDrawFrame");
    u32 da[2] = { jni_env_ptr(), jni_object("com/ea/blast/AndroidRenderer") };
    LOG("[boot] draw loop (max_frames=%d)\n", G.max_frames);
    long frames = 0;
    for (; draw && (!G.max_frames || frames < G.max_frames); frames++) {
        g_frame = frames;
        /* live input: steering ramps like a tilted phone; brake is a held touch; back key */
        for (int t = 0; t < g_nsteers; t++)
            if (frames >= g_steers[t].from && frames < g_steers[t].to) g_input.steer_target = g_steers[t].v;
        float d = g_input.steer_target - steer;
        steer += d > 0.08f ? 0.08f : d < -0.08f ? -0.08f : d;
        feed_accel(steer);
        /* pedals are touch zones (Method B/D): gas bottom-right, brake bottom-left */
        if (g_input.gas != gassing) {
            gassing = g_input.gas;
            touch_ptr(1, gassing ? 0 : 1, G.width * 9 / 10, G.height * 5 / 6);
        }
        if (g_input.brake != braking) {
            braking = g_input.brake;
            touch_ptr(2, braking ? 0 : 1, G.width / 10, G.height * 5 / 6);
        }
        for (int t = 0; t < g_nbacks; t++) if (frames == g_backs[t]) g_input.back++;
        for (int t = 0; t < g_ncams; t++) if (frames == g_cams[t]) g_input.camera++;
        /* camera change: the HUD camera button (top right) shows on a first touch, a second one presses it */
        static int cams_sent, cam_taps, cam_t, cam_x = -1, cam_y;
        static long cam_last = -1000;
        if (cam_x < 0) {
            cam_x = G.width * 95 / 100; cam_y = G.height * 6 / 100;
            if (getenv("RR2_CAM_POS")) sscanf(getenv("RR2_CAM_POS"), "%d,%d", &cam_x, &cam_y);
        }
        static long race_seen = -1000;
        if (g_race_hud_drawn) { g_race_hud_drawn = 0; race_seen = frames; }
        if (frames - race_seen > 10) cams_sent = g_input.camera;          /* only inside a race */
        if (!cam_taps && cams_sent < g_input.camera) {
            cam_taps = frames - cam_last < 90 ? 1 : 2;
            if (g_verbose) LOG("[input] camera change (%d taps)\n", cam_taps);
            cams_sent++;
            cam_last = frames;
            cam_t = 0;
        }
        if (cam_taps) {
            if (cam_t == 0) touch_ptr(3, 0, cam_x, cam_y);
            if (cam_t == 3) touch_ptr(3, 1, cam_x, cam_y);
            if (++cam_t == 6) { cam_taps--; cam_t = 0; }
        }
        while (backs_sent < g_input.back) { send_key(4, 1); send_key(4, 0); backs_sent++; }
        u32 vid = jni_take_video_completion();          /* deliver VideoPlayer onCompletion */
        if (vid && g_video_done_fn) { u32 va[2] = { jni_env_ptr(), vid }; emu_call(c, g_video_done_fn, 2, va); }
        for (int t = 0; t < g_nholds; t++) {
            if (frames == g_holds[t].from) on_touch(0, g_holds[t].x, g_holds[t].y);
            if (frames == g_holds[t].to) on_touch(1, g_holds[t].x, g_holds[t].y);
        }
        for (int t = 0; t < g_ntaps; t++) {             /* scripted input: press, release 4 frames later */
            if (frames == g_taps[t].frame) on_touch(0, g_taps[t].x, g_taps[t].y);
            if (frames == g_taps[t].frame + 4) on_touch(1, g_taps[t].x, g_taps[t].y);
        }
        emu_call(c, draw, 2, da);
        for (int t = 0; t < g_nshots; t++)
            if (frames == g_shots[t].frame && !G.headless)
                LOG("[boot] screenshot %s: %s\n", g_shots[t].path,
                    glhost_screenshot(g_shots[t].path, G.width, G.height) ? "ok" : "failed");
        if (!host_present()) break;
        if (frames % 300 == 299) {                       /* periodic perf readout */
            static struct timespec lt; static u64 li; static long lf;
            struct timespec nt; clock_gettime(CLOCK_MONOTONIC, &nt);
            if (lt.tv_sec) {
                double d = (nt.tv_sec - lt.tv_sec) + (nt.tv_nsec - lt.tv_nsec) / 1e9;
                LOG("[perf] frames %ld-%ld: %.1f fps, main %.1f MIPS", lf, frames,
                    (frames - lf) / d, (c->insn_count - li) / (d * 1e6));
                static u64 prev[MAX_THREADS];
                for (int t = 0; t < G.nthreads; t++) {
                    cpu_t *tc = G.threads[t];
                    if (tc != c && tc->tid < MAX_THREADS) {
                        u64 n = tc->insn_count - prev[tc->tid];
                        if (n > 1000000) LOG(" | %s %.1f", tc->name, n / (d * 1e6));
                        prev[tc->tid] = tc->insn_count;
                    }
                }
                LOG("\n");
            }
            lt = nt; li = c->insn_count; lf = frames;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);

    /* leave like Android does: onPause (the game saves here), let it run, then onStop */
    u32 pause_fn = elf_lookup("Java_com_ea_blast_MainActivity_NativeOnPause");
    u32 stop_fn = elf_lookup("Java_com_ea_blast_MainActivity_NativeOnStop");
    if (pause_fn) {
        u32 a[2] = { jni_env_ptr(), jni_activity() };
        LOG("[boot] pausing (saves game)\n");
        /* onPause runs on the UI thread and waits for the GL thread: keep drawing meanwhile */
        volatile int done = 0;
        if (guest_async_call(pause_fn, 2, a, &done))
            for (int f = 0; f < 600 && !done && draw; f++) emu_call(c, draw, 2, da);
        for (int f = 0; f < 30 && draw; f++) emu_call(c, draw, 2, da);
        if (stop_fn) emu_call(c, stop_fn, 2, a);
        usleep(300000);                               /* let a save thread finish writing */
        LOG("[boot] paused (%s)\n", done ? "ok" : "timed out");
    }
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    double ct = (c1.tv_sec - c0.tv_sec) + (c1.tv_nsec - c0.tv_nsec) / 1e9;
    u64 n = c->insn_count - i0;
    stats_dump();
    for (int i = 0; i < 64 && g_readers[i].pc; i++) LOG("[watch] reader %08x x%u\n", g_readers[i].pc, g_readers[i].n);
    if (do_prof) {
        prof_report();
        if (!G.headless) glhost_report(frames);
        /* guest memory residency by region */
        static const struct { const char *name; u32 lo, hi; } rg[] = {
            { "hle data", GUEST_HLE_DATA, GUEST_MMAP_BASE }, { "mmap arena", GUEST_MMAP_BASE, GUEST_LIB_BASE },
            { "library", GUEST_LIB_BASE, GUEST_HEAP_BASE }, { "heap", GUEST_HEAP_BASE, GUEST_HEAP_MAX },
            { "stacks", 0x60000000u, 0x70800000u },
        };
        static unsigned char vec[(0x40000000u >> 12) + 1];
        for (unsigned i = 0; i < sizeof(rg) / sizeof(rg[0]); i++) {
            size_t len = rg[i].hi - rg[i].lo, pages = len >> 12, res = 0;
            if (mincore(g_mem + rg[i].lo, len, vec) == 0)
                for (size_t k = 0; k < pages; k++) res += vec[k] & 1;
            LOG("[mem] guest %-10s resident %6.1f MB\n", rg[i].name, res * 4096.0 / 1048576.0);
        }
        LOG("[mem] guest heap in use %.1f MB (TLSF), mmap live %.1f MB\n",
            guest_heap_in_use() / 1048576.0, guest_mmap_live() / 1048576.0);
    }
    LOG("[boot] done: draw loop %.2fs wall, %.2fs cpu, %llu insns: %.1f MIPS (cpu), %.1f fps\n",
        dt, ct, (unsigned long long)n, n / (ct * 1e6 + 1e-9), frames / (dt + 1e-9));
    return 0;
}
