#include "sdlog.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cyd_mac.h"

// Lines wait here until the UI task writes them (logging can come from
// any task; the card is written from one)
#define RING 8192
static char s_ring[RING];
static unsigned s_head, s_tail;   // s_head - s_tail bytes waiting
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_uart;
static FILE* s_file;

static void path(char* out, size_t n, const char* name)
{
    const char* dir = hal_dirs()[0];
    snprintf(out, n, "%s/%s", dir ? dir : "/sd", name);
}

static int log_vprintf(const char* fmt, va_list ap)
{
    char line[192];
    va_list ap2;
    va_copy(ap2, ap);
    const int len = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);
    int k = len < 0 ? 0 : (len < (int)sizeof line ? len : (int)sizeof line - 1);
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < k && s_head - s_tail < RING; ++i) s_ring[s_head++ % RING] = line[i];
    taskEXIT_CRITICAL(&s_mux);
    return s_uart ? s_uart(fmt, ap) : len;
}

void sdlog_init(void)
{
    char a[96], b[96];
    path(a, sizeof a, "log.txt");
    path(b, sizeof b, "log-old.txt");
    unlink(b);
    rename(a, b);
    s_file = fopen(a, "w");
    if (s_file) {
        fprintf(s_file, "CYD Mac SE %s\n", APP_VERSION);
        fflush(s_file);
    }
    s_uart = esp_log_set_vprintf(log_vprintf);
}

void sdlog_flush(void)
{
    if (!s_file) return;
    static char chunk[1024];
    bool wrote = false;
    for (;;) {
        int k = 0;
        taskENTER_CRITICAL(&s_mux);
        while (k < (int)sizeof chunk && s_tail != s_head) chunk[k++] = s_ring[s_tail++ % RING];
        taskEXIT_CRITICAL(&s_mux);
        if (!k) break;
        fwrite(chunk, 1, k, s_file);
        wrote = true;
    }
    if (wrote) {
        fflush(s_file);
        fsync(fileno(s_file));
    }
}

void sdlog_crash(const char* const* lines, int n)
{
    char a[96];
    path(a, sizeof a, "crash.txt");
    FILE* f = fopen(a, "a");
    if (!f) return;
    fprintf(f, "----\n");
    for (int i = 0; i < n; ++i) fprintf(f, "%s\n", lines[i]);
    fclose(f);
}
