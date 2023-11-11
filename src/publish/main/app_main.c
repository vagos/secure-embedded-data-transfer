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
#include "mbedtls/sha256.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include <sys/param.h>
#include "rom/sha.h"
#include "mbedtls/aes.h"

#include "ring_buffer.h"
#include "continuous_read.h"
#include "shuffle_buffer.h"

#define SHA256_DIGEST 32

#if CONFIG_IDF_TARGET_ESP32
static adc_channel_t channel[1] = {ADC_CHANNEL_6};
#else
static adc_channel_t channel[2] = {ADC_CHANNEL_2, ADC_CHANNEL_3};
#endif

static const char *TAG = "SEDT";
static int CLIENT_ID;
static const int MQTT_RING_SIZE = 10;
static RING_BUFFER_T *mqtt_ring_buffer = NULL;
static adc_continuous_handle_t adc_handle = NULL;

#undef CONFIG_BROKER_URI
#define CONFIG_BROKER_URI "mqtt://broker.hivemq.com"

#define TOPIC_SUBSCRIBE "/topic/sedt/#"
#define TOPIC_PUBLISH "/topic/sedt"

#if CONFIG_BROKER_CERTIFICATE_OVERRIDDEN == 1
static const uint8_t mqtt_eclipseprojects_io_pem_start[]  = "-----BEGIN CERTIFICATE-----\n" CONFIG_BROKER_CERTIFICATE_OVERRIDE "\n-----END CERTIFICATE-----";
#else
extern const uint8_t mqtt_eclipseprojects_io_pem_start[]   asm("_binary_mqtt_pem_start");
#endif
extern const uint8_t mqtt_eclipseprojects_io_pem_end[]   asm("_binary_mqtt_pem_end");

/*
 * ADC function
 * 
 */
#if 0
static bool IRAM_ATTR s_conv_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    BaseType_t mustYield = pdFALSE;
    //Notify that ADC continuous driver has done enough number of conversions
    vTaskNotifyGiveFromISR(adc_task_handle, &mustYield);

    return (mustYield == pdTRUE);
}
#endif
static void adc_init()
{
    continuous_adc_init(channel, sizeof(channel) / sizeof(adc_channel_t), &adc_handle);
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));
}

