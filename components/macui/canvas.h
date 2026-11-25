// Drawing on the 1-bit canvas (g_canvas): 1 = black, 0 = white.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { CV_WHITE = 0, CV_BLACK = 1, CV_INVERT = 2 };

void cv_fill(int x, int y, int w, int h, int color);
void cv_frame(int x, int y, int w, int h, int color);
// A key: rounded corners (radius 4), 1 px frame; filled = black inside
void cv_key(int x, int y, int w, int h, int filled);
// Text (UTF-8; arrows and the command / option signs are drawn too).
// Returns the width in pixels. y = top of the 16 px line.
int cv_text(int x, int y, const char* s, int color);
int cv_text_width(const char* s);
// Centred in the box, cut with "..." if too wide
void cv_text_center(int x, int y, int w, int h, const char* s, int color);
// Left-aligned in a box of width w, cut with "..." if too wide
void cv_text_fit(int x, int y, int w, const char* s, int color);

#ifdef __cplusplus
}
#endif
