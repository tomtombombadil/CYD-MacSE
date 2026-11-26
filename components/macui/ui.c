// The touch UI around the Mac screen (see cyd_mac.h).
//
// Layout (800x480, right-handed; Left Handed swaps the two side panels):
//   x 0..143    Disks, Drag, Double Click, status, Settings
//   x 144..655  the Mac's 512x342 screen, 1:1, at the top
//   x 656..799  Command, Option, Control, Space, Enter, arrows
//   y 342..479  keyboard, 4 rows
//
// Mouse: a tap on the Mac screen moves the pointer there and clicks
// (press and release follow the stylus). Pressing and moving more than a
// few pixels drags. A tap on the menu bar keeps the button held, so the
// next tap picks a menu item. Drag: the next tap presses and holds, the
// one after moves there and lets go (drag an icon or window by two taps).
// Double Click: the next tap double-clicks.
// Keys: Shift, Command, Option and Control stay down until the next key or
// click; Caps Lock stays down until tapped again.
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "canvas.h"
#include "cyd_mac.h"

// ---- Mac key codes (Mini vMac's MKC_*: the ADB key codes) ----
enum {
    K_A = 0x00, K_S = 0x01, K_D = 0x02, K_F = 0x03, K_H = 0x04, K_G = 0x05, K_Z = 0x06,
    K_X = 0x07, K_C = 0x08, K_V = 0x09, K_B = 0x0B, K_Q = 0x0C, K_W = 0x0D, K_E = 0x0E,
    K_R = 0x0F, K_Y = 0x10, K_T = 0x11, K_1 = 0x12, K_2 = 0x13, K_3 = 0x14, K_4 = 0x15,
    K_6 = 0x16, K_5 = 0x17, K_EQUAL = 0x18, K_9 = 0x19, K_7 = 0x1A, K_MINUS = 0x1B,
    K_8 = 0x1C, K_0 = 0x1D, K_RBRACKET = 0x1E, K_O = 0x1F, K_U = 0x20, K_LBRACKET = 0x21,
    K_I = 0x22, K_P = 0x23, K_RETURN = 0x24, K_L = 0x25, K_J = 0x26, K_QUOTE = 0x27,
    K_K = 0x28, K_SEMI = 0x29, K_BACKSLASH = 0x2A, K_COMMA = 0x2B, K_SLASH = 0x2C,
    K_N = 0x2D, K_M = 0x2E, K_PERIOD = 0x2F, K_TAB = 0x30, K_SPACE = 0x31, K_GRAVE = 0x32,
    K_DELETE = 0x33, K_ESCAPE = 0x35, K_COMMAND = 0x37, K_SHIFT = 0x38, K_CAPS = 0x39,
    K_OPTION = 0x3A, K_CONTROL = 0x3B, K_ENTER = 0x4C, K_LEFT = 0x7B, K_RIGHT = 0x7C,
    K_DOWN = 0x7D, K_UP = 0x7E,
};

// ---- keys on the glass ----
enum Kind { KIND_MAC, KIND_MOD, KIND_CAPS, KIND_DISKS, KIND_DRAG, KIND_DOUBLE, KIND_SETTINGS };

typedef struct {
    int16_t x, y, w, h;
    uint8_t kind;
    uint8_t code;       // Mac key code (KIND_MAC / MOD / CAPS)
    const char* label;
    const char* shifted;  // label while Shift is down (NULL: same)
} Key;

#define MAX_KEYS 90
static Key s_keys[MAX_KEYS];
static int s_nkeys;

// ---- state ----
typedef enum { PAGE_NONE, PAGE_DISKS, PAGE_SETTINGS } Page;

static struct {
    bool left_handed;
    bool sound;
    int speed;          // index into kSpeeds
} s_set = {false, true, 0};

static const struct { const char* label; uint8_t value; } kSpeeds[] = {
    {"1x", 0}, {"2x", 1}, {"4x", 2}, {"Max", 255},
};
#define N_SPEEDS 4

static Page s_page = PAGE_NONE;
static bool s_shift, s_command, s_option, s_control, s_caps;
static bool s_drag_armed, s_double_armed;
// The Mac's mouse button held between taps (menu bar tap, Drag)
enum { HOLD_NONE, HOLD_START, HOLD_WAIT, HOLD_MOVE };
static int s_hold;            // HOLD_*
static bool s_hold_drag;      // started by Drag (lights the key)

