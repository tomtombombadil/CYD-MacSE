// Drawing on the 1-bit canvas (see canvas.h).
#include "canvas.h"

#include <string.h>

#include "cyd_mac.h"
#include "font.h"

static inline void put(int x, int y, int color)
{
    if ((unsigned)x >= CANVAS_W || (unsigned)y >= CANVAS_H) return;
    uint8_t* p = &g_canvas[y * CANVAS_STRIDE + (x >> 3)];
    const uint8_t m = (uint8_t)(0x80 >> (x & 7));
    if (color == CV_BLACK) *p |= m;
    else if (color == CV_WHITE) *p &= (uint8_t)~m;
    else *p ^= m;
}

void cv_fill(int x, int y, int w, int h, int color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > CANVAS_W) w = CANVAS_W - x;
    if (y + h > CANVAS_H) h = CANVAS_H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; ++yy) {
        int xx = x;
        const int end = x + w;
        uint8_t* row = &g_canvas[yy * CANVAS_STRIDE];
        while (xx < end && (xx & 7)) put(xx++, yy, color);
        while (xx + 8 <= end) {
            uint8_t* p = &row[xx >> 3];
            if (color == CV_BLACK) *p = 0xFF;
            else if (color == CV_WHITE) *p = 0;
            else *p = (uint8_t)~*p;
            xx += 8;
        }
        while (xx < end) put(xx++, yy, color);
    }
}

void cv_frame(int x, int y, int w, int h, int color)
{
    cv_fill(x, y, w, 1, color);
    cv_fill(x, y + h - 1, w, 1, color);
    cv_fill(x, y + 1, 1, h - 2, color);
    cv_fill(x + w - 1, y + 1, 1, h - 2, color);
}

void cv_key(int x, int y, int w, int h, int filled)
{
    // corner shape: rows 0..3 start this many pixels in
    static const int kIn[4] = {3, 1, 1, 0};
    cv_fill(x, y, w, h, CV_WHITE);
    if (filled) {
        for (int r = 0; r < 4; ++r) {
            cv_fill(x + kIn[r], y + r, w - 2 * kIn[r], 1, CV_BLACK);
            cv_fill(x + kIn[r], y + h - 1 - r, w - 2 * kIn[r], 1, CV_BLACK);
        }
        cv_fill(x, y + 4, w, h - 8, CV_BLACK);
        return;
    }
    cv_fill(x + 4, y, w - 8, 1, CV_BLACK);
    cv_fill(x + 4, y + h - 1, w - 8, 1, CV_BLACK);
    cv_fill(x, y + 4, 1, h - 8, CV_BLACK);
    cv_fill(x + w - 1, y + 4, 1, h - 8, CV_BLACK);
    // the rounded corners
    const int pts[][2] = {{3, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 3}};
    for (unsigned i = 0; i < sizeof pts / sizeof pts[0]; ++i) {
        const int dx = pts[i][0], dy = pts[i][1];
        put(x + dx, y + dy, CV_BLACK);
        put(x + w - 1 - dx, y + dy, CV_BLACK);
        put(x + dx, y + h - 1 - dy, CV_BLACK);
        put(x + w - 1 - dx, y + h - 1 - dy, CV_BLACK);
    }
}

// Next glyph index from UTF-8 (unknown characters: '?')
static int next_glyph(const char** ps)
{
    const unsigned char* s = (const unsigned char*)*ps;
    unsigned c = *s++;
    if (c >= 0x80) {
        unsigned cp = 0;
        int more = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; more = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; more = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; more = 3; }
        while (more-- > 0 && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
        *ps = (const char*)s;
        switch (cp) {
            case 0x2190: return 95;   // left
            case 0x2191: return 96;   // up
            case 0x2192: return 97;   // right
            case 0x2193: return 98;   // down
            case 0x2318: return 99;   // command
            case 0x2325: return 100;  // option
            case 0x2026: return -2;   // ... (drawn as three dots)
            default: return '?' - FONT_FIRST;
        }
    }
    *ps = (const char*)s;
    if (c < FONT_FIRST || c >= FONT_FIRST + 95) return '?' - FONT_FIRST;
    return (int)(c - FONT_FIRST);
}

static int glyph_w(int g)
{
    if (g == -2) return 3 * kFontW['.' - FONT_FIRST] + 2;
    return kFontW[g];
}

static int draw_glyph(int x, int y, int g, int color)
{
    if (g == -2) {
        const int d = '.' - FONT_FIRST;
        int w = 0;
        for (int i = 0; i < 3; ++i) w += draw_glyph(x + w, y, d, color) + 1;
        return w - 1;
    }
    const int w = kFontW[g];
    for (int r = 0; r < FONT_H; ++r) {
        const uint16_t bits = kFontBits[g][r];
        if (!bits) continue;
        for (int c = 0; c < w; ++c)
            if (bits & (0x8000 >> c)) put(x + c, y + r, color);
    }
    return w;
}

int cv_text_width(const char* s)
{
    int w = 0, n = 0;
    while (*s) {
        w += glyph_w(next_glyph(&s));
        ++n;
    }
    return n ? w + (n - 1) : 0;
}

int cv_text(int x, int y, const char* s, int color)
{
    const int x0 = x;
    int first = 1;
    while (*s) {
        if (!first) x += 1;
        first = 0;
        x += draw_glyph(x, y, next_glyph(&s), color);
    }
    return x - x0;
}

// s cut to fit w pixels, with "..." added if cut
static void fit(const char* s, int w, char* out, int cap)
{
    if (cv_text_width(s) <= w) {
        strncpy(out, s, (size_t)cap - 1);
        out[cap - 1] = 0;
        return;
    }
    const int dots = cv_text_width("\xE2\x80\xA6") + 1;
    int n = 0, used = 0;
    const char* p = s;
    while (*p && n < cap - 5) {
        const char* q = p;
        const int gw = glyph_w(next_glyph(&q)) + 1;
        if (used + gw + dots > w) break;
        used += gw;
        while (p < q) out[n++] = *p++;
    }
    out[n++] = (char)0xE2;
    out[n++] = (char)0x80;
    out[n++] = (char)0xA6;
    out[n] = 0;
}

void cv_text_center(int x, int y, int w, int h, const char* s, int color)
{
    char buf[160];
    fit(s, w - 4, buf, sizeof buf);
    const int tw = cv_text_width(buf);
    cv_text(x + (w - tw) / 2, y + (h - FONT_H) / 2 + 1, buf, color);
}

void cv_text_fit(int x, int y, int w, const char* s, int color)
{
    char buf[160];
    fit(s, w, buf, sizeof buf);
    cv_text(x, y, buf, color);
}
