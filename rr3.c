/*
 * rr3.c - Real Racing 3 (v7.6.0, armeabi-v7a) boot and frame loop.
 *
 * Mirrors what the APK's Java side does (com.firemint.realracing.MainActivity):
 * System.loadLibrary(c++_shared, Nimble, fmodex, RealRacing3) -> JNI_OnLoad,
 * onCreate -> onCreateJNI, onStart -> onStartJNI, then the GLSurfaceView
 * thread: onViewCreatedJNI, onViewChangedJNI(w, h, orientation, rotation),
 * onResumeJNI, onWindowFocusChangedJNI(true) and onViewRenderJNI per frame.
 * The UI and GL threads are the same host thread here.
 *
 * Data layout (G.assets_dir): sdcard/ = /sdcard/Android/data/com.ea.games.r3_row,
 * internal/ = /data/data/com.ea.games.r3_row, apk/ = the installed APK and libs.
 */
#include "emu.h"
#include <time.h>

extern long g_frame;
void jni_rr3_setup(void);

static u32 fn(const char *name)
{
    u32 a = elf_lookup(name);
    if (!a) LOG("[rr3] missing native %s\n", name);
    return a;
}

static void jcall(cpu_t *c, u32 f, int n, const u32 *extra)
{
    if (!f) return;
    u32 a[8] = { jni_env_ptr(), jni_activity() };
    for (int i = 0; i < n && i < 6; i++) a[2 + i] = extra[i];
    jni_push_frame();
    emu_call(c, f, 2 + n, a);
    jni_pop_frame();
}

int rr3_main(const char *so_path)
{
    G.game = 3;
    if (!G.save_dir) G.save_dir = "./save_rr3";
    LOG("[rr3] loading %s (data %s, saves %s)\n", so_path, G.assets_dir, G.save_dir);
    if (elf_load(&G, so_path) != 0) fatal("failed to load %s", so_path);
    if (getenv("RR2_STUBS")) { extern void hle_dump_stubs(void); hle_dump_stubs(); }
    cpu_icache_reset();
    thumb_it_reset();
    if (getenv("RR2_NOJIT")) g_jit = 0;
    jit_reset();

    cpu_t *c = emu_new_cpu();
    c->r[13] = GUEST_STACK_TOP & ~7u;
    snprintf(c->name, sizeof(c->name), "main");

    jni_rr3_setup();
    jni_init();
    /* System.loadLibrary order from MainActivity's static initializer: constructors, then JNI_OnLoad */
    static const char *const order[] = { "libc++_shared.so", "libNimble.so", "libfmodex.so", "libRealRacing3.so" };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        LOG("[rr3] loadLibrary %s\n", order[i]);
        elf_run_init_lib(c, order[i]);
        u32 onload = elf_lookup_in(order[i], "JNI_OnLoad");
        if (!onload) continue;
        u32 args[2] = { jni_vm_ptr(), 0 };
        u32 ver = emu_call(c, onload, 2, args);
        LOG("[rr3] %s JNI_OnLoad -> 0x%x\n", order[i], ver);
    }

    const u32 orient = 2, rot = 1;                 /* landscape */
    LOG("[rr3] onCreateJNI\n");          jcall(c, fn("Java_com_firemint_realracing_MainActivity_onCreateJNI"), 0, NULL);
    LOG("[rr3] onStartJNI\n");           jcall(c, fn("Java_com_firemint_realracing_MainActivity_onStartJNI"), 0, NULL);
    LOG("[rr3] onViewCreatedJNI\n");     jcall(c, fn("Java_com_firemint_realracing_MainActivity_onViewCreatedJNI"), 0, NULL);
    u32 vc[4] = { (u32)G.width, (u32)G.height, orient, rot };
    LOG("[rr3] onViewChangedJNI %dx%d\n", G.width, G.height);
    jcall(c, fn("Java_com_firemint_realracing_MainActivity_onViewChangedJNI"), 4, vc);
    LOG("[rr3] onResumeJNI\n");          jcall(c, fn("Java_com_firemint_realracing_MainActivity_onResumeJNI"), 0, NULL);
    u32 one = 1;
    jcall(c, fn("Java_com_firemint_realracing_MainActivity_onWindowFocusChangedJNI"), 1, &one);

    u32 render = fn("Java_com_firemint_realracing_MainActivity_onViewRenderJNI");
    u32 ra[2] = { orient, rot };
    LOG("[rr3] render loop (max_frames=%d)\n", G.max_frames);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    u64 i0 = c->insn_count;
    long frames = 0;
    for (; render && (!G.max_frames || frames < G.max_frames); frames++) {
        g_frame = frames;
        jcall(c, render, 2, ra);
        extern void frame_screenshots(long frame);
        frame_screenshots(frames);
        if (!host_present()) break;
        if (frames % 120 == 119) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double d = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            LOG("[perf] frame %ld: %.1f fps, main %.1f MIPS\n", frames, 120 / d, (c->insn_count - i0) / (d * 1e6));
            t0 = t1; i0 = c->insn_count;
        }
    }
    LOG("[rr3] onPauseJNI / onStopJNI\n");
    jcall(c, fn("Java_com_firemint_realracing_MainActivity_onPauseJNI"), 0, NULL);
    jcall(c, fn("Java_com_firemint_realracing_MainActivity_onStopJNI"), 0, NULL);
    host_shutdown();
    return 0;
}
