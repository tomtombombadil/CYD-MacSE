#pragma once
// The board's log (ESP_LOG lines) copied to the SD card: mac\log.txt (this
// start) and mac\log-old.txt (the one before). Crash reports are added to
// mac\crash.txt. Take the card to a PC to read them.
#include <stdbool.h>

void sdlog_init(void);    // after the card is mounted
void sdlog_flush(void);   // about once a second, from the UI task
void sdlog_crash(const char* const* lines, int n);
