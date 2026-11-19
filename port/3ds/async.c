/* plat_async (platform.h): one worker thread on another core. Old 3DS: the system core (core 1), which an
 * application may use for up to 80% of its time (APT_SetAppCpuTimeLimit); New 3DS: core 2, a whole one.
 * One job at a time; the caller waits for it with plat_async_wait. */
#include <3ds.h>

#include "ctr.h"
#include "platform.h"
#include "psx_runtime.h"

static Thread worker;
static LightEvent go, done;
static void (*volatile job_fn)(void *);
static void *volatile job_arg;
static volatile int busy, quit, started;
unsigned ctr_async_jobs, ctr_async_done; /* (the heartbeat: platform.c) */

static void worker_main(void *unused) {
    (void)unused;
    void ctr_exception_install(void);
    ctr_exception_install();
    for (;;) {
        LightEvent_Wait(&go);
        if (quit) break;
        started = 1;
        __sync_synchronize();
        job_fn(job_arg);
        ctr_async_done++;
        __sync_synchronize();
        busy = 0;
        LightEvent_Signal(&done);
    }
}

/* Core 1 (the system core) gives an application's threads only the share it sets. Only an Old 3DS runs threads of
 * ours there (a New 3DS has core 2): 80% then, while the game runs. The HOME Menu runs on core 1 while the game is
 * suspended: the share goes back to it then (else HOME, or the power button's sleep, takes half a minute). */
static aptHookCookie share_hook;
static void share_event(APT_HookType t, void *unused) {
    (void)unused;
    if (t == APTHOOK_ONSUSPEND || t == APTHOOK_ONSLEEP) APT_SetAppCpuTimeLimit(5);
    else if (t == APTHOOK_ONRESTORE || t == APTHOOK_ONWAKEUP) APT_SetAppCpuTimeLimit(80);
}
/* closing: core 1 the application's again (the HOME Menu's 5% while suspended, which closing doesn't restore), so our
 * threads there end in time (render.c ctr_shutdown) */
void ctr_core1_closing(void) {
    if (share_hook.callback) APT_SetAppCpuTimeLimit(80);
}
void ctr_core1_share(int any_model) { /* (any_model: a New 3DS's thread falling back to core 1) */
    static int done;
    if (done || (ctr_is_new3ds() && !any_model)) return;
    done = 1;
    APT_SetAppCpuTimeLimit(80);
    aptHook(&share_hook, share_event, NULL);
}

void ctr_async_init(void) {
    LightEvent_Init(&go, RESET_ONESHOT);
    LightEvent_Init(&done, RESET_ONESHOT);
    int core = ctr_is_new3ds() ? 2 : 1;
    /* (core 1 runs an application's threads only for the share it is given: set before any thread may land there,
     * a New 3DS's fallback included) */
    ctr_core1_share(0);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    worker = threadCreate(worker_main, NULL, 64 * 1024, prio - 1, core, false);
    if (!worker && core == 2) ctr_core1_share(1), core = 1, worker = threadCreate(worker_main, NULL, 64 * 1024, prio - 1, 1, false);
    port_log("async: %s\n", worker ? (core == 2 ? "worker on core 2" : "worker on core 1") : "none");
}

static int dead; /* the worker never ran a job: jobs run on the caller from then on */

int plat_async(void (*fn)(void *), void *arg) {
    if (!worker || dead) return 0;
    plat_async_wait();
    if (dead) return 0;
    job_fn = fn;
    job_arg = arg;
    ctr_async_jobs++;
    started = 0;
    busy = 1;
    __sync_synchronize();
    LightEvent_Signal(&go);
    return 1;
}

/* Wait for the job. If the worker hasn't even started it after half a second, it isn't being scheduled (a core the
 * system doesn't give this application): the job runs here, and every later one too. */
void plat_async_wait(void) {
    extern volatile const char *ctr_where;
    const char *was = (const char *)ctr_where;
    ctr_where = "waiting for the worker (movie / async job)";
    u64 t0 = osGetTime();
    while (busy) {
        LightEvent_WaitTimeout(&done, 20000000);
        if (busy && !started && osGetTime() - t0 > 500) {
            dead = 1;
            port_log("async: the worker isn't running; jobs run on the game's thread\n");
            void (*fn)(void *) = job_fn;
            void *arg = job_arg;
            busy = 0;
            fn(arg);
            ctr_where = was;
            return;
        }
    }
    __sync_synchronize();
    ctr_where = was;
}

int plat_async_busy(void) { return busy; }

/* ---- the I/O thread (plat_io_async): the disc pack read ahead while the game runs (cd.c). Its reads waited for the
 * card on the game's thread: a 128 KiB block of a movie's stream some 30 ms, three times a second (field 1464's movie
 * background on an Old 3DS: 1.5 ms of a VBlank, the game a frame behind each time). On the game's core, above the game:
 * it only runs to start a read and when one returns. */
