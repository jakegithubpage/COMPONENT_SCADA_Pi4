#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include <string.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "mqtt_client.h"
#include <errno.h>



#define WIFI_SSID      "xxxxx"
#define WIFI_PASS      "xxxxx"

#define GATEWAY_IP     "x.x.x.x"
#define GATEWAY_PORT   9100

#define MQTT_URI       "mqtt://x.x.x.x:1885"
#define MQTT_SUB_TOPIC "sensors/rotary/0/cmd"
#define MQTT_PUB_TOPIC "sensors/rotary/0/status"

#define DEVICE_ID      3

#define ENC_A          GPIO_NUM_5
#define ENC_B          GPIO_NUM_6
#define ENC_SW         GPIO_NUM_7

#define Sev_D1         GPIO_NUM_15
#define Sev_a          GPIO_NUM_16
#define Sev_f          GPIO_NUM_17
#define Sev_D2         GPIO_NUM_18
#define Sev_D3         GPIO_NUM_8
#define Sev_b          GPIO_NUM_3
#define Sev_D4         GPIO_NUM_37
#define Sev_g          GPIO_NUM_38
#define Sev_c          GPIO_NUM_39
#define Sev_dp         GPIO_NUM_40
#define Sev_d          GPIO_NUM_41
#define Sev_e          GPIO_NUM_42

static const char *TAG = "Rotary Encoder";
static volatile int RightAc;
static volatile int LeftAc;
static QueueHandle_t encoderQueue;
static volatile int encoderDelta = 0;
static bool wifi_connected = false;
static volatile bool enabCheck = false;
static volatile bool rotaryEnabled = false;
static esp_mqtt_client_handle_t mqtt_client;


static void mqtt_event_handler(void *arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;

    switch(event->event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI("MQTT", "Connected to broker");

            esp_mqtt_client_subscribe(
                mqtt_client,
                MQTT_SUB_TOPIC,
                1
                );
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW("MQTT", "DISCONNECTED");
            break;
        
        case MQTT_EVENT_DATA:
             ESP_LOGI("MQTT",
                     "RX topic=%.*s data=%.*s",
                     event->topic_len,
                     event->topic,
                     event->data_len,
                     event->data);
            
            if (strncmp(event->topic,
                        MQTT_SUB_TOPIC,
                        event->topic_len) == 0)
            {

                cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);

                if (root) 
                {
                    cJSON *enable = cJSON_GetObjectItem(root, "enable");

                    if (cJSON_IsBool(enable))
                    {
                        rotaryEnabled = cJSON_IsTrue(enable);
                        ESP_LOGI(TAG, "ROTARY %s (JSON)",
                        rotaryEnabled ? "ENABLED" : "DISABLED");
                        cJSON_Delete(root);
                        break;

                    }
                    cJSON_Delete(root);
                }
                if (strncmp(event->data, "ON", event->data_len) == 0)
                {
                    rotaryEnabled = true;
                    
                }
                else if (strncmp(event->data, "OFF", event->data_len) == 0)
                {
                    rotaryEnabled = false;
                }
                else 
                {
                    ESP_LOGW("Rotary", "Uknown payload -> Unable to identify");

                }
            }
            break;

        case MQTT_EVENT_ERROR:

            esp_mqtt_error_type_t et = event->error_handle->error_type;
            ESP_LOGE("MQTT", "MQTT_EVENT_ERROR type=%d", (int)et);
            break;

        default:
            break;
        }
        
    
}

static void wifi_event_handler(void* arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_connected = false;
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        wifi_connected = true;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        printf("ESP32 IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_URI,
    };

    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt_client,
                                   ESP_EVENT_ANY_ID,
                                   mqtt_event_handler,
                                   NULL);
    esp_mqtt_client_start(mqtt_client);
}

static void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               &wifi_event_handler, NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}




static void IRAM_ATTR encoder_isr(void *arg)
{
    int a = gpio_get_level(ENC_A);
    int b = gpio_get_level(ENC_B);

    int delta = (a == b) ? -1 : +1;

    xQueueSendFromISR(encoderQueue, &delta, NULL);
}