// the touch in progress
static bool s_down;
static int s_press_key = -1;  // key pressed (index), -1 none
static bool s_press_mac;      // pressing on the Mac screen
static bool s_press_page;
static int s_px, s_py;        // where it went down
static bool s_moving;         // left the dead zone: the pointer follows
static int s_mx, s_my;        // last pointer position sent

static uint32_t s_seen_changes;
static bool s_dirty_settings;
static uint64_t s_dirty_since;
static char s_status[2][40];
static uint32_t s_seen_activity;
static uint64_t s_activity_until;
static bool s_light_on;

// ---- the disk list (Disks page) ----
#define MAX_FILES 200
typedef struct {
    char path[PATH_MAX_LEN];
    uint32_t size;
} DiskFile;
static DiskFile* s_files;
static int s_nfiles, s_first;
#define LIST_ROWS 7

// ---------------------------------------------------------------- helpers

static int side_keys_x(void) { return s_set.left_handed ? 0 : MAC_X + MAC_W; }
static int side_tools_x(void) { return s_set.left_handed ? MAC_X + MAC_W : 0; }

static void add_key(int x, int y, int w, int h, int kind, int code, const char* label, const char* shifted)
{
    if (s_nkeys >= MAX_KEYS) return;
    Key* k = &s_keys[s_nkeys++];
    k->x = (int16_t)x;
    k->y = (int16_t)y;
    k->w = (int16_t)w;
    k->h = (int16_t)h;
    k->kind = (uint8_t)kind;
    k->code = (uint8_t)code;
    k->label = label;
    k->shifted = shifted;
}

typedef struct {
    float units;
    int kind, code;
    const char *label, *shifted;
} RowKey;

static void add_row(int row, const RowKey* r, int n)
{
    const int y = MAC_H + 3 + row * 34;
    float u = 0;
    for (int i = 0; i < n; ++i) {
        const int x0 = (int)(u * CANVAS_W / 15.0f + 0.5f);
        u += r[i].units;
        const int x1 = (int)(u * CANVAS_W / 15.0f + 0.5f);
        add_key(x0 + 1, y, x1 - x0 - 2, 32, r[i].kind, r[i].code, r[i].label, r[i].shifted);
    }
}

#define MK(u, c, l, s) {u, KIND_MAC, c, l, s}

