/* Host-only stress test for nn::os synchronization primitives in host/nn_os_sync.c.
   Tests mutual exclusion, recursive locking, TryLock, TimedLock, CondVar Wait/TimedWait,
   Signal, and Broadcast under high thread contention with pthreads. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>

#include "hos.h"
#include <windows.h>

/* Forward declare HLE functions from nn_os_sync.c */
void imp__ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi(Ctx *c);
void imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE(Ctx *c);
void imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE(Ctx *c);
void imp__ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE(Ctx *c);
void imp__ZN2nn2os14TimedLockMutexEPNS0_9MutexTypeENS_8TimeSpanE(Ctx *c);
void imp__ZN2nn2os27InitializeConditionVariableEPNS0_21ConditionVariableTypeE(Ctx *c);
void imp__ZN2nn2os21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE(Ctx *c);
void imp__ZN2nn2os26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE(Ctx *c);
void imp__ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE(Ctx *c);
void imp__ZN2nn2os26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE(Ctx *c);

/* Minimal host environment stubs for test */
int hos_trace = 0;
const uint64_t hos_image_base = 0x80000000;
const uint64_t hos_image_end = 0x90000000;
const uint64_t hos_program_id = 0;
const char hos_main_build_id[] = "test";
int hos_game_is_afterglow(void) { return 0; }
int hos_force_online(void) { return 0; }

struct HosThread {
    Ctx ctx;
    char name[48];
    const char *volatile current_wait;
    uint64_t volatile wait_obj;
};

static __thread HosThread *test_self_thread = NULL;
HosThread *hos_self(void) { return test_self_thread; }
const char *hos_thread_name(HosThread *t) { return t ? t->name : "none"; }
void hos_thread_set_wait(HosThread *t, const char *what, uint64_t obj) {
    if (!t) t = test_self_thread;
    if (t) { t->current_wait = what; t->wait_obj = obj; }
}
void hos_thread_clear_wait(HosThread *t) {
    if (!t) t = test_self_thread;
    if (t) { t->current_wait = NULL; t->wait_obj = 0; }
}
const char *hos_thread_get_wait(HosThread *t, uint64_t *out_obj) {
    if (!t) return NULL;
    if (out_obj) *out_obj = t->wait_obj;
    return t->current_wait;
}
void hos_thread_backtrace(HosThread *t, char *buf, size_t cap) { (void)t; if (cap) buf[0] = 0; }
void hos_threads_dump(void) {}
void hos_log(const char *fmt, ...) { (void)fmt; }
void hos_vcore_checkpoint(void) {}

int64_t hos_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void hos_sleep_ns(int64_t ns) {
    struct timespec ts = { .tv_sec = ns / 1000000000LL, .tv_nsec = ns % 1000000000LL };
    nanosleep(&ts, NULL);
}

int hos_cv_sleep(void *cv, void *lk, unsigned ms) {
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)cv, (PSRWLOCK)lk, ms, 0);
}

/* Stress test parameters */
#define NUM_THREADS 12
#define TEST_DURATION_SECS 3
#define MUTEX_ADDR 0x10000ULL
#define COND_ADDR  0x20000ULL

static volatile int g_stop = 0;
static volatile int g_in_cs = 0;
static volatile int g_cs_violations = 0;
static volatile int g_cond_predicate = 0;

static volatile long g_stat_locks = 0;
static volatile long g_stat_trylocks = 0;
static volatile long g_stat_timedlocks = 0;
static volatile long g_stat_waits = 0;
static volatile long g_stat_timedwaits = 0;
static volatile long g_stat_signals = 0;
static volatile long g_stat_broadcasts = 0;

static void do_cs_work(void) {
    int cur = __atomic_add_fetch(&g_in_cs, 1, __ATOMIC_SEQ_CST);
    if (cur != 1) {
        __atomic_add_fetch(&g_cs_violations, 1, __ATOMIC_SEQ_CST);
    }
    /* Burn tiny cycles in CS */
    for (volatile int i = 0; i < 50; i++) {}
    __atomic_sub_fetch(&g_in_cs, 1, __ATOMIC_SEQ_CST);
}