static void encoder_init(void)
{
    gpio_config_t io = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
        .pin_bit_mask =
            (1ULL << ENC_A) |
            (1ULL << ENC_B) | 
            (1ULL << ENC_SW),
    };
    gpio_config(&io);
    encoderQueue = xQueueCreate(16, sizeof(int));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(ENC_A, encoder_isr, NULL);


}

static void sevenSeg_init(void)
{
    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask =
            (1ULL << Sev_D1) |
            (1ULL << Sev_a) |
            (1ULL << Sev_f) |
            (1ULL << Sev_D2) |
            (1ULL << Sev_D3) |
            (1ULL << Sev_b) |
            (1ULL << Sev_D4) |
            (1ULL << Sev_g) |
            (1ULL << Sev_c) |
            (1ULL << Sev_dp) |
            (1ULL << Sev_d) |
            (1ULL << Sev_e),
    };
    gpio_config(&io);

}

static inline void sevenSeg_allOff(void)
{
    gpio_set_level(Sev_D1, 1);
    gpio_set_level(Sev_D2, 1);
    gpio_set_level(Sev_D3, 1);
    gpio_set_level(Sev_D4, 1);
}

static void sevenSeg_leftAc(int digit)
{
    sevenSeg_allOff();   // 🔴 THIS IS THE FIX

    switch (digit)
    {
        case 1: // L
            gpio_set_level(Sev_D1, 0);
            gpio_set_level(Sev_f, 1);
            gpio_set_level(Sev_e, 1);
            gpio_set_level(Sev_d, 1);
            gpio_set_level(Sev_g, 0);
            gpio_set_level(Sev_c, 0);
            gpio_set_level(Sev_b, 0);
            gpio_set_level(Sev_a, 0);
            break;

        case 2: // 1
            gpio_set_level(Sev_D2, 0);
            gpio_set_level(Sev_f, 0);
            gpio_set_level(Sev_e, 0);
            gpio_set_level(Sev_d, 0);
            gpio_set_level(Sev_g, 0);
            gpio_set_level(Sev_c, 1);
            gpio_set_level(Sev_b, 1);
            gpio_set_level(Sev_a, 0);
            break;

        case 3: // r
            gpio_set_level(Sev_D3, 0);
            gpio_set_level(Sev_f, 0);
            gpio_set_level(Sev_e, 1);
            gpio_set_level(Sev_d, 0);
            gpio_set_level(Sev_g, 1);
            gpio_set_level(Sev_c, 0);
            gpio_set_level(Sev_b, 0);
            gpio_set_level(Sev_a, 0);
            break;

        case 4: // 0
            gpio_set_level(Sev_D4, 0);
            gpio_set_level(Sev_f, 1);
            gpio_set_level(Sev_e, 1);
            gpio_set_level(Sev_d, 1);
            gpio_set_level(Sev_g, 0);
            gpio_set_level(Sev_c, 1);
            gpio_set_level(Sev_b, 1);
            gpio_set_level(Sev_a, 1);
            break;
    }
}

static void sevenSeg_RightAc(int digit)
{
    sevenSeg_allOff();   

    switch (digit)
    {
        case 1: // L
            gpio_set_level(Sev_D1, 0);
            gpio_set_level(Sev_f, 1);
            gpio_set_level(Sev_e, 1);
            gpio_set_level(Sev_d, 1);
            gpio_set_level(Sev_g, 0);
            gpio_set_level(Sev_c, 0);
            gpio_set_level(Sev_b, 0);
            gpio_set_level(Sev_a, 0);
            break;

        case 2: // 0
            gpio_set_level(Sev_D4, 0);
            gpio_set_level(Sev_f, 1);
            gpio_set_level(Sev_e, 1);
            gpio_set_level(Sev_d, 1);
            gpio_set_level(Sev_g, 0);
            gpio_set_level(Sev_c, 1);
            gpio_set_level(Sev_b, 1);
            gpio_set_level(Sev_a, 1);
            break;

        case 3: // r
            gpio_set_level(Sev_D3, 0);
            gpio_set_level(Sev_f, 0);
            gpio_set_level(Sev_e, 1);
            gpio_set_level(Sev_d, 0);
            gpio_set_level(Sev_g, 1);
            gpio_set_level(Sev_c, 0);
            gpio_set_level(Sev_b, 0);
            gpio_set_level(Sev_a, 0);
            break;

        case 4: // 1
            gpio_set_level(Sev_D2, 0);
            gpio_set_level(Sev_f, 0);
            gpio_set_level(Sev_e, 0);
            gpio_set_level(Sev_d, 0);
            gpio_set_level(Sev_g, 0);
            gpio_set_level(Sev_c, 1);
            gpio_set_level(Sev_b, 1);
            gpio_set_level(Sev_a, 0);

            
            break;
    }
}