static void build_keys(void)
{
    s_nkeys = 0;
    static const RowKey r0[] = {
        MK(1, K_ESCAPE, "Esc", NULL), MK(1, K_1, "1", "!"), MK(1, K_2, "2", "@"), MK(1, K_3, "3", "#"),
        MK(1, K_4, "4", "$"), MK(1, K_5, "5", "%"), MK(1, K_6, "6", "^"), MK(1, K_7, "7", "&"),
        MK(1, K_8, "8", "*"), MK(1, K_9, "9", "("), MK(1, K_0, "0", ")"), MK(1, K_MINUS, "-", "_"),
        MK(1, K_EQUAL, "=", "+"), MK(2, K_DELETE, "Delete", NULL),
    };
    static const RowKey r1[] = {
        MK(1.5f, K_TAB, "Tab", NULL), MK(1, K_Q, "Q", NULL), MK(1, K_W, "W", NULL), MK(1, K_E, "E", NULL),
        MK(1, K_R, "R", NULL), MK(1, K_T, "T", NULL), MK(1, K_Y, "Y", NULL), MK(1, K_U, "U", NULL),
        MK(1, K_I, "I", NULL), MK(1, K_O, "O", NULL), MK(1, K_P, "P", NULL), MK(1, K_LBRACKET, "[", "{"),
        MK(1, K_RBRACKET, "]", "}"), MK(1.5f, K_BACKSLASH, "\\", "|"),
    };
    static const RowKey r2[] = {
        {1.75f, KIND_CAPS, K_CAPS, "Caps Lock", NULL}, MK(1, K_A, "A", NULL), MK(1, K_S, "S", NULL),
        MK(1, K_D, "D", NULL), MK(1, K_F, "F", NULL), MK(1, K_G, "G", NULL), MK(1, K_H, "H", NULL),
        MK(1, K_J, "J", NULL), MK(1, K_K, "K", NULL), MK(1, K_L, "L", NULL), MK(1, K_SEMI, ";", ":"),
        MK(1, K_QUOTE, "'", "\""), MK(2.25f, K_RETURN, "Return", NULL),
    };
    static const RowKey r3[] = {
        {2.25f, KIND_MOD, K_SHIFT, "Shift", NULL}, MK(1, K_Z, "Z", NULL), MK(1, K_X, "X", NULL),
        MK(1, K_C, "C", NULL), MK(1, K_V, "V", NULL), MK(1, K_B, "B", NULL), MK(1, K_N, "N", NULL),
        MK(1, K_M, "M", NULL), MK(1, K_COMMA, ",", "<"), MK(1, K_PERIOD, ".", ">"),
        MK(1, K_SLASH, "/", "?"), MK(1, K_GRAVE, "`", "~"), {1.75f, KIND_MOD, K_SHIFT, "Shift", NULL},
    };
    add_row(0, r0, sizeof r0 / sizeof r0[0]);
    add_row(1, r1, sizeof r1 / sizeof r1[0]);
    add_row(2, r2, sizeof r2 / sizeof r2[0]);
    add_row(3, r3, sizeof r3 / sizeof r3[0]);

    // the side with the Mac's other keys: 6 rows of 57 px
    const int kx = side_keys_x() + 4, kw = 136, half = 66, third = 44;
#define ROWY(i) (3 + (i) * 57)
    add_key(kx, ROWY(0), kw, 51, KIND_MOD, K_COMMAND, "\xE2\x8C\x98 Command", NULL);
    add_key(kx, ROWY(1), half, 51, KIND_MOD, K_OPTION, "Option", NULL);
    add_key(kx + half + 4, ROWY(1), half, 51, KIND_MOD, K_CONTROL, "Control", NULL);
    add_key(kx, ROWY(2), kw, 51, KIND_MAC, K_SPACE, "Space", NULL);
    add_key(kx, ROWY(3), kw, 51, KIND_MAC, K_ENTER, "Enter", NULL);
    add_key(kx + third + 2, ROWY(4), third, 51, KIND_MAC, K_UP, "\xE2\x86\x91", NULL);
    add_key(kx, ROWY(5), third, 51, KIND_MAC, K_LEFT, "\xE2\x86\x90", NULL);
    add_key(kx + third + 2, ROWY(5), third, 51, KIND_MAC, K_DOWN, "\xE2\x86\x93", NULL);
    add_key(kx + 2 * (third + 2), ROWY(5), third, 51, KIND_MAC, K_RIGHT, "\xE2\x86\x92", NULL);

    // the tools side
    const int tx = side_tools_x() + 4;
    add_key(tx, ROWY(0), kw, 51, KIND_DISKS, 0, "Disks", NULL);
    add_key(tx, ROWY(1), kw, 51, KIND_DRAG, 0, "Drag", NULL);
    add_key(tx, ROWY(2), kw, 51, KIND_DOUBLE, 0, "Double Click", NULL);
    add_key(tx, ROWY(5), kw, 51, KIND_SETTINGS, 0, "Settings", NULL);
}

static bool key_lit(const Key* k)
{
    switch (k->kind) {
        case KIND_MOD:
            return (k->code == K_SHIFT && s_shift) || (k->code == K_COMMAND && s_command)
                || (k->code == K_OPTION && s_option) || (k->code == K_CONTROL && s_control);
        case KIND_CAPS: return s_caps;
        case KIND_DISKS: return s_page == PAGE_DISKS;
        case KIND_SETTINGS: return s_page == PAGE_SETTINGS;
        case KIND_DRAG: return s_drag_armed || (s_hold != HOLD_NONE && s_hold_drag);
        case KIND_DOUBLE: return s_double_armed;
        default: return false;
    }
}

static void draw_key(int i)
{
    const Key* k = &s_keys[i];
    const bool lit = key_lit(k) || (i == s_press_key);
    const char* label = (s_shift && k->shifted) ? k->shifted : k->label;
    cv_key(k->x, k->y, k->w, k->h, lit);
    cv_text_center(k->x, k->y, k->w, k->h, label, lit ? CV_WHITE : CV_BLACK);
}

