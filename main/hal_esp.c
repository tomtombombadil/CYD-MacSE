// The CYD's platform services (see cyd_mac.h) and the SD card.
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdmmc_cmd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board.h"
#include "cyd_mac.h"
#include "crash_report.h"
#include "hal_esp.h"
#include "wifi_web.h"

static const char* TAG = "sd";
static SemaphoreHandle_t s_lock;
static const char* s_dirs[3] = {NULL, NULL, NULL};

uint64_t hal_time_us(void) { return (uint64_t)esp_timer_get_time(); }

void hal_sleep_ms(int ms)
{
    vTaskDelay(ms > 0 ? pdMS_TO_TICKS(ms) : 1);
}

void hal_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void hal_unlock(void) { xSemaphoreGive(s_lock); }

const char* const* hal_dirs(void) { return s_dirs; }

int64_t hal_unix_time(void) { return 0; }   // no clock on the board

void hal_sync_file(void* f) { fsync(fileno((FILE*)f)); }

bool g_wifi_on;
const char* hal_net_address(void) { return g_wifi_on ? wifi_web_address() : NULL; }

void hal_after_disk_io(int is_write, uint32_t start, uint32_t count, const void* mac_buf)
{
    crash_check_after_io(is_write, start, count, mac_buf);
}

bool hal_esp_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    spi_bus_config_t bus = {0};
    bus.mosi_io_num = PIN_SD_MOSI;
    bus.miso_io_num = PIN_SD_MISO;
    bus.sclk_io_num = PIN_SD_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 16384;
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = (gpio_num_t)PIN_SD_CS;
    dev.host_id = SPI3_HOST;
    esp_vfs_fat_mount_config_t cfg = {0};
    cfg.format_if_mount_failed = false;
    cfg.max_files = 10;   // 6 drives, the ROM, the settings, the disk list
    cfg.allocation_unit_size = 16 * 1024;
    sdmmc_card_t* card = NULL;
    const esp_err_t err = esp_vfs_fat_sdspi_mount("/sd", &host, &dev, &cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no SD card (%s)", esp_err_to_name(err));
        s_dirs[0] = "/sd";
        return false;
    }
    struct stat st;
    if (stat("/sd/mac", &st) == 0 && S_ISDIR(st.st_mode)) {
        s_dirs[0] = "/sd/mac";
        s_dirs[1] = "/sd";
    } else {
        s_dirs[0] = "/sd";
    }
    ESP_LOGI(TAG, "SD card mounted, files in %s", s_dirs[0]);
    return true;
}
