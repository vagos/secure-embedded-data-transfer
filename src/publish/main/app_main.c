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


static const char *TAG = "SEDT";
static int CLIENT_ID;
static const int MQTT_RING_SIZE = 64;
static RING_BUFFER_T *mqtt_ring_buffer = NULL;
static TaskHandle_t publish_task_handle = NULL;
static TaskHandle_t data_task_handle = NULL;

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
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
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
        if (strncmp(event->data, "send binary please", event->data_len) == 0) {
            ESP_LOGI(TAG, "Sending the binary");
            send_binary(client);
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
static void push_mqtt_data(MQTT_MSG_T data)
{
	ring_buffer_push(mqtt_ring_buffer, data.mic_data, data.mic_length);
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
    MQTT_MSG_T mqtt_msg;
    mqtt_ring_buffer = ring_buffer_create(MQTT_RING_SIZE,sizeof(MQTT_MSG_T));
    memset(&mqtt_msg,0,sizeof(MQTT_MSG_T));
    /* TODO: Remove */
    while (true) 
	{
        int nframe = ring_buffer_pop(mqtt_ring_buffer, mqtt_msg.mic_data, sizeof(MQTT_MSG_T));
        esp_mqtt_client_publish(client, TOPIC_PUBLISH, "hello", 0, 0, 0);
        #if 0
		if(nframe > 0)
		{
			for(int i=0;i<nframe;i++)
			{
                //&mqtt_msg[i]
				int msg_id = esp_mqtt_client_publish(client, TOPIC_PUBLISH, "hello", 0, 0, 0);
                ESP_LOGI(TAG, "sent publish successful, msg_id=%d client=%d", msg_id, CLIENT_ID);
                //int delay = 1000;
                //vTaskDelay(delay / portTICK_PERIOD_MS);
			}  
            ESP_LOGI(TAG, "sent publish successful\r\n");
		}
        #endif
        vTaskDelay(100 / portTICK_PERIOD_MS);
        #if 0
        int msg_id = esp_mqtt_client_publish(client, TOPIC_PUBLISH, "data-while", 0, 0, 0);
        int delay = 1000;
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d client=%d", msg_id, CLIENT_ID);
        vTaskDelay(delay / portTICK_PERIOD_MS);
        #endif
    }
}
static void data_app_start(void *arg)
{
    MQTT_MSG_T mic_data={0};
    char *buf = "he"; 

    while(true)
    {
        memset(&mic_data,0,sizeof(MQTT_MSG_T));
        memcpy(mic_data.mic_data,buf,2);
        mic_data.mic_length = 2;
        push_mqtt_data(mic_data);
        //printf("data_app_start\r\n");
        ESP_LOGI(TAG, "data_app_start\r\n");
        vTaskDelay(100 / portTICK_PERIOD_MS);
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

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    /* Set client ID to some random integer. */
    CLIENT_ID = esp_random();
    ESP_LOGI(TAG, "client id: %d", CLIENT_ID);
    xTaskCreate(data_app_start, "data_task", 512, NULL, 20, &data_task_handle);
    xTaskCreate(mqtt_app_start, "mqtt_publish", 512, NULL, 20, &publish_task_handle);
    vTaskStartScheduler();
	while(1);
    //mqtt_app_start();
}
