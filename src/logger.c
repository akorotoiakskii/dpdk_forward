#include "logger.h"

#include <dirent.h>
#include <errno.h>
#include <rte_log.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static FILE *log_fp = NULL;
static char log_directory[256];
static int current_log_index = 0;

static void delete_oldest_log() {
    // Calculate the index of the oldest log file to keep
    int oldest_index = current_log_index - MAX_LOG_FILES;
    if (oldest_index < 0) {
        return;  // No files to delete
    }

    // Scan the directory to find and delete files with indices <= oldest_index
    DIR *dir = opendir(log_directory);
    if (!dir) {
        perror("opendir");
        return;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        // Check if the file matches the pattern and contains an index
        int index;
        if (strstr(entry->d_name, ".log") &&
            sscanf(entry->d_name, "%*[^_]_%*[^_]_%d.log", &index) == 1 &&
            index <= oldest_index) {
            // Construct the full path to the file
            char file_path[300];
            int written = snprintf(file_path, sizeof(file_path), "%s/%s",
                                   log_directory, entry->d_name);

            // Check for truncation
            if (written < 0 || written >= sizeof(file_path)) {
                fprintf(stderr, "Error: Log file path is too long\n");
                continue;
            }

            // Attempt to delete the file
            if (unlink(file_path) != 0) {
                perror("Error deleting log file");
            } else {
                LOG(INFO, "Deleted log file: %s", file_path);
            }
        }
    }

    closedir(dir);
}

int scan_log_directory() {
    DIR *dir = opendir(log_directory);
    if (!dir) {
        perror("opendir");
        return -1;
    }

    struct dirent *entry;
    int max_index = -1;

    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".log")) {
            int index;
            if (sscanf(entry->d_name, "%*[^_]_%*[^_]_%d.log", &index) == 1) {
                if (index > max_index) {
                    max_index = index;
                }
            }
        }
    }

    closedir(dir);
    return max_index + 1;
}

int create_new_log_file() {
    // Create a new log file with a timestamp
    char timestamp[64];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);

    char new_log[300];
    int written = snprintf(new_log, sizeof(new_log), "%s/%s_%d.log",
                           log_directory, timestamp, current_log_index++);

    // Check for truncation
    if (written < 0 || written >= sizeof(new_log)) {
        fprintf(stderr, "Error: Log file path is too long\n");
        return -1;
    }

    log_fp = fopen(new_log, "a");
    if (!log_fp) {
        perror("Error opening new log file");
        return -1;
    }

    rte_openlog_stream(log_fp);
    LOG(INFO, "Created new log file: %s", new_log);
    return 0;
}

void rotate_logs() {
    if (log_fp) fclose(log_fp);

    // Scan the directory to determine the next log index
    current_log_index = scan_log_directory();
    if (current_log_index < 0) {
        return;
    }

    // Delete the oldest log file if the limit is reached
    if (current_log_index >= MAX_LOG_FILES) {
        delete_oldest_log();
    }

    // Create a new log file
    if (create_new_log_file() < 0) {
        fprintf(stderr, "Error rotating logs\n");
    }
}

int logger_init(const char *directory) {
    // Ensure the directory exists
    if (mkdir(directory, 0755) && errno != EEXIST) {
        perror("mkdir");
        return -1;
    }

    // Save the directory path
    strncpy(log_directory, directory, sizeof(log_directory) - 1);
    log_directory[sizeof(log_directory) - 1] = '\0';

    rotate_logs();

    return 0;
}

void check_log_rotate() {
    struct stat st;
    if (log_fp && fstat(fileno(log_fp), &st) == 0 &&
        st.st_size > MAX_LOG_SIZE) {
        rotate_logs();
        LOG(INFO, "Log rotated due to size limit");
    }
}

void logger_close() {
    if (log_fp) fclose(log_fp);
}
