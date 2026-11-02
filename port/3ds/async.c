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

void ctr_async_exit(void) {
    if (!worker) return;
    quit = 1;
    LightEvent_Signal(&go);
    threadJoin(worker, 1000000000ULL); /* (a second at most: closing must not hang) */
    threadFree(worker);
    worker = NULL;
}
