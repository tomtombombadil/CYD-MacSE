// After the board restarts by itself (a crash, a watchdog, a brown-out):
// say so on the Mac's screen, with the crash's place in the firmware from
// the core dump (flash partition "coredump"), and wait for a tap. The
// caller then leaves the saved disks out, so a disk that crashes the
// board doesn't do it again at every start.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio.h"
#include "canvas.h"
#include "crash_report.h"
#include "cyd_mac.h"
#include "touch_gt911.h"

static const char* TAG = "crash";

// Kept over a restart (not over a power-off): the tasks' least free stack
// seen, and how many crash restarts in a row
#define WATCH_MAGIC 0x4D414353u
typedef struct {
    uint32_t magic;
    uint32_t crashes;
    uint32_t free_stack[3];   // audio, ui, mac: bytes, least seen
    uint32_t io_n;            // disk transfers since the start
    uint32_t io_write, io_start, io_count, io_buf;   // the last one
    char note[64];            // what a check found, before it stopped the board
} watch_t;
static RTC_NOINIT_ATTR watch_t s_watch;
static TaskHandle_t s_tasks[3];

void crash_watch_tasks(TaskHandle_t audio, TaskHandle_t ui, TaskHandle_t mac)
{
    s_tasks[0] = audio;
    s_tasks[1] = ui;
    s_tasks[2] = mac;
}

// Stop the board with a note for the next start's report
static void stop_with(const char* what)
{
    snprintf(s_watch.note, sizeof s_watch.note, "%s", what);
    ESP_LOGE(TAG, "%s", what);
    abort();
}

static void check_memory(const char* when)
{
    char what[64];
    if (!heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true)) {
        snprintf(what, sizeof what, "Internal heap damaged (%s)", when);
        stop_with(what);
    }
    if (g_audio_task && uxTaskGetStackHighWaterMark(g_audio_task) < 32) {
        snprintf(what, sizeof what, "Audio stack end overwritten (%s)", when);
        stop_with(what);
    }
}

void crash_check_after_io(int is_write, uint32_t start, uint32_t count, const void* buf)
{
    s_watch.io_n++;
    s_watch.io_write = (uint32_t)is_write;
    s_watch.io_start = start;
    s_watch.io_count = count;
    s_watch.io_buf = (uint32_t)(uintptr_t)buf;
    check_memory(is_write ? "after a disk write" : "after a disk read");
}

void crash_watch_tick(void)
{
    check_memory("1 s check");
    for (int i = 0; i < 3; ++i) {
        if (!s_tasks[i]) continue;
        const uint32_t f = (uint32_t)uxTaskGetStackHighWaterMark(s_tasks[i]);
        if (f < s_watch.free_stack[i]) s_watch.free_stack[i] = f;
    }
}

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
    const bool valid = s_watch.magic == WATCH_MAGIC && (r == ESP_RST_SW || why);
    const watch_t last = s_watch;
    s_watch.magic = WATCH_MAGIC;
    s_watch.crashes = why ? (valid ? last.crashes : 0) + 1 : 0;
    for (int i = 0; i < 3; ++i) s_watch.free_stack[i] = UINT32_MAX;
    s_watch.io_n = 0;
    s_watch.note[0] = 0;
    if (!why) {
        esp_core_dump_image_erase();   // an old one, from before a normal start
        return false;
    }

    char lines[16][96];
    int n = 0;
    snprintf(lines[n++], 96, "The board restarted after %s.", why);
    snprintf(lines[n++], 96, "Firmware %s, crash %lu in a row", APP_VERSION, (unsigned long)s_watch.crashes);
    if (valid)
        snprintf(lines[n++], 96, "Least free stack: audio %ld, ui %ld, mac %ld",
                 last.free_stack[0] == UINT32_MAX ? -1L : (long)last.free_stack[0],
                 last.free_stack[1] == UINT32_MAX ? -1L : (long)last.free_stack[1],
                 last.free_stack[2] == UINT32_MAX ? -1L : (long)last.free_stack[2]);

    if (valid && last.note[0]) snprintf(lines[n++], 96, "%.60s", last.note);
    if (valid && last.io_n)
        snprintf(lines[n++], 96, "Disk I/O %lu, last: %s %lu bytes at %lu, buf %08lx", (unsigned long)last.io_n,
                 last.io_write ? "write" : "read", (unsigned long)last.io_count, (unsigned long)last.io_start,
                 (unsigned long)last.io_buf);
    esp_core_dump_summary_t* s = calloc(1, sizeof *s);
    if (s && ESP_OK == esp_core_dump_image_check() && ESP_OK == esp_core_dump_get_summary(s)) {
        snprintf(lines[n++], 96, "Task %.16s, PC %08lx", s->exc_task, (unsigned long)s->exc_pc);
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        snprintf(lines[n++], 96, "Cause %lu, address %08lx",
                 (unsigned long)s->ex_info.exc_cause, (unsigned long)s->ex_info.exc_vaddr);
#endif
        const int depth = s->exc_bt_info.depth < 16 ? (int)s->exc_bt_info.depth : 16;
        for (int i = 0; i < depth && n < 14; i += 4) {
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
    for (int i = 0; i < n; ++i, y += 18) cv_text_fit(MAC_X + 24, y, MAC_W - 48, lines[i], CV_BLACK);
    y += 6;
    cv_text_fit(MAC_X + 24, y, MAC_W - 48, "Please send a photo of this screen.", CV_BLACK);
    cv_text_fit(MAC_X + 24, y + 18, MAC_W - 48, "The disks were not put back in.", CV_BLACK);
    cv_text_center(MAC_X, MAC_Y + MAC_H - 40, MAC_W, 20, "Tap to go on (or wait 30 seconds)", CV_BLACK);
    hal_unlock();

    // A tap anywhere (down, then up), or 30 seconds
    int x, yy;
    int t = 0;
    while (!touch_read(&x, &yy) && t < 1500) { vTaskDelay(pdMS_TO_TICKS(20)); ++t; }
    while (touch_read(&x, &yy) && t < 1500) { vTaskDelay(pdMS_TO_TICKS(20)); ++t; }

    hal_lock();
    cv_fill(MAC_X, MAC_Y, MAC_W, MAC_H, CV_WHITE);
    g_mac_blit_enabled = true;
    hal_unlock();
    return true;
}
