#pragma once
#include <stdbool.h>
bool touch_init(void);
// true while touched; x / y in panel pixels (800x480)
bool touch_read(int* x, int* y);
