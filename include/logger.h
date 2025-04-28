#ifndef LOGGER_H
#define LOGGER_H

#include <rte_log.h>
#include <time.h>
#include <stdio.h>

#define MAX_LOG_SIZE (16 * 1024 * 1024) // 16 MB
#define MAX_LOG_FILES 64 // 1GB for logs

#ifndef RTE_LOGTYPE_FWD
#define RTE_LOGTYPE_FWD RTE_LOGTYPE_USER1
#endif

// Helper macro to get the current timestamp
#define TIMESTAMP_BUFFER_SIZE 64
#define GET_TIMESTAMP() ({ \
    char buffer[TIMESTAMP_BUFFER_SIZE]; \
    time_t now = time(NULL); \
    struct tm *tm_info = localtime(&now); \
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info); \
    buffer; \
})

// Updated LOG macro to include a timestamp
#define LOG(level, fmt, args...) \
    RTE_LOG(level, FWD, "[%s] [%s:%s:%d] " fmt "\n", GET_TIMESTAMP(), __FILE__, __func__, __LINE__, ##args)

int logger_init(const char *log_filename);
void check_log_rotate();
void logger_close();

#endif // LOGGER_H