static void draw_status(void)
{
    const int x = side_tools_x() + 4;
    const int y = ROWY(3);
    cv_fill(x, y, 136, 108, CV_WHITE);
    cv_text_center(x, y + 4, 136, 18, s_status[0], CV_BLACK);
    cv_text_center(x, y + 24, 136, 18, s_status[1], CV_BLACK);
    // disk light
    const int lx = x + 4, ly = y + 52;
    cv_frame(lx, ly, 14, 10, CV_BLACK);
    if (s_light_on) cv_fill(lx + 2, ly + 2, 10, 6, CV_BLACK);
    cv_text(lx + 20, ly - 3, "Disk", CV_BLACK);
}

static void set_status(const char* a, const char* b)
{
    if (strcmp(a, s_status[0]) == 0 && strcmp(b, s_status[1]) == 0) return;
    snprintf(s_status[0], sizeof s_status[0], "%s", a);
    snprintf(s_status[1], sizeof s_status[1], "%s", b);
    draw_status();
}

static void update_status(void)
{
    if (g_emu.rom == -1) set_status("No ROM found", "See the Mac screen");
    else if (g_emu.rom == -2) set_status("ROM not usable", "See the Mac screen");
    else if (s_hold != HOLD_NONE) set_status("Button Held", "Tap to let go");
    else if (s_drag_armed) set_status("Drag:", "Tap the start");
    else if (s_double_armed) set_status("Double Click:", "Tap the place");
    else if (g_emu.drives == 0) set_status("No disk in", "Tap Disks");
    else set_status("", "");
}

static void draw_all_keys(void)
{
    for (int i = 0; i < s_nkeys; ++i) draw_key(i);
}

static void redraw_keys_of(int kind, int code)
{
    for (int i = 0; i < s_nkeys; ++i)
        if (s_keys[i].kind == kind && (code < 0 || s_keys[i].code == code)) draw_key(i);
}

static void draw_frame(void)
{
    // everything but the Mac screen
    cv_fill(0, 0, MAC_X, MAC_H, CV_WHITE);
    cv_fill(MAC_X + MAC_W, 0, CANVAS_W - MAC_X - MAC_W, MAC_H, CV_WHITE);
    cv_fill(0, MAC_H, CANVAS_W, CANVAS_H - MAC_H, CV_WHITE);
    cv_fill(MAC_X - 1, 0, 1, MAC_H, CV_BLACK);
    cv_fill(MAC_X + MAC_W, 0, 1, MAC_H, CV_BLACK);
    cv_fill(0, MAC_H, CANVAS_W, 1, CV_BLACK);
    draw_all_keys();
    s_status[0][0] = s_status[1][0] = '\1';   // force a redraw
    update_status();
}

// ---------------------------------------------------------------- settings file

static const char* settings_path(void)
{
    static char p[PATH_MAX_LEN];
    const char* const* d = hal_dirs();
    snprintf(p, sizeof p, "%s/macse.txt", d[0] ? d[0] : ".");
    return p;
}

static void save_settings(void)
{
    FILE* f = fopen(settings_path(), "w");
    if (!f) return;
    fprintf(f, "# Mac SE emulator settings (written by the emulator)\n");
    fprintf(f, "left_handed=%d\nsound=%d\nspeed=%s\n", s_set.left_handed, s_set.sound, kSpeeds[s_set.speed].label);
    for (int i = 0; i < EMU_DRIVES; ++i)
        if (g_emu.drives & (1u << i)) fprintf(f, "disk=%s\n", g_emu.drive_path[i]);
    fclose(f);
}

static int s_next_slot;

static void insert_disk(const char* path)
{
    const int slot = s_next_slot;
    s_next_slot = (s_next_slot + 1) % INSERT_SLOTS;
    snprintf(g_insert_path[slot], PATH_MAX_LEN, "%s", path);
    emu_post(EV_INSERT, (uint8_t)slot, 0, 0);
}

static void load_settings(bool insert_saved_disks)
{
    FILE* f = fopen(settings_path(), "r");
    if (!f) return;
    char line[PATH_MAX_LEN + 16];
    int disks = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        const char* v = eq + 1;
        if (!strcmp(line, "left_handed")) s_set.left_handed = atoi(v) != 0;
        else if (!strcmp(line, "sound")) s_set.sound = atoi(v) != 0;
        else if (!strcmp(line, "speed")) {
            for (int i = 0; i < N_SPEEDS; ++i)
                if (!strcasecmp(v, kSpeeds[i].label)) s_set.speed = i;
        } else if (!strcmp(line, "disk") && insert_saved_disks && disks < INSERT_SLOTS) {
            insert_disk(v);   // the Mac picks them up as it starts
            ++disks;
        }
    }
    fclose(f);
}

