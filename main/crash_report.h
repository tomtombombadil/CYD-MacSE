#pragma once
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// If the last restart was a crash / watchdog / brown-out: show what is
// known on the Mac's screen and wait for a tap. Returns true then.
bool crash_report_show(void);

// Keep track of the tasks' least free stack, for the next crash report
void crash_watch_tasks(TaskHandle_t audio, TaskHandle_t ui, TaskHandle_t mac);
void crash_watch_tick(void);   // about once a second
