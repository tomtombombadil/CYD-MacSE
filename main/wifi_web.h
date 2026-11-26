#pragma once
#include <stdbool.h>
// Join the Wi-Fi named in mac\wifi.txt (if there is one) and serve the logs
// at http://<address>/. Returns false when there is no wifi.txt.
bool wifi_web_start(void);
// "192.168.1.23" once joined, else ""
const char* wifi_web_address(void);
