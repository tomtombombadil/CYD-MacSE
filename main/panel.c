// The 800x480 RGB panel, shown straight from the 1-bit canvas.
//
// As cyd-classic-arcade's Panel_RgbBounce: ESP-IDF's RGB driver with no
// frame buffer of its own, two small bounce buffers in internal RAM that
// the DMA reads, and our refill (on_bounce_empty) - here it expands the
// canvas's bits to RGB565 instead of copying a PSRAM frame. Nothing of the
// picture is in PSRAM, so the emulated Mac's RAM (PSRAM) keeps the cache.
#include "panel.h"

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_log.h>
#include <string.h>

#include "board.h"
#include "cyd_mac.h"

static const char* TAG = "panel";

DRAM_ATTR uint8_t g_canvas[CANVAS_H * CANVAS_STRIDE];

// one canvas byte -> 8 pixels (16 bytes): white 0xFFFF, black 0x0000
static DRAM_ATTR uint32_t s_expand[256][4];

static IRAM_ATTR bool refill(esp_lcd_panel_handle_t panel, void* bounce_buf, int pos_px, int len_bytes, void* ctx)
{
    (void)panel;
    (void)ctx;
    const uint8_t* src = &g_canvas[pos_px >> 3];
    uint32_t* dst = (uint32_t*)bounce_buf;
    int n = len_bytes >> 4;   // canvas bytes
    if ((pos_px >> 3) + n > (int)sizeof g_canvas) n = (int)sizeof g_canvas - (pos_px >> 3);
    while (n-- > 0) {
        const uint32_t* e = s_expand[*src++];
        dst[0] = e[0];
        dst[1] = e[1];
        dst[2] = e[2];
        dst[3] = e[3];
        dst += 4;
    }
    return false;
}

bool panel_init(void)
{
    for (int b = 0; b < 256; ++b)
        for (int i = 0; i < 4; ++i) {
            // pixels 2i (low half-word: first in memory) and 2i+1
            const uint32_t p0 = (b & (0x80 >> (2 * i))) ? 0x0000 : 0xFFFF;
            const uint32_t p1 = (b & (0x40 >> (2 * i))) ? 0x0000 : 0xFFFF;
            s_expand[b][i] = p0 | (p1 << 16);
        }
    memset(g_canvas, 0, sizeof g_canvas);

    esp_lcd_rgb_panel_config_t c = {0};
    c.clk_src = LCD_CLK_SRC_DEFAULT;
    c.timings.pclk_hz = RGB_PCLK_HZ;
    c.timings.h_res = BOARD_W;
    c.timings.v_res = BOARD_H;
    c.timings.hsync_pulse_width = 4;
    c.timings.hsync_back_porch = 8;
    c.timings.hsync_front_porch = 4;
    c.timings.vsync_pulse_width = 4;
    c.timings.vsync_back_porch = 8;
    c.timings.vsync_front_porch = 4;
    c.timings.flags.hsync_idle_low = 1;
    c.timings.flags.vsync_idle_low = 1;
    c.timings.flags.pclk_active_neg = 1;
    c.data_width = 16;
    c.bits_per_pixel = 16;
    c.num_fbs = 0;
    c.bounce_buffer_size_px = BOARD_W * RGB_BOUNCE_LINES;
    c.dma_burst_size = 64;
    c.hsync_gpio_num = PIN_HSYNC;
    c.vsync_gpio_num = PIN_VSYNC;
    c.de_gpio_num = PIN_DE;
    c.pclk_gpio_num = PIN_PCLK;
    c.disp_gpio_num = -1;
    static const int kData[16] = RGB_DATA_PINS;
    for (int i = 0; i < 16; ++i) c.data_gpio_nums[i ^ 8] = kData[i];
    c.flags.no_fb = 1;

    esp_lcd_panel_handle_t h = NULL;
    esp_err_t err = esp_lcd_new_rgb_panel(&c, &h);
    if (err == ESP_OK) {
        esp_lcd_rgb_panel_event_callbacks_t cb = {0};
        cb.on_bounce_empty = refill;
        err = esp_lcd_rgb_panel_register_event_callbacks(h, &cb, NULL);
    }
    if (err == ESP_OK) err = esp_lcd_panel_reset(h);
    if (err == ESP_OK) err = esp_lcd_panel_init(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB panel didn't start: %s", esp_err_to_name(err));
        return false;
    }
    // the strongest drive on the bus (cyd-classic-arcade v0.26.7: stripes
    // on a warm board at ESP-IDF's default)
    for (int i = 0; i < 16; ++i) gpio_set_drive_capability((gpio_num_t)kData[i], GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)PIN_PCLK, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)PIN_HSYNC, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)PIN_VSYNC, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)PIN_DE, GPIO_DRIVE_CAP_3);

    gpio_reset_pin((gpio_num_t)PIN_BACKLIGHT);
    gpio_set_direction((gpio_num_t)PIN_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_BACKLIGHT, 1);
    return true;
}
