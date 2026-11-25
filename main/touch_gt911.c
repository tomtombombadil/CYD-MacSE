// GT911 capacitive touch on I2C (ESP-IDF's i2c_master driver).
#include "touch_gt911.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/usb_serial_jtag_reg.h>
#include <soc/soc.h>

#include "board.h"

static const char* TAG = "touch";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static bool rd(uint16_t reg, uint8_t* buf, size_t n)
{
    const uint8_t r[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return i2c_master_transmit_receive(s_dev, r, 2, buf, n, 20) == ESP_OK;
}

static bool wr8(uint16_t reg, uint8_t v)
{
    const uint8_t b[3] = {(uint8_t)(reg >> 8), (uint8_t)reg, v};
    return i2c_master_transmit(s_dev, b, 3, 20) == ESP_OK;
}

static bool add_dev(uint16_t addr)
{
    i2c_device_config_t dc = {0};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = addr;
    dc.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(s_bus, &dc, &s_dev) != ESP_OK) return false;
    uint8_t id[4];
    if (rd(0x8140, id, 4)) {   // product id, "911"
        ESP_LOGI(TAG, "GT%c%c%c%c at 0x%02x", id[0], id[1], id[2], id[3] ? id[3] : ' ', addr);
        return true;
    }
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return false;
}

bool touch_init(void)
{
    // GPIO 19 / 20 are the S3's USB pins: take the USB-Serial-JTAG pad off
    // them (USB on this board is the CH340C on UART0)
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE | USB_SERIAL_JTAG_DP_PULLUP);

    // Reset with INT low: address 0x5D. INT is the amp's LRCLK too - let
    // go of it before the sound starts.
    gpio_reset_pin((gpio_num_t)PIN_TOUCH_INT);
    gpio_reset_pin((gpio_num_t)PIN_TOUCH_RST);
    gpio_set_direction((gpio_num_t)PIN_TOUCH_INT, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)PIN_TOUCH_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_TOUCH_INT, 0);
    gpio_set_level((gpio_num_t)PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(11));
    gpio_set_level((gpio_num_t)PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(6));
    gpio_set_direction((gpio_num_t)PIN_TOUCH_INT, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(60));

    i2c_master_bus_config_t bc = {0};
    bc.i2c_port = 0;
    bc.sda_io_num = PIN_I2C_SDA;
    bc.scl_io_num = PIN_I2C_SCL;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = 1;
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) return false;
    for (int tries = 0; tries < 3; ++tries) {
        if (add_dev(0x5D) || add_dev(0x14)) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGE(TAG, "GT911 touch didn't answer");
    return false;
}

bool touch_read(int* x, int* y)
{
    static bool s_last;
    static int s_x, s_y;
    if (!s_dev) return false;
    uint8_t st;
    if (!rd(0x814E, &st, 1)) return s_last;
    if (!(st & 0x80)) return s_last;   // no new reading: as before
    const int n = st & 0x0F;
    if (n > 0) {
        uint8_t p[4];
        if (rd(0x8150, p, 4)) {
            s_x = p[0] | (p[1] << 8);
            s_y = p[2] | (p[3] << 8);
        }
    }
    wr8(0x814E, 0);
    s_last = n > 0;
    *x = s_x;
    *y = s_y;
    return s_last;
}
