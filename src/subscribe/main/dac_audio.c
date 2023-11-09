#include "dac_audio.h"


static const char *TAG = "dac_audio_debug";

#if 0
void dac_audio_init(dac_continuous_handle_t *dac_handle)
{
    ESP_LOGI(TAG, "DAC audio start");
    ESP_LOGI(TAG, "--------------------------------------");
    dac_continuous_config_t cont_cfg = {
        .chan_mask = DAC_CHANNEL_MASK_ALL,
        .desc_num = 4,
        .buf_size = 1024,
        .freq_hz = CONFIG_AUDIO_SAMPLE_RATE,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_APLL,   // Using APLL as clock source to get a wider frequency range
        /* Assume the data in buffer is 'A B C D E F'
         * DAC_CHANNEL_MODE_SIMUL:
         *      - channel 0: A B C D E F
         *      - channel 1: A B C D E F
         * DAC_CHANNEL_MODE_ALTER:
         *      - channel 0: A C E
         *      - channel 1: B D F
         */
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    /* Allocate continuous channels */
    ESP_ERROR_CHECK(dac_continuous_new_channels(&cont_cfg, &dac_handle));
    ESP_ERROR_CHECK(dac_continuous_enable(dac_handle));
    ESP_LOGI(TAG, "DAC initialized success, DAC DMA is ready");
}
#endif
void dac_write_data_synchronously(dac_continuous_handle_t handle, uint8_t *data, size_t data_size)
{
    /* ESP_LOGI(TAG, "Audio size %d bytes, played at frequency %d Hz synchronously", data_size, CONFIG_EXAMPLE_AUDIO_SAMPLE_RATE); */
    //uint32_t cnt = 1;
    //printf("Play count: %"PRIu32"\n", cnt++);
    ESP_ERROR_CHECK(dac_continuous_write(handle, data, data_size, NULL, -1));
}
