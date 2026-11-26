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
#include <esp_cpu.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio.h"
#include "canvas.h"
#include "crash_report.h"
#include "sdlog.h"
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
    uint32_t uptime_s, mac_ticks, drives;   // at the last 1 s check
    uint32_t trap_addr;       // the audio stack's end, when found overwritten
    uint8_t trap[32];         // its first 32 bytes then
    int64_t wall;             // the clock at the last 1 s check (0: not set)
    uint32_t trap_bad;        // how many of its first 1024 bytes were not 0xA5
    uint32_t trail_n;         // disk code steps (hal_trail) since the start
    struct { uint32_t tick; uint32_t arg; char step; } trail[12];   // the last ones
} watch_t;
static RTC_NOINIT_ATTR watch_t s_watch;
static TaskHandle_t s_tasks[3];

void crash_watch_tasks(TaskHandle_t audio, TaskHandle_t ui, TaskHandle_t mac)
{
    s_tasks[0] = audio;
    s_tasks[1] = ui;
    s_tasks[2] = mac;
}

// The disk code's steps (osglu_cyd.c): the last 12, for the next report
void hal_trail(char step, uint32_t arg)
{
    const uint32_t i = s_watch.trail_n++ % 12;
    s_watch.trail[i].tick = g_emu.ticks;
    s_watch.trail[i].arg = arg;
    s_watch.trail[i].step = step;
}

// Stop the board with a note for the next start's report
static void stop_with(const char* what)
{
    snprintf(s_watch.note, sizeof s_watch.note, "%s", what);
    ESP_LOGE(TAG, "%s", what);
    abort();
}

// From the audio task, every 256 samples: its own stack's end (FreeRTOS
// fills it with 0xA5; its overflow check looks at the first 20 bytes)
void crash_check_audio_stack(const char* when)
{
    const uint8_t* end = pxTaskGetStackStart(NULL);
    if (!end) return;
    bool ok = true;
    for (int i = 0; i < 32; ++i) ok &= end[i] == 0xA5;
    if (ok) return;
    s_watch.trap_addr = (uint32_t)(uintptr_t)end;
    memcpy(s_watch.trap, end, 32);
    uint32_t bad = 0;
    for (int i = 0; i < 1024; ++i) bad += end[i] != 0xA5;
    s_watch.trap_bad = bad;
    char what[64];
    snprintf(what, sizeof what, "Audio stack end overwritten (%s)", when);
    stop_with(what);
}

// A hardware watchpoint on the audio stack's end, on the core this runs
// on (each core has its own): whatever code stores there stops the board
// with its own address in the crash report. If the report then still
// says "Audio stack end overwritten", a DMA did it (no watchpoint sees
// DMA).
void crash_arm_watchpoint(void)
{
    if (!g_audio_task) return;
    const uintptr_t end = (uintptr_t)pxTaskGetStackStart(g_audio_task);
    if (!end) return;
    const uintptr_t a = (end + 63) & ~(uintptr_t)63;
    esp_cpu_set_watchpoint(0, (const void*)a, 64, ESP_CPU_WATCHPOINT_STORE);
    ESP_LOGI(TAG, "watching %08lx on core %d", (unsigned long)a, xPortGetCoreID());
}

void crash_check_after_io(int is_write, uint32_t start, uint32_t count, const void* buf)
{
    s_watch.io_n++;
    s_watch.io_write = (uint32_t)is_write;
    s_watch.io_start = start;
    s_watch.io_count = count;
    s_watch.io_buf = (uint32_t)(uintptr_t)buf;
    // No heap walk here any more (0.1.11): it holds the heap's lock with
    // interrupts off; on a damaged heap it can spin until the interrupt
    // watchdog (0.1.9 / 0.1.10's reports), and it starves the panel.
    if (g_audio_task && uxTaskGetStackHighWaterMark(g_audio_task) < 32)
        stop_with(is_write ? "Audio stack end overwritten (after a disk write)"
                           : "Audio stack end overwritten (after a disk read)");
}

