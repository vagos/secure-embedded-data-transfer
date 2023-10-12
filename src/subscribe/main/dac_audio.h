/*
*   DAC_AUDIO.H
*
**/

#ifndef DAC_AUDIO_H_
#define DAC_AUDIO_H_

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/dac_continuous.h"
#include "esp_check.h"

#define  CONFIG_AUDIO_SAMPLE_RATE 16000

extern void dac_audio_init(dac_continuous_handle_t *handle);
extern void dac_write_data_synchronously(dac_continuous_handle_t handle, uint8_t *data, size_t data_size);

#endif










