#pragma once
#include <stdbool.h>
bool audio_init(void);

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
extern TaskHandle_t g_audio_task;
