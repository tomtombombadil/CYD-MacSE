// The PC test build's platform services (see cyd_mac.h).
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "cyd_mac.h"

uint8_t g_canvas[CANVAS_H * CANVAS_STRIDE];

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static const char* s_dirs[3] = {NULL, NULL, NULL};

void hal_host_set_dir(const char* d)
{
    s_dirs[0] = d;
}

uint64_t hal_time_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)(t.tv_nsec / 1000);
}

void hal_sleep_ms(int ms)
{
    if (ms > 0) usleep((useconds_t)ms * 1000);
}

void hal_lock(void) { pthread_mutex_lock(&s_lock); }
void hal_unlock(void) { pthread_mutex_unlock(&s_lock); }

const char* const* hal_dirs(void) { return s_dirs; }

int64_t hal_unix_time(void)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return (int64_t)t + lt.tm_gmtoff;
}

void hal_sync_file(void* f) { (void)f; }
void hal_after_disk_io(int w, uint32_t s, uint32_t c, const void* b) { (void)w; (void)s; (void)c; (void)b; }

const char* hal_net_address(void) { return NULL; }
