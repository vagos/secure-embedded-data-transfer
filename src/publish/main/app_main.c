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
#include "ring_buffer.h"
#include "continuous_read.h"

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

#undef CONFIG_BROKER_URL
#define CONFIG_BROKER_URL "mqtt://broker.hivemq.com"

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

/*----------------------------------------------------------------------------------------------------------*/
//
// Note: this function is for testing purposes only publishing part of the active partition
//       (to be checked against the original binary)
//
static void send_binary(esp_mqtt_client_handle_t client)
{
    esp_partition_mmap_handle_t out_handle;
    const void *binary_address;
    const esp_partition_t *partition = esp_ota_get_running_partition();
    esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA, &binary_address, &out_handle);
    // sending only the configured portion of the partition (if it's less than the partition size)
    int binary_size = MIN(CONFIG_BROKER_BIN_SIZE_TO_SEND, partition->size);
    int msg_id = esp_mqtt_client_publish(client, "/topic/binary", binary_address, binary_size, 0, 0);
    ESP_LOGI(TAG, "binary sent with msg_id=%d", msg_id);
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
            if (strncmp(event->data, "send binary please", event->data_len) == 0) 
            {
                ESP_LOGI(TAG, "Sending the binary");
                send_binary(client);
            }
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

static void mqtt_app_start(void *arg)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = CONFIG_BROKER_URI,
            .verification.certificate = (const char *)mqtt_eclipseprojects_io_pem_start
        },
    };

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
    
    MIC_DATA_T mqtt_msg_buffer[MQTT_RING_SIZE];
    memset(&mqtt_msg_buffer,0,sizeof(MIC_DATA_T) * MQTT_RING_SIZE);

    /* TODO: Remove */
    while (true) 
	{
        int nframe = ring_buffer_pop(mqtt_ring_buffer, (uint8_t *)&mqtt_msg_buffer[0], sizeof(MIC_DATA_T) * MQTT_RING_SIZE);
        esp_mqtt_client_publish(client, TOPIC_PUBLISH, "hello", 0, 0, 0);

		if(nframe > 0)
		{
			for(int i=0; i < nframe; i++)
			{
                //&mqtt_msg[i]
				int msg_id = esp_mqtt_client_publish(client, TOPIC_PUBLISH, (char *)mqtt_msg_buffer[i].data, 0, 0, 0);
                ESP_LOGI(TAG, "sent publish successful, msg_id=%d client=%d,data:%s", msg_id, CLIENT_ID,mqtt_msg_buffer[i].data);
			}  
            //ESP_LOGI(TAG, "sent publish successful\r\n");
		}
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
static void data_app_start(void *arg)
{
    MIC_DATA_T mic_data;
    char buf[] = "23"; 
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
                    ESP_LOGI(TAG, "Unit: %s, Channel: %"PRIu32", Value: %"PRIx32, unit, chan_num, data);
                } 
                else 
                {
                    ESP_LOGW(TAG, "Invalid data [%s_%"PRIu32"_%"PRIx32"]", unit, chan_num, data);
                }
            }
        } 
        else if (ret == ESP_ERR_TIMEOUT) 
        {
            //We try to read `EXAMPLE_READ_LEN` until API returns timeout, which means there's no available data
            ESP_LOGW(TAG, "ADC Timeout\r\n");
        }
        memset(&mic_data,0,sizeof(MIC_DATA_T));
        memcpy(mic_data.data, buf, sizeof(buf));
        mic_data.len = sizeof(buf);

        ring_buffer_push(mqtt_ring_buffer, mic_data.data, mic_data.len);

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

    mqtt_ring_buffer = ring_buffer_create(MQTT_RING_SIZE, 3);

    xTaskCreate(data_app_start, "data_task", 4096, NULL, 2, NULL);
    mqtt_app_start(NULL);
    /* xTaskCreate(mqtt_app_start, "mqtt_publish", 2048, NULL, 2, &publish_task_handle); */
    vTaskStartScheduler();
	while(1);
}
