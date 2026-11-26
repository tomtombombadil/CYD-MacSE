#pragma once
#include <stdbool.h>
// The canvas lock and the SD card (mounted at /sd)
bool hal_esp_init(void);

extern bool g_wifi_on;   // wifi.txt found and Wi-Fi started
