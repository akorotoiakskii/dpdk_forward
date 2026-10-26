#ifndef SCHEDULER_H
#define SCHEDULER_H
#include <stdint.h>

// Maximum number of tasks that can be scheduled
#define MAX_TASKS (32)

void sched_init();
// Function to add a task to the scheduler
// Parameters:
// - func: The function to be executed
// - interval_ms: The interval in milliseconds between executions
// Returns 0 on success, -1 if no free slot is available
int sched_add_func(void (*func)(void), uint64_t interval_ms);
void sched_start();
void sched_stop();

#endif // SCHEDULER_H
