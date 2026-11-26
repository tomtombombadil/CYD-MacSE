// The board's logs on a web page on the home network (Tom, 2026-11-25:
// no pulling the SD card). Only when mac\wifi.txt is on the card:
//   ssid=<network name>
//   password=<Wi-Fi password>
// http://<the board's address>/ shows crash.txt, then the end of
// log-old.txt (the start before this one) and log.txt (this start), as
// plain text to copy. The address is on the crash screen and in the log;
// the board also asks the router for the name "cyd-macse".
// Two more optional lines (Tom, 2026-11-25):
//   ntp=<time server>   (else the router, the network's gateway)
//   tz=<POSIX time zone> (else US Eastern: EST5EDT,M3.2.0,M11.1.0)
// The clock is set from the time server once joined; the log and the Mac
// use it.
#include "wifi_web.h"

#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include <stdlib.h>
#include <time.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>

#include "cyd_mac.h"
#include "sdlog.h"

static const char* TAG = "wifi";
static char s_ip[20];
static char s_ntp[64];
static httpd_handle_t s_httpd;

#define TAIL 16384   // bytes of each log shown

static void path(char* out, size_t n, const char* name)
{
    const char* dir = hal_dirs()[0];
    snprintf(out, n, "%s/%s", dir ? dir : "/sd", name);
}

// The last `tail` bytes of a file (all of it for tail 0) into the reply
static void send_file(httpd_req_t* req, const char* name, long tail)
{
    char p[96], buf[1024];
    path(p, sizeof p, name);
    snprintf(buf, sizeof buf, "\n===== %s =====\n", name);
    httpd_resp_sendstr_chunk(req, buf);
    FILE* f = fopen(p, "r");
    if (!f) {
        httpd_resp_sendstr_chunk(req, "(none)\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    if (tail && size > tail) {
        fseek(f, size - tail, SEEK_SET);
        httpd_resp_sendstr_chunk(req, "(...)\n");
    } else {
        fseek(f, 0, SEEK_SET);
    }
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) httpd_resp_send_chunk(req, buf, (ssize_t)n);
    fclose(f);
}

static esp_err_t on_root(httpd_req_t* req)
{
    sdlog_flush();   // what is still waiting goes to log.txt first
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr_chunk(req, "CYD Mac SE " APP_VERSION " - select all, copy and paste it to Claude.\n");
    send_file(req, "crash.txt", TAIL);
    send_file(req, "log-old.txt", TAIL);
    send_file(req, "log.txt", TAIL);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void start_web(void)
{
    if (s_httpd) return;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.core_id = 0;           // core 1 is the Mac's
    c.stack_size = 6144;
    if (httpd_start(&s_httpd, &c) != ESP_OK) return;
    static const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = on_root};
    httpd_register_uri_handler(s_httpd, &root);
}

static void on_time(struct timeval* tv)
{
    (void)tv;
    char b[32];
    const time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S %Z", &lt);
    ESP_LOGI(TAG, "clock set from %s: %s", s_ntp, b);
}

static void start_ntp(const esp_ip4_addr_t* gw)
{
    static bool started;
    if (started) return;
    started = true;
    if (!s_ntp[0]) snprintf(s_ntp, sizeof s_ntp, IPSTR, IP2STR(gw));
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, s_ntp);
    sntp_set_time_sync_notification_cb(on_time);
    esp_sntp_init();
    ESP_LOGI(TAG, "asking %s for the time", s_ntp);
}

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = 0;
        esp_wifi_connect();   // keep trying (the driver paces it)
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "log page: http://%s/", s_ip);
        start_web();
        start_ntp(&e->ip_info.gw);
    }
}

static bool read_settings(char* ssid, char* pass)
{
    static char tz[64] = "EST5EDT,M3.2.0,M11.1.0";
    char p[96];
    path(p, sizeof p, "wifi.txt");
    FILE* f = fopen(p, "r");
    if (!f) return false;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(line, "ssid")) snprintf(ssid, 33, "%s", eq + 1);
        else if (!strcmp(line, "password")) snprintf(pass, 65, "%s", eq + 1);
        else if (!strcmp(line, "ntp")) snprintf(s_ntp, sizeof s_ntp, "%s", eq + 1);
        else if (!strcmp(line, "tz")) snprintf(tz, sizeof tz, "%s", eq + 1);
    }
    fclose(f);
    setenv("TZ", tz, 1);
    tzset();
    return ssid[0] != 0;
}

bool wifi_web_start(void)
{
    static char ssid[33], pass[65];
    if (!read_settings(ssid, pass)) return false;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t* sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, "cyd-macse");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, ssid, strlen(ssid));
    memcpy(wc.sta.password, pass, strlen(pass));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (esp_wifi_start() != ESP_OK) return false;
    ESP_LOGI(TAG, "joining %s", ssid);
    return true;
}

const char* wifi_web_address(void) { return s_ip; }
