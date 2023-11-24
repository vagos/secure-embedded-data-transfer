/* Secure Embdedded Data Transfer

   This code is in the Public Domain. 

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "esp_system.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_random.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include <sys/param.h>
#include "dac_audio.h"
/* #include "app_main.h" */

#define DAC_BUFFER_SIZE 1024

static const char *TAG = "SEDT";
static int CLIENT_ID;

#undef CONFIG_BROKER_URL
#define CONFIG_BROKER_URL "mqtt://broker.hivemq.com"

#define TOPIC_SUBSCRIBE "/topic/sedt/#"
//#define TOPIC_PUBLISH "/topic/sedt"

uint8_t mic_data[DAC_BUFFER_SIZE];
uint8_t mic_data_len;

#if CONFIG_BROKER_CERTIFICATE_OVERRIDDEN == 1
static const uint8_t mqtt_eclipseprojects_io_pem_start[]  = "-----BEGIN CERTIFICATE-----\n" CONFIG_BROKER_CERTIFICATE_OVERRIDE "\n-----END CERTIFICATE-----";
#else
extern const uint8_t mqtt_eclipseprojects_io_pem_start[]   asm("_binary_mqtt_pem_start");
#endif
extern const uint8_t mqtt_eclipseprojects_io_pem_end[]   asm("_binary_mqtt_pem_end");

/*
 * Event handler registered to receive MQTT events
 * This function is called by the MQTT client event loop.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        esp_mqtt_client_subscribe(client, TOPIC_SUBSCRIBE, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "DATA: ");
        ESP_LOG_BUFFER_HEX(TAG, event->data, event->data_len);
        mic_data_len = event->data_len;
        for (int i = 0; i < DAC_BUFFER_SIZE / mic_data_len; i++)
        {
            memcpy(mic_data + i * mic_data_len, event->data, mic_data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGI(TAG, "Last captured errno : %d (%s)",  event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        } else {
            ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mic_out_start(void *arg)
{
    static const char *MICTAG = "MIC_TASK";
    ESP_LOGI(MICTAG, "DAC audio example start");
    ESP_LOGI(MICTAG, "--------------------------------------");
    static uint32_t cnt = 0;
    dac_continuous_handle_t dac_handle;
    // Using APLL as clock source to get a wider frequency range
    /* Assume the data in buffer is 'A B C D E F'
        * DAC_CHANNEL_MODE_SIMUL:
        *      - channel 0: A B C D E F
        *      - channel 1: A B C D E F
        * DAC_CHANNEL_MODE_ALTER:
        *      - channel 0: A C E
        *      - channel 1: B D F
        */
    dac_continuous_config_t cont_cfg = {
        .chan_mask = DAC_CHANNEL_MASK_ALL,
        .desc_num = 4,
        .buf_size = 1024,
        .freq_hz = CONFIG_AUDIO_SAMPLE_RATE,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_APLL,   
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    /* Allocate continuous channels */
    ESP_ERROR_CHECK(dac_continuous_new_channels(&cont_cfg, &dac_handle));
    ESP_ERROR_CHECK(dac_continuous_enable(dac_handle));
    ESP_LOGI(TAG, "DAC initialized success, DAC DMA is ready");
    while(1)
    {
        ESP_LOGI(TAG, "Playing: ");
        dac_write_data_synchronously(dac_handle, mic_data, DAC_BUFFER_SIZE);
        ESP_LOGI(TAG, "Play count: %"PRIu32"\n", cnt++);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = CONFIG_BROKER_URL,
            /* .verification.certificate = (const char *)mqtt_eclipseprojects_io_pem_start */
        },
    };

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
    
    /* TODO: Remove */
    while (true) 
	{
        int delay = 1000;
        vTaskDelay(delay / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_example", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(example_connect());
    /* Set client ID to some random integer. */
    CLIENT_ID = esp_random();
    ESP_LOGI(TAG, "client id: %d", CLIENT_ID);
    xTaskCreate(mic_out_start, "mic_task", 4096, NULL, 2, NULL);
    mqtt_app_start();
    vTaskStartScheduler();
	while(1);
}