static void *stress_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    HosThread t;
    memset(&t, 0, sizeof t);
    snprintf(t.name, sizeof t.name, "Worker_%d", id);
    test_self_thread = &t;

    Ctx c;
    memset(&c, 0, sizeof c);

    unsigned int seed = (unsigned int)id + 1337;

    while (!g_stop) {
        int is_consumer = (id < 6);
        int op;
        if (is_consumer) {
            /* Consumers do lock, trylock, timedlock, and waits */
            int r = rand_r(&seed) % 4;
            if (r == 0) op = 0;      /* Lock */
            else if (r == 1) op = 1; /* TryLock */
            else if (r == 2) op = 2; /* TimedLock */
            else op = 3;             /* Wait / TimedWait */
        } else {
            /* Producers do lock, trylock, timedlock, signals and broadcasts */
            int r = rand_r(&seed) % 5;
            if (r == 0) op = 0;      /* Lock */
            else if (r == 1) op = 1; /* TryLock */
            else if (r == 2) op = 2; /* TimedLock */
            else if (r == 3) op = 4; /* Signal */
            else op = 5;             /* Broadcast */
        }
        switch (op) {
        case 0: { /* LockMutex + recursive lock */
            c.x[0] = MUTEX_ADDR;
            imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE(&c);
            /* Recursive lock */
            c.x[0] = MUTEX_ADDR;
            imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE(&c);

            do_cs_work();
            __atomic_add_fetch(&g_stat_locks, 1, __ATOMIC_RELAXED);

            /* Unlock inner and outer */
            c.x[0] = MUTEX_ADDR;
            imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE(&c);
            c.x[0] = MUTEX_ADDR;
            imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE(&c);
            break;
        }
        case 1: { /* TryLockMutex */
            c.x[0] = MUTEX_ADDR;
            imp__ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE(&c);
            if (c.x[0]) {
                do_cs_work();
                __atomic_add_fetch(&g_stat_trylocks, 1, __ATOMIC_RELAXED);
                c.x[0] = MUTEX_ADDR;
                imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE(&c);
            }
            break;
        }
        case 2: { /* TimedLockMutex (short timeout: 100us - 2ms) */
            c.x[0] = MUTEX_ADDR;
            c.x[1] = (rand_r(&seed) % 2000000) + 100000; /* nanoseconds */
            imp__ZN2nn2os14TimedLockMutexEPNS0_9MutexTypeENS_8TimeSpanE(&c);
            if (c.x[0]) {
                do_cs_work();
                __atomic_add_fetch(&g_stat_timedlocks, 1, __ATOMIC_RELAXED);
                c.x[0] = MUTEX_ADDR;
                imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE(&c);
            }
            break;
        }
        case 3: { /* WaitConditionVariable / TimedWaitConditionVariable */
            c.x[0] = MUTEX_ADDR;
            imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE(&c);
            int timed = rand_r(&seed) % 2;
            if (timed) {
                c.x[0] = COND_ADDR;
                c.x[1] = MUTEX_ADDR;
                c.x[2] = 200000; /* 200 us */
                imp__ZN2nn2os26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE(&c);
                __atomic_add_fetch(&g_stat_timedwaits, 1, __ATOMIC_RELAXED);
            } else {
                if (g_cond_predicate <= 0 && !g_stop) {
                    c.x[0] = COND_ADDR;
                    c.x[1] = MUTEX_ADDR;
                    imp__ZN2nn2os21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE(&c);
                    __atomic_add_fetch(&g_stat_waits, 1, __ATOMIC_RELAXED);
                }
                if (g_cond_predicate > 0) g_cond_predicate--;
            }
            do_cs_work();
            c.x[0] = MUTEX_ADDR;
            imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE(&c);
            break;
        }
        case 4: { /* SignalConditionVariable */
            c.x[0] = MUTEX_ADDR;
            imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE(&c);
            g_cond_predicate++;
            c.x[0] = COND_ADDR;
            imp__ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE(&c);
            __atomic_add_fetch(&g_stat_signals, 1, __ATOMIC_RELAXED);
            c.x[0] = MUTEX_ADDR;
            imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE(&c);
            break;
        }
        case 5: { /* BroadcastConditionVariable */
            c.x[0] = MUTEX_ADDR;
            imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE(&c);
            g_cond_predicate += 2;
            c.x[0] = COND_ADDR;
            imp__ZN2nn2os26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE(&c);
            __atomic_add_fetch(&g_stat_broadcasts, 1, __ATOMIC_RELAXED);
            c.x[0] = MUTEX_ADDR;
            imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE(&c);
            break;
        }
        }
        usleep(10);
    }

    test_self_thread = NULL;
    return NULL;
}

int main(void) {
    printf("Starting sync stress test: %d threads, %d seconds...\n", NUM_THREADS, TEST_DURATION_SECS);

    /* Initialize main test thread */
    HosThread main_t;
    memset(&main_t, 0, sizeof main_t);
    snprintf(main_t.name, sizeof main_t.name, "TestMain");
    test_self_thread = &main_t;

    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[0] = MUTEX_ADDR;
    c.x[1] = 0;
    c.x[2] = 0;
    imp__ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi(&c);

    c.x[0] = COND_ADDR;
    imp__ZN2nn2os27InitializeConditionVariableEPNS0_21ConditionVariableTypeE(&c);

    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        int rc = pthread_create(&threads[i], NULL, stress_worker, (void *)(intptr_t)i);
        if (rc != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            return 1;
        }
    }

    /* Run for TEST_DURATION_SECS */
    sleep(TEST_DURATION_SECS);

    /* Signal threads to stop */
    g_stop = 1;

    /* Wake any threads on the condvar */
    c.x[0] = MUTEX_ADDR;
    imp__ZN2nn2os9LockMutexEPNS0_9MutexTypeE(&c);
    g_cond_predicate = 100000;
    c.x[0] = COND_ADDR;
    imp__ZN2nn2os26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE(&c);
    c.x[0] = MUTEX_ADDR;
    imp__ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE(&c);

    /* Join all threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("All threads joined successfully.\n");
    printf("Stats:\n");
    printf("  Locks:        %ld\n", g_stat_locks);
    printf("  TryLocks:     %ld\n", g_stat_trylocks);
    printf("  TimedLocks:   %ld\n", g_stat_timedlocks);
    printf("  CondWaits:    %ld\n", g_stat_waits);
    printf("  TimedWaits:   %ld\n", g_stat_timedwaits);
    printf("  Signals:      %ld\n", g_stat_signals);
    printf("  Broadcasts:   %ld\n", g_stat_broadcasts);
    printf("  Violations:   %d\n", g_cs_violations);

    if (g_cs_violations > 0) {
        fprintf(stderr, "FAILURE: %d mutual exclusion violations detected!\n", g_cs_violations);
        return 1;
    }

    assert(g_stat_locks > 0);
    assert(g_stat_trylocks > 0);
    assert(g_stat_timedlocks > 0);
    assert(g_stat_timedwaits > 0);
    assert(g_stat_signals > 0);
    assert(g_stat_broadcasts > 0);

    printf("PASS: All tests passed with 0 violations.\n");
    return 0;
}
