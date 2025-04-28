#define _GNU_SOURCE
#include "scheduler.h"

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "logger.h"

typedef struct {
    void (*func)(void);
    uint64_t interval_ms;
    uint64_t next_call_ms;
    int active;
} ScheduledTask;

static ScheduledTask tasks[MAX_TASKS];
static pthread_t scheduler_thread;
static int scheduler_running = 0;
static int scheduler_initialized = 0;

static uint64_t current_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int core_nb = 0;

void sched_init(int core) {
    if (core < 0 || core >= RTE_MAX_LCORE) {
        LOG(ERR, "Invalid core ID: %d", core);
        return;
    }
    core_nb = core;
    if (scheduler_initialized) {
        LOG(WARNING, "Scheduler is already initialized");
        return;
    }
    memset(tasks, 0, sizeof(tasks));
    scheduler_initialized = 1;
    LOG(INFO, "Scheduler initialized");
}

int sched_add_func(void (*func)(void), uint64_t interval_ms) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!tasks[i].active) {
            tasks[i].func = func;
            tasks[i].interval_ms = interval_ms;
            tasks[i].next_call_ms = current_time_ms() + interval_ms;
            tasks[i].active = 1;
            LOG(INFO, "Added task %d with interval %" PRIu64 " ms", i,
                interval_ms);
            return 0;
        }
    }
    LOG(ERR, "Failed to add task: no free slot available");
    return -1;  // No free slot
}

static void *scheduler_loop(void *arg) {
    (void)arg;
    if (!scheduler_initialized) {
        LOG(ERR, "Scheduler not initialized");
        return NULL;
    }
    if (core_nb < 0 || core_nb >= RTE_MAX_LCORE) {
        LOG(ERR, "Invalid core ID: %d", core_nb);
        return NULL;
    }
    // Set the thread to run on the specified core
    stick_this_thread_to_core(core_nb);

    LOG(INFO, "Scheduler loop started");
    while (scheduler_running) {
        uint64_t now = current_time_ms();
        for (int i = 0; i < MAX_TASKS; i++) {
            if (!tasks[i].active) continue;
            if (now >= tasks[i].next_call_ms) {
                tasks[i].func();
                tasks[i].next_call_ms = now + tasks[i].interval_ms;
            }
        }
        usleep(1000);  // Sleep 1ms
    }
    LOG(INFO, "Scheduler loop stopped");
    return NULL;
}

void sched_start() {
    if (!scheduler_running) {
        scheduler_running = 1;
        pthread_create(&scheduler_thread, NULL, scheduler_loop, NULL);
        LOG(INFO, "Scheduler started");
    } else {
        LOG(WARNING, "Scheduler is already running");
    }
}

void sched_stop() {
    if (scheduler_running) {
        scheduler_running = 0;
        pthread_join(scheduler_thread, NULL);
        LOG(INFO, "Scheduler stopped");
    } else {
        LOG(WARNING, "Scheduler is not running");
    }
}