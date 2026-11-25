// After the board restarts by itself (a crash, a watchdog, a brown-out):
// say so on the Mac's screen, with the crash's place in the firmware from
// the core dump (flash partition "coredump"), and wait for a tap. The
// caller then leaves the saved disks out, so a disk that crashes the
// board doesn't do it again at every start.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_core_dump.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "canvas.h"
#include "crash_report.h"
#include "cyd_mac.h"
#include "touch_gt911.h"

static const char* TAG = "crash";

static const char* reason_text(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_PANIC: return "a crash (panic)";
        case ESP_RST_INT_WDT: return "a crash (interrupt watchdog)";
        case ESP_RST_TASK_WDT: return "a crash (task watchdog)";
        case ESP_RST_WDT: return "a crash (watchdog)";
        case ESP_RST_BROWNOUT: return "a brown-out (the power dipped)";
        default: return NULL;
    }
}

bool crash_report_show(void)
{
    const esp_reset_reason_t r = esp_reset_reason();
    const char* why = reason_text(r);
    if (!why) {
        esp_core_dump_image_erase();   // an old one, from before a normal start
        return false;
    }

    char lines[12][96];
    int n = 0;
    snprintf(lines[n++], 96, "The board restarted after %s.", why);
    snprintf(lines[n++], 96, "Firmware %s", APP_VERSION);

    esp_core_dump_summary_t* s = calloc(1, sizeof *s);
    if (s && ESP_OK == esp_core_dump_image_check() && ESP_OK == esp_core_dump_get_summary(s)) {
        snprintf(lines[n++], 96, "Task %.16s, PC %08lx", s->exc_task, (unsigned long)s->exc_pc);
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        snprintf(lines[n++], 96, "Cause %lu, address %08lx",
                 (unsigned long)s->ex_info.exc_cause, (unsigned long)s->ex_info.exc_vaddr);
#endif
        const int depth = s->exc_bt_info.depth < 16 ? (int)s->exc_bt_info.depth : 16;
        for (int i = 0; i < depth && n < 10; i += 4) {
            char* p = lines[n++];
            p[0] = 0;
            for (int j = i; j < i + 4 && j < depth; ++j)
                snprintf(p + strlen(p), 96 - strlen(p), "%08lx ", (unsigned long)s->exc_bt_info.bt[j]);
        }
    } else {
        snprintf(lines[n++], 96, "No crash details were saved.");
    }
    free(s);
    esp_core_dump_image_erase();

    for (int i = 0; i < n; ++i) ESP_LOGE(TAG, "%s", lines[i]);

    hal_lock();
    g_mac_blit_enabled = false;
    cv_fill(MAC_X, MAC_Y, MAC_W, MAC_H, CV_WHITE);
    cv_frame(MAC_X + 8, MAC_Y + 8, MAC_W - 16, MAC_H - 16, CV_BLACK);
    int y = MAC_Y + 24;
    for (int i = 0; i < n; ++i, y += 20) cv_text_fit(MAC_X + 24, y, MAC_W - 48, lines[i], CV_BLACK);
    y += 12;
    cv_text_fit(MAC_X + 24, y, MAC_W - 48, "Please send a photo of this screen.", CV_BLACK);
    cv_text_fit(MAC_X + 24, y + 20, MAC_W - 48, "The disks were not put back in.", CV_BLACK);
    cv_text_center(MAC_X, MAC_Y + MAC_H - 40, MAC_W, 20, "Tap to go on", CV_BLACK);
    hal_unlock();

    // A tap anywhere: down, then up
    int x, yy;
    while (!touch_read(&x, &yy)) vTaskDelay(pdMS_TO_TICKS(20));
    while (touch_read(&x, &yy)) vTaskDelay(pdMS_TO_TICKS(20));

    hal_lock();
    cv_fill(MAC_X, MAC_Y, MAC_W, MAC_H, CV_WHITE);
    g_mac_blit_enabled = true;
    hal_unlock();
    return true;
}
