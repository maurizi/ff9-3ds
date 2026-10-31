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
static volatile int busy, quit;

static void worker_main(void *unused) {
    (void)unused;
    for (;;) {
        LightEvent_Wait(&go);
        if (quit) break;
        job_fn(job_arg);
        __sync_synchronize();
        busy = 0;
        LightEvent_Signal(&done);
    }
}

void ctr_async_init(void) {
    LightEvent_Init(&go, RESET_ONESHOT);
    LightEvent_Init(&done, RESET_ONESHOT);
    int core = 1;
    if (ctr_is_new3ds()) core = 2;
    else APT_SetAppCpuTimeLimit(80);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    worker = threadCreate(worker_main, NULL, 64 * 1024, prio - 1, core, false);
    if (!worker && core == 2) worker = threadCreate(worker_main, NULL, 64 * 1024, prio - 1, 1, false);
    port_log("async: %s\n", worker ? (core == 2 ? "worker on core 2" : "worker on core 1") : "none");
}

int plat_async(void (*fn)(void *), void *arg) {
    if (!worker) return 0;
    plat_async_wait();
    job_fn = fn;
    job_arg = arg;
    busy = 1;
    __sync_synchronize();
    LightEvent_Signal(&go);
    return 1;
}

void plat_async_wait(void) {
    while (busy) LightEvent_Wait(&done);
    __sync_synchronize();
}

void ctr_async_exit(void) {
    if (!worker) return;
    plat_async_wait();
    quit = 1;
    LightEvent_Signal(&go);
    threadJoin(worker, U64_MAX);
    threadFree(worker);
    worker = NULL;
}
