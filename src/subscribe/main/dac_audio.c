#include "dac_audio.h"


static const char *TAG = "dac_audio_debug";

void dac_write_data_synchronously(dac_continuous_handle_t handle, uint8_t *data, size_t data_size)
{
    ESP_ERROR_CHECK(dac_continuous_write(handle, data, data_size, NULL, -1));
}