static void encoder_task(void *arg)
{


int delta;


    while(1){

        if(rotaryEnabled){

            if(enabCheck) {
            ESP_LOGI(TAG, "Rotary Enabled");
            enabCheck = false;
            }
            if (!xQueueReceive(encoderQueue, &delta, pdMS_TO_TICKS(200))) {
                continue;
            }
            if (delta > 0) {
                ESP_LOGI(TAG, "LEFT");
                RightAc = 0;
                LeftAc = 1;

                sevenSeg_RightAc(1);
                sys_delay_ms(2);

                sevenSeg_RightAc(2);
                sys_delay_ms(2);

                sevenSeg_RightAc(3);
                sys_delay_ms(2);

                sevenSeg_RightAc(4);
                sys_delay_ms(2);

                
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock >= 0) {
                    struct sockaddr_in dest = {0};
                    dest.sin_family = AF_INET;
                    dest.sin_port   = htons(GATEWAY_PORT);
                    inet_pton(AF_INET, GATEWAY_IP, &dest.sin_addr);

                    if (connect(sock,
                                (struct sockaddr*)&dest,
                                sizeof(dest)) == 0)
                    {
                        char msg[64];
                        snprintf(msg, sizeof(msg),
                                 "DEV=%d,TYPE=ROTARY,L=%d,R=%d\n",
                                 DEVICE_ID, LeftAc, RightAc);

                        send(sock, msg, strlen(msg), 0);

                        close(sock);
                    }
                }

            } else {

                RightAc = 1;
                LeftAc = 0;
                ESP_LOGI(TAG,"RIGHT");
                sevenSeg_leftAc(1);
                sys_delay_ms(2);

                sevenSeg_leftAc(2);
                sys_delay_ms(2);

                sevenSeg_leftAc(3);
                sys_delay_ms(2);

                sevenSeg_leftAc(4);
                sys_delay_ms(2);
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock >= 0) {
                    struct sockaddr_in dest = {0};
                    dest.sin_family = AF_INET;
                    dest.sin_port   = htons(GATEWAY_PORT);
                    inet_pton(AF_INET, GATEWAY_IP, &dest.sin_addr);

                    if (connect(sock,
                                (struct sockaddr*)&dest,
                                sizeof(dest)) == 0)
                    {
                        char msg[64];
                        snprintf(msg, sizeof(msg),
                                 "DEV=%d,TYPE=ROTARY,L=%d,R=%d\n",
                                 DEVICE_ID, LeftAc, RightAc);

                        send(sock, msg, strlen(msg), 0);

                        close(sock);
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
         else {
            vTaskDelay(pdMS_TO_TICKS(50));
            if(!enabCheck) {
                sevenSeg_allOff();
                ESP_LOGI(TAG, "Rotary Disabled");
                enabCheck = true;
            }

        }
    }
    }


void app_main(void)
{

    nvs_flash_init();
    sevenSeg_init();
    wifi_init();

    encoder_init();

    while (!wifi_connected) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }


    mqtt_start();

    xTaskCreate(encoder_task,
                "encoder_task",
                4096,
                NULL,
                5,
                NULL
            );

}