static void settings_changed(void)
{
    s_dirty_settings = true;
    s_dirty_since = hal_time_us();
}

// ---------------------------------------------------------------- pages

static void mac_area_clear(void) { cv_fill(MAC_X, MAC_Y, MAC_W, MAC_H, CV_WHITE); }

static void page_title(const char* t)
{
    cv_fill(MAC_X, MAC_Y, MAC_W, 24, CV_BLACK);
    cv_text_center(MAC_X, MAC_Y + 2, MAC_W, 20, t, CV_WHITE);
}

// Page buttons: a small table so taps find them
typedef struct {
    int16_t x, y, w, h;
    int id;
} PageBtn;
static PageBtn s_btns[24];
static int s_nbtns;

static void page_btn(int x, int y, int w, int h, int id, const char* label, bool lit)
{
    if (s_nbtns < (int)(sizeof s_btns / sizeof s_btns[0])) {
        PageBtn* b = &s_btns[s_nbtns++];
        b->x = (int16_t)x;
        b->y = (int16_t)y;
        b->w = (int16_t)w;
        b->h = (int16_t)h;
        b->id = id;
    }
    cv_key(x, y, w, h, lit);
    cv_text_center(x, y, w, h, label, lit ? CV_WHITE : CV_BLACK);
}

enum { B_BACK = 1, B_PREV, B_NEXT, B_LEFT, B_SOUND, B_RESTART, B_SPEED0 = 10, B_FILE0 = 20 };

static void format_size(uint32_t n, char* out, size_t cap)
{
    if (n < 1024 * 1024) snprintf(out, cap, "%uK", (unsigned)(n / 1024));
    else if (n < 10u * 1024 * 1024) snprintf(out, cap, "%u.%uM", (unsigned)(n >> 20), (unsigned)(((n & 0xFFFFF) * 10) >> 20));
    else snprintf(out, cap, "%uM", (unsigned)(n >> 20));
}

static bool in_drive(const char* path)
{
    for (int i = 0; i < EMU_DRIVES; ++i)
        if ((g_emu.drives & (1u << i)) && !strcmp(g_emu.drive_path[i], path)) return true;
    return false;
}

static void draw_disks_page(void)
{
    s_nbtns = 0;
    mac_area_clear();
    page_title("Disks: tap one to put it in the Mac");
    const int x = MAC_X + 8, w = MAC_W - 16;
    if (s_nfiles == 0) {
        cv_text_center(MAC_X, MAC_Y + 60, MAC_W, 20, "No disk images found.", CV_BLACK);
        cv_text_center(MAC_X, MAC_Y + 84, MAC_W, 20, "Put .dsk / .img / .hda files in a", CV_BLACK);
        cv_text_center(MAC_X, MAC_Y + 104, MAC_W, 20, "folder named mac on the SD card.", CV_BLACK);
    }
    for (int r = 0; r < LIST_ROWS && s_first + r < s_nfiles; ++r) {
        const DiskFile* d = &s_files[s_first + r];
        const int y = MAC_Y + 30 + r * 38;
        const bool in = in_drive(d->path);
        PageBtn* b = &s_btns[s_nbtns++];
        b->x = (int16_t)x;
        b->y = (int16_t)y;
        b->w = (int16_t)w;
        b->h = 34;
        b->id = B_FILE0 + s_first + r;
        cv_key(x, y, w, 34, in);
        const int c = in ? CV_WHITE : CV_BLACK;
        const char* name = strrchr(d->path, '/');
        name = name ? name + 1 : d->path;
        char size[16];
        format_size(d->size, size, sizeof size);
        cv_text_fit(x + 10, y + 9, w - 160, name, c);
        cv_text(x + w - 140, y + 9, size, c);
        if (in) cv_text(x + w - 78, y + 9, "In Drive", c);
    }
    const int by = MAC_Y + MAC_H - 44;
    if (s_first > 0) page_btn(MAC_X + 8, by, 120, 38, B_PREV, "Previous", false);
    if (s_first + LIST_ROWS < s_nfiles) page_btn(MAC_X + 136, by, 120, 38, B_NEXT, "Next", false);
    page_btn(MAC_X + MAC_W - 128, by, 120, 38, B_BACK, "Back", false);
}

