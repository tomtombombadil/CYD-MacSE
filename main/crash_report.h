#pragma once
#include <stdbool.h>

// If the last restart was a crash / watchdog / brown-out: show what is
// known on the Mac's screen and wait for a tap. Returns true then.
bool crash_report_show(void);