/*
 * Event handler registered to receive MQTT events
 * This function is called by the MQTT client event loop.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    switch ((esp_mqtt_event_id_t)event_id) 
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) 
            {
                ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
                ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
                ESP_LOGI(TAG, "Last captured errno : %d (%s)",  event->error_handle->esp_transport_sock_errno,
                        strerror(event->error_handle->esp_transport_sock_errno));
            } 
            else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) 
            {
                ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
            } 
            else 
            {
                ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
            }
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
}

void encrypt_message(const unsigned char* input, unsigned char* encrypted, int n)
{
    const unsigned char key[32] = {0x13, 0x37, 0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37};
    unsigned char iv[] = {0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 256); //set key for encryption

    mbedtls_aes_crypt_cfb8(&aes, MBEDTLS_AES_ENCRYPT, n, iv, input, encrypted);
}

void decrypt_message(const unsigned char* input, unsigned char* decrypted, int n)
{
    const unsigned char key[32] = {0x13, 0x37, 0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37,0x13, 0x37};
    unsigned char iv[] = {0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    mbedtls_aes_context aes; //aes is simple variable of given type
    mbedtls_aes_init(&aes);

    mbedtls_aes_crypt_cfb8(&aes, MBEDTLS_AES_DECRYPT, n, iv, input, decrypted);
}

static void mqtt_app_start(void *arg)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = CONFIG_BROKER_URI,
            /* .verification.certificate = (const char *)mqtt_eclipseprojects_io_pem_start */
        },
    };

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
    
    RING_BUFFER_DATA_T mqtt_msg_buffer[MQTT_RING_SIZE];
    memset(&mqtt_msg_buffer,0, MQTT_RING_SIZE);

    /* TODO: Remove */
    while (true) 
	{
        int nframe = ring_buffer_pop(mqtt_ring_buffer, (uint8_t *)mqtt_msg_buffer, sizeof(RING_BUFFER_DATA_T) * MQTT_RING_SIZE);

        if (nframe <= 0) continue;

        /* unsigned char sha256_buffer[SHA256_DIGEST]; */

        uint8_t encrypted[DATA_LEN];

        for(int i=0; i < nframe; i++)
        {
            RING_BUFFER_DATA_T *data = &mqtt_msg_buffer[i]; 
            /* ESP_LOGI(TAG, "data: %.*s", data->len, data->data); */
            /* mbedtls_sha256(data->data, data->len, sha256_buffer, 0); */
            /* ESP_LOG_BUFFER_HEX(TAG, sha256_buffer, sizeof(sha256_buffer)); */
            /* encrypt_message(data->data, encrypted, data->len); */
            /* ESP_LOG_BUFFER_HEX(TAG, encrypted, data->len); */

            /* decrypt_message(encrypted, data->data, data->len); */
            /* ESP_LOGI(TAG, "data: %.*s", data->len, data->data); */
            
            esp_mqtt_client_publish(client, TOPIC_PUBLISH, (char *)data->data, data->len, 1, 0);
            ESP_LOG_BUFFER_HEX(TAG, data->data, data->len);
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

static void data_app_start(void *arg)
{
    RING_BUFFER_DATA_T mic_data;
    char buf[DATA_LEN] = "hello"; 
    ESP_LOGI(TAG, "data_app_start\r\n");
    uint32_t ret_num = 0;
    esp_err_t ret;
    uint8_t result[EXAMPLE_READ_LEN] = {0};
    memset(result, 0xcc, EXAMPLE_READ_LEN);
    char unit[] = EXAMPLE_ADC_UNIT_STR(EXAMPLE_ADC_UNIT);
    adc_init();

    while(true)
    {
        ret = adc_continuous_read(adc_handle, result, EXAMPLE_READ_LEN, &ret_num, 0);
        if (ret == ESP_OK)
        {
            //ESP_LOGI("TASK", "ret is %x, ret_num is %"PRIu32" bytes", ret, ret_num);
            for (int i = 0; i < ret_num; i += SOC_ADC_DIGI_RESULT_BYTES) 
            {
                adc_digi_output_data_t *p = (void*)&result[i];
                uint32_t chan_num = EXAMPLE_ADC_GET_CHANNEL(p);
                uint32_t data = EXAMPLE_ADC_GET_DATA(p);
                /* Check the channel number validation, the data is invalid if the channel num exceed the maximum channel */
                if (chan_num < SOC_ADC_CHANNEL_NUM(EXAMPLE_ADC_UNIT))
                {
                    /* ESP_LOGI(TAG, "Unit: %s, Channel: %"PRIu32", Value: %"PRIx32, unit, chan_num, data); */
                    memcpy(mic_data.data, &data, sizeof(data));
                    mic_data.len = sizeof(data);
                    ring_buffer_push(mqtt_ring_buffer, (uint8_t*)&mic_data, sizeof(RING_BUFFER_DATA_T));
                } 
                else 
                {
                    /* ESP_LOGW(TAG, "Invalid data [%s_%"PRIu32"_%"PRIx32"]", unit, chan_num, data); */
                }
            }
        } 
        else if (ret == ESP_ERR_TIMEOUT) 
        {
            //We try to read `EXAMPLE_READ_LEN` until API returns timeout, which means there's no available data
            ESP_LOGW(TAG, "ADC Timeout\r\n");
        }


        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    for(;;);

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

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    /* Set client ID to some random integer. */
    CLIENT_ID = esp_random();
    ESP_LOGI(TAG, "client id: %d", CLIENT_ID);

    mqtt_ring_buffer = ring_buffer_create(MQTT_RING_SIZE, sizeof(RING_BUFFER_DATA_T));

    xTaskCreate(data_app_start, "data_task", 4096, NULL, 2, NULL);
    mqtt_app_start(NULL);
    /* xTaskCreate(mqtt_app_start, "mqtt_publish", 2048, NULL, 2, &publish_task_handle); */
    vTaskStartScheduler();
	while(1);
}