static void draw_settings_page(void)
{
    s_nbtns = 0;
    mac_area_clear();
    page_title("Settings");
    const int x = MAC_X + 16;
    int y = MAC_Y + 40;
    cv_text(x, y + 11, "Left Handed", CV_BLACK);
    page_btn(x + 200, y, 90, 38, B_LEFT, s_set.left_handed ? "On" : "Off", s_set.left_handed);
    y += 50;
    cv_text(x, y + 11, "Sound", CV_BLACK);
    page_btn(x + 200, y, 90, 38, B_SOUND, s_set.sound ? "On" : "Off", s_set.sound);
    y += 50;
    cv_text(x, y + 11, "Speed", CV_BLACK);
    for (int i = 0; i < N_SPEEDS; ++i)
        page_btn(x + 200 + i * 72, y, 66, 38, B_SPEED0 + i, kSpeeds[i].label, i == s_set.speed);
    y += 50;
    cv_text(x, y + 11, "Restart the Mac", CV_BLACK);
    page_btn(x + 200, y, 90, 38, B_RESTART, "Restart", false);
    y += 56;
    cv_text(x, y, "ROM: MacSE.ROM, disks: the mac folder on the SD card.", CV_BLACK);
    {
        char line[64];
        const char* a = hal_net_address();
        if (a && a[0]) snprintf(line, sizeof line, "Log page: http://%s/", a);
        else snprintf(line, sizeof line, "Log page: %s", a ? "joining Wi-Fi..." : "no wifi.txt on the card");
        cv_text(x, y + 22, line, CV_BLACK);
    }
    page_btn(MAC_X + MAC_W - 128, MAC_Y + MAC_H - 44, 120, 38, B_BACK, "Back", false);
}

static int cmp_files(const void* a, const void* b)
{
    const DiskFile* x = (const DiskFile*)a;
    const DiskFile* y = (const DiskFile*)b;
    const char* nx = strrchr(x->path, '/');
    const char* ny = strrchr(y->path, '/');
    return strcasecmp(nx ? nx + 1 : x->path, ny ? ny + 1 : y->path);
}

static bool skip_name(const char* n)
{
    if (n[0] == '.') return true;
    const char* dot = strrchr(n, '.');
    if (dot && (!strcasecmp(dot, ".rom") || !strcasecmp(dot, ".txt"))) return true;
    return false;
}

