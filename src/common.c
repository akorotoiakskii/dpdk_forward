#define _GNU_SOURCE
#include "common.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "logger.h"

int stick_this_thread_to_core(int core_id) {
    LOG(INFO, "Core ID: %d", core_id);

    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (core_id < 0 || core_id >= num_cores) {
        LOG(ERR, "Invalid core ID: %d", core_id);
        printf("Error: Invalid core ID: %d\n", core_id);
        return EINVAL;
    }

    int curreent_core_id = sched_getcpu();
    if (curreent_core_id == -1) {
        perror("sched_getcpu failed");
        LOG(ERR, "sched_getcpu failed");
    } else {
        LOG(INFO, "Current core ID: %d", curreent_core_id);
        printf("Current core ID: %d\n", curreent_core_id);
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    pthread_t current_thread = pthread_self();
    int ret =
        pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    if (ret != 0) {
        perror("pthread_setaffinity_np failed");
        LOG(ERR, "pthread_setaffinity_np failed");
        return ret;
    }

    // check after setting core
    curreent_core_id = sched_getcpu();
    if (curreent_core_id == -1) {
        perror("sched_getcpu failed");
        LOG(ERR, "sched_getcpu failed");
    } else {
        LOG(INFO, "New core ID: %d", curreent_core_id);
        printf("New core ID: %d\n", curreent_core_id);
    }
    return ret;
}
