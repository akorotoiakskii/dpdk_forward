#define _GNU_SOURCE
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"
#include "dpdk_controller.h"
#include "logger.h"
#include "packets_handler.h"
#include "scheduler.h"

void signal_handler(int signum) {
    LOG(INFO, "Received SIGNAL: %d", signum);
    if (signum == SIGINT || signum == SIGTERM) {
        LOG(INFO, "Stopping program");
        dpdk_stop_process();
    }
}

int log_init(int argc, char **argv) {
    char bin_path[PATH_MAX] = {0};
    char main_path[PATH_MAX] = {0};
    char log_path[PATH_MAX] = {0};

    // Get the directory of the binary
    if (realpath(argv[0], bin_path) == NULL) {
        fprintf(stderr, "Error: Invalid or unresolved binary path\n");
        return -1;
    }

    char *last_slash = strrchr(bin_path, '/');
    if (last_slash) {
        *last_slash = '\0';  // Remove the binary name to get the directory
    } else {
        fprintf(stderr, "Error: Invalid binary path\n");
        return -1;
    }
    printf("Path of bin directory: %s\n", bin_path);

    // Get the parent directory
    strncpy(main_path, bin_path, sizeof(main_path) - 1);
    main_path[sizeof(main_path) - 1] = '\0';  // Ensure null-termination
    last_slash = strrchr(main_path, '/');
    if (last_slash) {
        *last_slash =
            '\0';  // Remove the last directory to get the parent directory
    } else {
        fprintf(stderr, "Error: Invalid main path\n");
        return -1;
    }
    printf("Path of app directory: %s\n", main_path);

    // Construct the log directory path
    if (snprintf(log_path, sizeof(log_path), "%s/log", main_path) >=
        sizeof(log_path)) {
        fprintf(stderr, "Error: Log path is too long\n");
        return -1;
    }
    printf("Path of log directory: %s\n", log_path);

    printf("Initializing logging\n");
    if (logger_init(log_path) != 0) {
        fprintf(stderr, "Failed to initialize logger\n");
        return -1;
    }
    printf("Log is started\n");
    LOG(INFO, "Log is started");

    return 0;
}

void print_args(int argc, char **argv) {
    char argc_str[1024] = {0};
    for (int i = 0; i < argc; i++) {
        strncat(argc_str, argv[i], sizeof(argc_str) - strlen(argc_str) - 1);
        strncat(argc_str, " ", sizeof(argc_str) - strlen(argc_str) - 1);
    }
    LOG(INFO, "Argc: %s", argc_str);
}

int main(int argc, char **argv) {
    printf("START program\n");

    if (log_init(argc, argv) != 0) {
        printf("Failed to initialize logging");
        return -1;
    }

    print_args(argc, argv);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* INIT */
    LOG(INFO, "Begin initializing ...");

    uint16_t *free_cores = NULL;
    int free_cores_count = 0;

    dpdk_init(argc, argv, &free_cores, &free_cores_count);

    if (free_cores_count == 0) {
        LOG(ERR, "No free cores");
        printf("Error: No free cores \n");
        exit(-1);
    }
    LOG(INFO, "Free cores: ");
    for (int i = 0; i < free_cores_count; i++) {
        LOG(INFO, "%d ", free_cores[i]);
    }

    LOG(INFO, "Set new core ID: %d", free_cores[0]);
    printf("Set new core ID: %d\n", free_cores[0]);

    stick_this_thread_to_core(free_cores[0]);

    handler_init();
    sched_init(free_cores[0]);

    LOG(INFO, "Initialization is done");

    sched_add_func(fetch_all_stats, 1000);
    sched_add_func(check_log_rotate, 100);

    LOG(INFO, "Start processing...");

    sched_start();

    dpdk_launch_process();

    /* STOP */
    LOG(INFO, "Stop program");

    sched_stop();
    dpdk_ports_fini();
    handler_clean();
    logger_close();

    printf("Exit program\n");
    return 0;
}
