// Shared between the Mac emulator (Mini vMac glue, osglu_cyd.c), the touch
// UI (ui.c) and the board code (main/ on the CYD, host/ on a PC).
//
// Threads:
//   emulator thread - runs Mini vMac; the only one touching emulator state
//   UI thread       - reads touch, draws the keys / pages, posts events
//   audio thread    - pulls the Mac's sound samples (emu_audio_pull)
//   display         - the panel's refill interrupt reads g_canvas
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- the screen: one 800x480 1-bit picture (1 = black, MSB = leftmost) ----
// In internal RAM; the panel's bounce-buffer refill expands it to RGB565.
#define CANVAS_W 800
#define CANVAS_H 480
#define CANVAS_STRIDE (CANVAS_W / 8)
extern uint8_t g_canvas[CANVAS_H * CANVAS_STRIDE];

// Where the Mac's 512x342 screen sits on it (1:1). MAC_X is a multiple of 8.
#define MAC_W 512
#define MAC_H 342
#define MAC_X 144
#define MAC_Y 0

// ---- platform services (main/hal_esp.c on the CYD, host/hal_host.c) ----
uint64_t hal_time_us(void);
void hal_sleep_ms(int ms);
// Guards g_canvas's Mac area between the emulator's copy and the UI's pages
void hal_lock(void);
void hal_unlock(void);
// One SD card user at a time (the emulator's disks, the UI's settings
// and disk list, the log): recursive. Tom's crashes came 1 s after a disk
// went in - the UI saving macse.txt while the Mac read the disk.
void hal_fs_lock(void);
void hal_fs_unlock(void);
// Folders searched for the ROM and disk images, most specific first,
// NULL-terminated ("/sd/mac", "/sd" on the CYD)
const char* const* hal_dirs(void);
// Seconds since 1970 (UTC; the TZ variable gives local time), or 0 while
// the board doesn't know the time
int64_t hal_unix_time(void);
// The SD card's file system after writes: push what's cached to the card
void hal_sync_file(void* stdio_file);
// The board's address on the network ("" while joining), or NULL when
// it has no Wi-Fi set up
const char* hal_net_address(void);
// After each disk image read / write (the CYD checks its memory there)
void hal_after_disk_io(int is_write, uint32_t start, uint32_t count, const void* mac_buf);

// ---- UI -> emulator events ----
enum {
    EV_MOUSE_POS = 1,   // x, y: Mac screen coordinates
    EV_MOUSE_BTN,       // a: 1 down, 0 up
    EV_KEY,             // a: Mac key code (MKC_*), x: 1 down, 0 up
    EV_INSERT,          // a: slot in g_insert_path
    EV_SPEED,           // a: 0 = 1x, 1 = 2x, 2 = 4x, 3 = 8x, 255 = as fast as it goes
    EV_RESET,           // restart the Mac
    EV_REDRAW,          // copy the whole Mac screen to the canvas again
};
typedef struct {
    uint8_t kind;
    uint8_t a;
    uint16_t x, y;
} emu_event;

bool emu_post(uint8_t kind, uint8_t a, uint16_t x, uint16_t y);
bool emu_take(emu_event* e);   // emulator side

#define INSERT_SLOTS 4
#define PATH_MAX_LEN 256
extern char g_insert_path[INSERT_SLOTS][PATH_MAX_LEN];

// When false the emulator leaves the canvas's Mac area alone (a page is
// shown there). Change it under hal_lock().
extern volatile bool g_mac_blit_enabled;

// ---- emulator -> UI status ----
#define EMU_DRIVES 6
typedef struct {
    volatile int rom;            // 0 starting, 1 loaded, -1 not found, -2 wrong / bad ROM
    volatile uint32_t drives;    // bit n: drive n holds a disk
    char drive_path[EMU_DRIVES][PATH_MAX_LEN];
    volatile uint32_t changes;   // +1 at every insert / eject (the UI saves the list)
    volatile uint32_t ticks;     // emulated 1/60 s ticks
    volatile uint32_t disk_activity;  // +1 per disk read / write
    volatile bool sound_on;      // set by the UI
} emu_status_t;
extern emu_status_t g_emu;

// The emulator (osglu_cyd.c). Returns only if the Mac is switched off.
int emu_main(void);
// Mac sound, 22255 samples/s, 8-bit unsigned (0x80 = silence). Fills n
// samples; returns how many were real (the rest repeat the last one).
int emu_audio_pull(uint8_t* dst, int n);
#define EMU_SOUND_RATE 22255

// ---- the touch UI (ui.c), run on the UI thread ----
// insert_saved_disks: put back the disks macse.txt lists (not after a crash)
void ui_init(bool insert_saved_disks);
// One touch reading: down = finger / stylus on the glass, x / y in pixels
void ui_touch(bool down, int x, int y);
// About 30 times a second: status lights, saving settings
void ui_tick(void);

#ifdef __cplusplus
}
#endif
