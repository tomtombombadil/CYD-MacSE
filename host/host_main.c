// PC test build: runs the emulator and the touch UI with scripted taps.
//   macse_host <folder> [command ...]
// <folder> holds MacSE.ROM and disk images (as the SD card's mac folder).
// Commands, run in order:
//   wait:<ms>          let the Mac run
//   tap:<x>,<y>        a tap on the 800x480 glass (held 80 ms)
//   press:<x>,<y> / move:<x>,<y> / release (wait:ms between: held still)
//   shot:<file.pbm>    save the screen (1-bit PBM)
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyd_mac.h"

void hal_host_set_dir(const char* d);

static void* emu_thread(void* arg)
{
    (void)arg;
    emu_main();
    return NULL;
}

static bool s_touch_down;
static int s_tx, s_ty;

static void run_ui(int ms)
{
    // the UI polls touch about every 10 ms on the board (a touch held
    // down is reported at every poll, as the GT911 does)
    for (int t = 0; t < ms; t += 10) {
        if (s_touch_down) ui_touch(true, s_tx, s_ty);
        ui_tick();
        hal_sleep_ms(10);
    }
}

static void shot(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P4\n%d %d\n", CANVAS_W, CANVAS_H);
    hal_lock();
    fwrite(g_canvas, 1, sizeof g_canvas, f);
    hal_unlock();
    fclose(f);
    printf("saved %s\n", path);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <folder> [wait:ms tap:x,y shot:file.pbm ...]\n", argv[0]);
        return 1;
    }
    hal_host_set_dir(argv[1]);
    ui_init(true);
    pthread_t th;
    // The board's Mac task has 16 KB of stack: the PC gets no more (with
    // room for the C library's own use), so a big stack buffer fails here
    // too (0.1.0-0.1.12: a 32 KB one, fine with Linux's 8 MB stacks)
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16384 + 16384);
    pthread_create(&th, &at, emu_thread, NULL);
    int x = 0, y = 0;
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!strncmp(a, "wait:", 5)) run_ui(atoi(a + 5));
        else if (sscanf(a, "tap:%d,%d", &x, &y) == 2) {
            ui_touch(true, x, y);
            run_ui(80);
            ui_touch(false, x, y);
            run_ui(20);
        } else if (sscanf(a, "press:%d,%d", &x, &y) == 2) {
            ui_touch(true, x, y);
            s_touch_down = true; s_tx = x; s_ty = y;
            run_ui(20);
        } else if (sscanf(a, "move:%d,%d", &x, &y) == 2) {
            // in 10 steps, as a finger slides
            const int x0 = s_tx, y0 = s_ty;
            for (int k = 1; k <= 10; ++k) {
                s_tx = x0 + (x - x0) * k / 10; s_ty = y0 + (y - y0) * k / 10;
                ui_touch(true, s_tx, s_ty);
                run_ui(10);
            }
        } else if (!strcmp(a, "release")) {
            s_touch_down = false;
            ui_touch(false, s_tx, s_ty);
            run_ui(20);
        }
        else if (!strncmp(a, "shot:", 5)) shot(a + 5);
        else fprintf(stderr, "unknown command %s\n", a);
    }
    printf("ticks %u, rom %d, drives %x\n", g_emu.ticks, g_emu.rom, g_emu.drives);
    return 0;
}