void crash_watch_tick(void)
{
    s_watch.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_watch.wall = hal_unix_time();
    s_watch.mac_ticks = g_emu.ticks;
    s_watch.drives = g_emu.drives;
    static unsigned s_n;
    if (++s_n % 10 == 0)
        ESP_LOGI(TAG, "up %lu s, Mac ticks %lu, drives %lx, free internal %u, least stack audio %ld ui %ld mac %ld",
                 (unsigned long)s_watch.uptime_s, (unsigned long)s_watch.mac_ticks, (unsigned long)s_watch.drives,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (long)s_watch.free_stack[0],
                 (long)s_watch.free_stack[1], (long)s_watch.free_stack[2]);
    if (g_audio_task && uxTaskGetStackHighWaterMark(g_audio_task) < 32)
        stop_with("Audio stack end overwritten (1 s check)");
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
    s_watch.trail_n = 0;
    s_watch.note[0] = 0;
    s_watch.trap_addr = 0;
    s_watch.uptime_s = s_watch.mac_ticks = s_watch.drives = 0;
    s_watch.wall = 0;
    if (!why) {
        esp_core_dump_image_erase();   // an old one, from before a normal start
        return false;
    }

    char lines[24][96];
    int n = 0;
    snprintf(lines[n++], 96, "The board restarted after %s.", why);
    snprintf(lines[n++], 96, "Firmware %s, crash %lu in a row", APP_VERSION, (unsigned long)s_watch.crashes);
    if (valid)
        snprintf(lines[n++], 96, "Least free stack: audio %ld, ui %ld, mac %ld",
                 last.free_stack[0] == UINT32_MAX ? -1L : (long)last.free_stack[0],
                 last.free_stack[1] == UINT32_MAX ? -1L : (long)last.free_stack[1],
                 last.free_stack[2] == UINT32_MAX ? -1L : (long)last.free_stack[2]);

    if (valid && last.wall) {
        const time_t t = (time_t)last.wall;
        struct tm lt;
        localtime_r(&t, &lt);
        char b[40];
        strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &lt);
        snprintf(lines[n++], 96, "Crashed at about %s", b);
    }
    if (valid)
        snprintf(lines[n++], 96, "Up %lu s, Mac ticks %lu, drives %lx", (unsigned long)last.uptime_s,
                 (unsigned long)last.mac_ticks, (unsigned long)last.drives);
    if (valid && last.note[0]) snprintf(lines[n++], 96, "%.60s", last.note);
    if (valid && last.trap_addr) {
        snprintf(lines[n++], 96, "At %08lx, %lu bytes changed:", (unsigned long)last.trap_addr,
                 (unsigned long)last.trap_bad);
        for (int h = 0; h < 2; ++h) {
            char* p = lines[n++];
            p[0] = 0;
            for (int i = 0; i < 16; ++i) snprintf(p + strlen(p), 96 - strlen(p), "%02x", last.trap[h * 16 + i]);
        }
    }
    if (valid && last.io_n)
        snprintf(lines[n++], 96, "Disk I/O %lu, last: %s %lu bytes at %lu, buf %08lx", (unsigned long)last.io_n,
                 last.io_write ? "write" : "read", (unsigned long)last.io_count, (unsigned long)last.io_start,
                 (unsigned long)last.io_buf);
    if (valid && last.trail_n) {
        // Disk steps, oldest first: o open, f opened, s size, h volume at,
        // c read into memory, n Mac told, r / w transfer from, d done, e out
        const uint32_t k = last.trail_n < 12 ? last.trail_n : 12;
        snprintf(lines[n++], 96, "Disk steps (%lu), letter tick:number:", (unsigned long)last.trail_n);
        for (uint32_t h = 0; h < k; h += 4) {
            char* p = lines[n++];
            p[0] = 0;
            for (uint32_t j = h; j < h + 4 && j < k; ++j) {
                const uint32_t i = (last.trail_n - k + j) % 12;
                snprintf(p + strlen(p), 96 - strlen(p), "%c %lu:%lu  ", last.trail[i].step,
                         (unsigned long)last.trail[i].tick, (unsigned long)last.trail[i].arg);
            }
        }
    }
    esp_core_dump_summary_t* s = calloc(1, sizeof *s);
    if (s && ESP_OK == esp_core_dump_image_check() && ESP_OK == esp_core_dump_get_summary(s)) {
        snprintf(lines[n++], 96, "Task %.16s, PC %08lx", s->exc_task, (unsigned long)s->exc_pc);
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        snprintf(lines[n++], 96, "Cause %lu, address %08lx",
                 (unsigned long)s->ex_info.exc_cause, (unsigned long)s->ex_info.exc_vaddr);
#endif
        const int depth = s->exc_bt_info.depth < 16 ? (int)s->exc_bt_info.depth : 16;
        for (int i = 0; i < depth && n < 22; i += 4) {
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
    {
        const char* l[24];
        for (int i = 0; i < n; ++i) l[i] = lines[i];
        sdlog_crash(l, n);
    }

    hal_lock();
    g_mac_blit_enabled = false;
    cv_fill(MAC_X, MAC_Y, MAC_W, MAC_H, CV_WHITE);
    cv_frame(MAC_X + 8, MAC_Y + 8, MAC_W - 16, MAC_H - 16, CV_BLACK);
    int y = MAC_Y + 24;
    for (int i = 0; i < n && y < MAC_Y + MAC_H - 110; ++i, y += 16) cv_text_fit(MAC_X + 24, y, MAC_W - 48, lines[i], CV_BLACK);
    y += 6;
    cv_text_fit(MAC_X + 24, y, MAC_W - 48, "Please send a photo of this screen.", CV_BLACK);
    cv_text_fit(MAC_X + 24, y + 18, MAC_W - 48, "The disks were not put back in.", CV_BLACK);
    cv_text_center(MAC_X, MAC_Y + MAC_H - 40, MAC_W, 20, "Tap to go on (or wait 60 seconds)", CV_BLACK);
    hal_unlock();

    // A tap anywhere (down, then up), or 60 seconds
    int x, yy;
    int t = 0;
    bool shown = false;
    while (!touch_read(&x, &yy) && t < 3000) {
        const char* a = hal_net_address();
        if (!shown && a && a[0]) {
            char l[64];
            snprintf(l, sizeof l, "Or open http://%s/ and copy the page.", a);
            hal_lock();
            cv_text_center(MAC_X, MAC_Y + MAC_H - 62, MAC_W, 20, l, CV_BLACK);
            hal_unlock();
            shown = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        ++t;
    }
    while (touch_read(&x, &yy) && t < 3000) { vTaskDelay(pdMS_TO_TICKS(20)); ++t; }

    hal_lock();
    cv_fill(MAC_X, MAC_Y, MAC_W, MAC_H, CV_WHITE);
    g_mac_blit_enabled = true;
    hal_unlock();
    return true;
}