static Thread io_thread;
static LightEvent io_go, io_done;
static void (*volatile io_fn)(void *);
static void *volatile io_arg;
static volatile int io_busy;
static void io_main(void *unused) {
    (void)unused;
    for (;;) {
        LightEvent_Wait(&io_go);
        if (quit) break;
        io_fn(io_arg);
        __sync_synchronize();
        io_busy = 0;
        LightEvent_Signal(&io_done);
    }
}
int plat_io_async(void (*fn)(void *), void *arg) {
    if (!io_thread) {
        static int tried;
        if (tried++) return 0;
        LightEvent_Init(&io_go, RESET_ONESHOT);
        LightEvent_Init(&io_done, RESET_ONESHOT);
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        io_thread = threadCreate(io_main, NULL, 16 * 1024, prio - 1, 0, false);
        if (!io_thread) return 0;
    }
    plat_io_wait();
    io_fn = fn;
    io_arg = arg;
    io_busy = 1;
    __sync_synchronize();
    LightEvent_Signal(&io_go);
    return 1;
}
void plat_io_wait(void) {
    while (io_busy) LightEvent_WaitTimeout(&io_done, 20000000);
    __sync_synchronize();
}

/* ---- the drawing thread (plat_draw_async): a scene's ordering table walked while the game builds the next frame
 * (gpu.c). On the worker's core, above the sound thread there: the game's thread kicks the sound's mix as a frame
 * ends, just before the walk is sent, and the mix (some 9 ms a frame on an Old 3DS) would have the walk wait for most
 * of the time it has. The sound waits for the walk instead, which the game's next GPU call ends (a frame's game code,
 * 12 ms or so, at most its drawing), and mixes while the game's thread finishes the frame: its queue holds a quarter
 * of a second. */
static Thread draw_thread;
static LightEvent draw_go, draw_done;
static void (*volatile draw_fn)(void *);
static void *volatile draw_arg;
static volatile int draw_busy, draw_started, draw_dead;
static void draw_main(void *unused) {
    (void)unused;
    void ctr_exception_install(void);
    ctr_exception_install();
    for (;;) {
        LightEvent_Wait(&draw_go);
        if (quit) break;
        draw_started = 1;
        __sync_synchronize();
        draw_fn(draw_arg);
        __sync_synchronize();
        draw_busy = 0;
        LightEvent_Signal(&draw_done);
    }
}
int plat_draw_async(void (*fn)(void *), void *arg) {
    if (draw_dead) return 0;
    if (!draw_thread) {
        static int tried;
        if (tried++) return 0;
        LightEvent_Init(&draw_go, RESET_ONESHOT);
        LightEvent_Init(&draw_done, RESET_ONESHOT);
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        ctr_core1_share(0);
        draw_thread = threadCreate(draw_main, NULL, 128 * 1024, prio - 3, ctr_is_new3ds() ? 2 : 1, false);
        port_log("async: drawing thread %s\n", draw_thread ? "started" : "not created");
        if (!draw_thread) return 0;
    }
    plat_draw_wait();
    if (draw_dead) return 0;
    draw_fn = fn;
    draw_arg = arg;
    draw_started = 0;
    draw_busy = 1;
    __sync_synchronize();
    LightEvent_Signal(&draw_go);
    return 1;
}
/* (as plat_async_wait: a thread the system doesn't schedule within half a second isn't used again, and its job runs
 * here) */
void plat_draw_wait(void) {
    if (!draw_busy) return;
    extern volatile const char *ctr_where;
    const char *was = (const char *)ctr_where;
    ctr_where = "waiting for the drawing thread";
    u64 t0 = osGetTime();
    while (draw_busy) {
        LightEvent_WaitTimeout(&draw_done, 20000000);
        if (draw_busy && !draw_started && osGetTime() - t0 > 500) {
            draw_dead = 1;
            port_log("async: the drawing thread isn't running; the game's thread draws\n");
            void (*fn)(void *) = draw_fn;
            void *arg = draw_arg;
            draw_busy = 0;
            fn(arg);
            break;
        }
    }
    __sync_synchronize();
    ctr_where = was;
}

void ctr_async_exit(void) {
    quit = 1;
    if (draw_thread) {
        plat_draw_wait();
        LightEvent_Signal(&draw_go);
        if (R_SUCCEEDED(threadJoin(draw_thread, 1000000000ULL))) threadFree(draw_thread);
        draw_thread = NULL;
    }
    if (io_thread) {
        plat_io_wait();
        LightEvent_Signal(&io_go);
        if (R_SUCCEEDED(threadJoin(io_thread, 1000000000ULL))) threadFree(io_thread);
        io_thread = NULL;
    }
    if (!worker) return;
    LightEvent_Signal(&go);
    /* (a second at most: closing must not hang; not freed if it hasn't ended, its stack would go under it) */
    if (R_SUCCEEDED(threadJoin(worker, 1000000000ULL))) threadFree(worker);
    worker = NULL;
}