static void scan_disks(void)
{
    if (!s_files) s_files = (DiskFile*)calloc(MAX_FILES, sizeof(DiskFile));
    s_nfiles = 0;
    s_first = 0;
    if (!s_files) return;
    for (const char* const* d = hal_dirs(); *d; ++d) {
        DIR* dir = opendir(*d);
        if (!dir) continue;
        struct dirent* e;
        while ((e = readdir(dir)) && s_nfiles < MAX_FILES) {
            if (skip_name(e->d_name)) continue;
            DiskFile* f = &s_files[s_nfiles];
            if (snprintf(f->path, PATH_MAX_LEN, "%s/%s", *d, e->d_name) >= PATH_MAX_LEN) continue;
            struct stat st;
            if (stat(f->path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            if (st.st_size < 400 * 1024) continue;   // smaller than any Mac disk
            f->size = (uint32_t)st.st_size;
            ++s_nfiles;
        }
        closedir(dir);
    }
    qsort(s_files, (size_t)s_nfiles, sizeof(DiskFile), cmp_files);
}

static void open_page(Page p)
{
    hal_lock();
    g_mac_blit_enabled = false;
    s_page = p;
    if (p == PAGE_DISKS) draw_disks_page();
    else draw_settings_page();
    hal_unlock();
    redraw_keys_of(KIND_DISKS, -1);
    redraw_keys_of(KIND_SETTINGS, -1);
}

static void close_page(void)
{
    hal_lock();
    s_page = PAGE_NONE;
    g_mac_blit_enabled = true;
    hal_unlock();
    emu_post(EV_REDRAW, 0, 0, 0);
    redraw_keys_of(KIND_DISKS, -1);
    redraw_keys_of(KIND_SETTINGS, -1);
}

static void page_tap(int x, int y)
{
    int id = 0;
    for (int i = 0; i < s_nbtns; ++i) {
        const PageBtn* b = &s_btns[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) id = b->id;
    }
    if (!id) return;
    if (s_page == PAGE_DISKS) {
        if (id == B_BACK) close_page();
        else if (id == B_PREV) { s_first -= LIST_ROWS; if (s_first < 0) s_first = 0; hal_lock(); draw_disks_page(); hal_unlock(); }
        else if (id == B_NEXT) { s_first += LIST_ROWS; hal_lock(); draw_disks_page(); hal_unlock(); }
        else if (id >= B_FILE0 && id - B_FILE0 < s_nfiles) {
            const DiskFile* d = &s_files[id - B_FILE0];
            if (!in_drive(d->path)) insert_disk(d->path);
            close_page();
        }
        return;
    }
    // settings
    if (id == B_BACK) { close_page(); return; }
    if (id == B_LEFT) {
        s_set.left_handed = !s_set.left_handed;
        build_keys();
        draw_frame();
    } else if (id == B_SOUND) {
        s_set.sound = !s_set.sound;
        g_emu.sound_on = s_set.sound;
    } else if (id >= B_SPEED0 && id < B_SPEED0 + N_SPEEDS) {
        s_set.speed = id - B_SPEED0;
        emu_post(EV_SPEED, kSpeeds[s_set.speed].value, 0, 0);
    } else if (id == B_RESTART) {
        emu_post(EV_RESET, 0, 0, 0);
        close_page();
        settings_changed();
        return;
    }
    settings_changed();
    hal_lock();
    draw_settings_page();
    hal_unlock();
}

// ---------------------------------------------------------------- Mac input

static void send_key(int code, bool down) { emu_post(EV_KEY, (uint8_t)code, down ? 1 : 0, 0); }

static void set_mod(int code, bool on)
{
    bool* f = code == K_SHIFT ? &s_shift : code == K_COMMAND ? &s_command : code == K_OPTION ? &s_option : &s_control;
    if (*f == on) return;
    *f = on;
    send_key(code, on);
    redraw_keys_of(KIND_MOD, code);
    if (code == K_SHIFT) draw_all_keys();   // labels change
}

// After a key or a click: the latched modifiers go up
static void release_mods(void)
{
    set_mod(K_SHIFT, false);
    set_mod(K_COMMAND, false);
    set_mod(K_OPTION, false);
    set_mod(K_CONTROL, false);
}

static void mouse_to(int x, int y)
{
    int mx = x - MAC_X, my = y - MAC_Y;
    if (mx < 0) mx = 0;
    if (mx > MAC_W - 1) mx = MAC_W - 1;
    if (my < 0) my = 0;
    if (my > MAC_H - 1) my = MAC_H - 1;
    if (mx == s_mx && my == s_my) return;
    s_mx = mx;
    s_my = my;
    emu_post(EV_MOUSE_POS, 0, (uint16_t)mx, (uint16_t)my);
}

static void button(bool down) { emu_post(EV_MOUSE_BTN, down ? 1 : 0, 0, 0); }

static void end_hold(void)
{
    if (s_hold == HOLD_NONE) return;
    s_hold = HOLD_NONE;
    button(false);
    release_mods();
    redraw_keys_of(KIND_DRAG, -1);
}

static void mac_press(int x, int y)
{
    s_press_mac = true;
    s_moving = false;
    mouse_to(x, y);
    if (s_hold == HOLD_WAIT) {
        s_hold = HOLD_MOVE;   // the pointer went there with the button held
        return;
    }
    if (s_double_armed) {
        s_double_armed = false;
        redraw_keys_of(KIND_DOUBLE, -1);
        button(true);
        hal_sleep_ms(40);
        button(false);
        hal_sleep_ms(60);
        button(true);   // the second click lets go with the stylus
        return;
    }
    button(true);
    if (s_drag_armed) {
        s_drag_armed = false;
        s_hold = HOLD_START;
        s_hold_drag = true;
        redraw_keys_of(KIND_DRAG, -1);
    } else if (y - MAC_Y < 20) {
        // the menu bar: keep the menu open for the next tap
        s_hold = HOLD_START;
        s_hold_drag = false;
    }
}

static void mac_release(void)
{
    s_press_mac = false;
    if (s_hold == HOLD_START) {
        // pulled down a menu by dragging: that was a whole menu choice
        if (s_moving && !s_hold_drag) end_hold();
        else s_hold = HOLD_WAIT;
        return;
    }
    if (s_hold == HOLD_MOVE) {
        end_hold();
        return;
    }
    button(false);
    release_mods();
}

static void key_press(int i)
{
    const Key* k = &s_keys[i];
    if (s_hold != HOLD_NONE && k->kind != KIND_DRAG) end_hold();
    s_press_key = i;
    if (k->kind == KIND_MAC) send_key(k->code, true);
    draw_key(i);
}

static void key_release(int i)
{
    const Key* k = &s_keys[i];
    s_press_key = -1;
    switch (k->kind) {
        case KIND_MAC:
            send_key(k->code, false);
            release_mods();
            break;
        case KIND_MOD: {
            const bool on = k->code == K_SHIFT ? s_shift : k->code == K_COMMAND ? s_command
                          : k->code == K_OPTION ? s_option : s_control;
            set_mod(k->code, !on);
            break;
        }
        case KIND_CAPS:
            s_caps = !s_caps;
            send_key(K_CAPS, s_caps);
            break;
        case KIND_DISKS:
            if (s_page == PAGE_DISKS) close_page();
            else { scan_disks(); open_page(PAGE_DISKS); }
            break;
        case KIND_SETTINGS:
            if (s_page == PAGE_SETTINGS) close_page();
            else open_page(PAGE_SETTINGS);
            break;
        case KIND_DRAG:
            if (s_hold != HOLD_NONE) end_hold();
            else s_drag_armed = !s_drag_armed;
            s_double_armed = false;
            redraw_keys_of(KIND_DOUBLE, -1);
            break;
        case KIND_DOUBLE:
            s_double_armed = !s_double_armed;
            s_drag_armed = false;
            redraw_keys_of(KIND_DRAG, -1);
            break;
    }
    draw_key(i);
    update_status();
}

static int key_at(int x, int y)
{
    for (int i = 0; i < s_nkeys; ++i) {
        const Key* k = &s_keys[i];
        // a little slack around each key (the gaps belong to a key too)
        if (x >= k->x - 1 && x < k->x + k->w + 1 && y >= k->y - 1 && y < k->y + k->h + 2) return i;
    }
    return -1;
}

static bool on_mac(int x, int y)
{
    return x >= MAC_X && x < MAC_X + MAC_W && y >= MAC_Y && y < MAC_Y + MAC_H;
}

// ---------------------------------------------------------------- entry points

void ui_init(bool insert_saved_disks)
{
    s_mx = s_my = -1;
    load_settings(insert_saved_disks);
    g_emu.sound_on = s_set.sound;
    if (s_set.speed) emu_post(EV_SPEED, kSpeeds[s_set.speed].value, 0, 0);
    build_keys();
    hal_lock();
    draw_frame();
    hal_unlock();
    s_seen_changes = g_emu.changes;
}

void ui_touch(bool down, int x, int y)
{
    if (down && !s_down) {
        s_down = true;
        s_px = x;
        s_py = y;
        s_press_key = -1;
        s_press_page = false;
        if (on_mac(x, y)) {
            if (s_page != PAGE_NONE) s_press_page = true;
            else mac_press(x, y);
        } else {
            const int k = key_at(x, y);
            if (k >= 0) key_press(k);
        }
        update_status();
    } else if (down && s_down) {
        if (s_press_mac) {
            // a firm stylus wobbles: the pointer stays put until the
            // touch has clearly moved, then it follows
            if (!s_moving && (abs(x - s_px) > 6 || abs(y - s_py) > 6)) s_moving = true;
            if (s_moving) mouse_to(x, y);
        }
    } else if (!down && s_down) {
        s_down = false;
        if (s_press_page) page_tap(s_px, s_py);
        else if (s_press_mac) mac_release();
        else if (s_press_key >= 0) key_release(s_press_key);
        update_status();
    }
}

void ui_tick(void)
{
    const uint64_t now = hal_time_us();
    if (g_emu.changes != s_seen_changes) {
        s_seen_changes = g_emu.changes;
        settings_changed();
        if (s_page == PAGE_DISKS) {
            hal_lock();
            draw_disks_page();
            hal_unlock();
        }
    }
    if (s_dirty_settings && now - s_dirty_since > 1000000) {
        s_dirty_settings = false;
        save_settings();
    }
    if (g_emu.disk_activity != s_seen_activity) {
        s_seen_activity = g_emu.disk_activity;
        s_activity_until = now + 150000;
    }
    const bool light = now < s_activity_until;
    if (light != s_light_on) {
        s_light_on = light;
        draw_status();
    }
    update_status();
}
