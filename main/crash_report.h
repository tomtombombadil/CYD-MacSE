#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// If the last restart was a crash / watchdog / brown-out: show what is
// known on the Mac's screen and wait for a tap. Returns true then.
bool crash_report_show(void);

// Keep track of the tasks' least free stack, for the next crash report
void crash_watch_tasks(TaskHandle_t audio, TaskHandle_t ui, TaskHandle_t mac);
void crash_watch_tick(void);
// After each disk transfer: note it, check the heap and the audio stack
void crash_check_after_io(int is_write, uint32_t start, uint32_t count, const void* buf);   // about once a second
// From the audio task: stop with a note if its stack's end was overwritten
void crash_check_audio_stack(const char* when);
// Watch the audio stack's end with a hardware watchpoint on this core
void crash_arm_watchpoint(void);
