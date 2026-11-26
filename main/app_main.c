// CYD Mac SE: Mini vMac on the Freenove 5" ESP32-S3 board.
//   core 0: the panel's refill interrupt, touch + UI, sound
//   core 1: the emulated Mac
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdlib.h>
#include <time.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio.h"
#include "canvas.h"
#include "crash_report.h"
#include "cyd_mac.h"
#include "hal_esp.h"
#include "panel.h"
#include "sdlog.h"
#include "wifi_web.h"
#include "touch_gt911.h"

static const char* TAG = "macse";

static void emu_task(void* arg)
{
    (void)arg;
    crash_arm_watchpoint();   // core 1
    emu_main();
    ESP_LOGI(TAG, "the Mac was switched off");
    vTaskDelete(NULL);
}

static void ui_task(void* arg)
{
    (void)arg;
    crash_arm_watchpoint();   // core 0
    int n = 0;
    for (;;) {
        int x = 0, y = 0;
        const bool down = touch_read(&x, &y);
        ui_touch(down, x, y);
        if (++n % 3 == 0) ui_tick();
        if (n % 100 == 0) {
            crash_watch_tick();
            sdlog_flush();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "PSRAM free: %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);   // Tom's; wifi.txt's tz= replaces it
    tzset();
    const bool sd = hal_esp_init();
    if (sd) sdlog_init();
    ESP_LOGI(TAG, "start, reset reason %d", (int)esp_reset_reason());
    if (sd) g_wifi_on = wifi_web_start();   // the log page, if mac\wifi.txt says how
    const bool touch = touch_init();   // before the sound: its INT pin is the amp's LRCLK
    if (!panel_init()) return;
    // After a crash the saved disks stay out (one may be the cause)
    const bool crashed = crash_report_show();
    ui_init(!crashed);
    if (!sd) {
        hal_lock();
        cv_fill(MAC_X, MAC_Y, MAC_W, MAC_H, CV_WHITE);
        cv_text_center(MAC_X, 140, MAC_W, 20, "No SD card found.", CV_BLACK);
        cv_text_center(MAC_X, 164, MAC_W, 20, "Put MacSE.ROM and disk images in a folder named mac", CV_BLACK);
        cv_text_center(MAC_X, 184, MAC_W, 20, "on a FAT32 card, put it in, and restart.", CV_BLACK);
        g_mac_blit_enabled = false;
        hal_unlock();
    }
    if (!touch) ESP_LOGE(TAG, "no touch");
    audio_init();
    TaskHandle_t ui = NULL, mac = NULL;
    if (sd) xTaskCreatePinnedToCore(emu_task, "mac", 16384, NULL, 4, &mac, 1);
    crash_watch_tasks(g_audio_task, NULL, mac);
    xTaskCreatePinnedToCore(ui_task, "ui", 6144, NULL, 5, &ui, 0);
    crash_watch_tasks(g_audio_task, ui, mac);
}
