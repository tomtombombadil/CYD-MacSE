// Mac sound out: 22255 samples a second, 8-bit, into the NS4168 amp by I2S.
#include "audio.h"

#include <driver/i2s_std.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "cyd_mac.h"

static const char* TAG = "audio";
static i2s_chan_handle_t s_tx;
#define CHUNK 256

static void audio_task(void* arg)
{
    (void)arg;
    uint8_t in[CHUNK];
    int16_t out[CHUNK];
    for (;;) {
        emu_audio_pull(in, CHUNK);
        const bool on = g_emu.sound_on;
        for (int i = 0; i < CHUNK; ++i) out[i] = on ? (int16_t)(((int)in[i] - 128) * 96) : 0;
        size_t done = 0;
        i2s_channel_write(s_tx, out, sizeof out, &done, portMAX_DELAY);   // paces the loop
    }
}

bool audio_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = CHUNK;
    cc.auto_clear = true;
    esp_err_t err = i2s_new_channel(&cc, &s_tx, NULL);
    if (err == ESP_OK) {
        i2s_std_config_t sc = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EMU_SOUND_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,   // an amp: no master clock
                .bclk = (gpio_num_t)PIN_I2S_BCLK,
                .ws = (gpio_num_t)PIN_I2S_WS,
                .dout = (gpio_num_t)PIN_I2S_DOUT,
                .din = I2S_GPIO_UNUSED,
            },
        };
        sc.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
        err = i2s_channel_init_std_mode(s_tx, &sc);
    }
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S start failed: %s", esp_err_to_name(err));
        return false;
    }
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 6, NULL, 0);
    return true;
}
