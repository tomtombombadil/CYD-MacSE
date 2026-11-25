#pragma once
#include <stdbool.h>
// Starts the RGB panel showing g_canvas (call on core 0: its refill
// interrupt runs on the core that starts it)
bool panel_init(void);
