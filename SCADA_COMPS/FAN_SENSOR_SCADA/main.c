#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mqtt_client.h"
#include <errno.h>
#include "driver/gpio.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#define LEDPIN GPIO_NUM_7
#define FANPIN GPIO_NUM_6
#define SENPIN GPIO_NUM_18

#define WIFI_SSID      "xxxxx"
#define WIFI_PASS      "xxxxx"

#define GATEWAY_IP     "x.x.x.x"  
#define GATEWAY_PORT   9100           
#define DEVICE_ID 2

#define MQTT_URI       "mqtt://x.x.x.x:1885"
#define MQTT_SUB_TOPIC "sensors/mosense/0/cmd"
#define MQTT_PUB_TOPIC "sensors/mosense/0/status"

static const char *TAG = "MO_SENSE";
static bool wifi_connected = false;
static bool mosenseEnabled = false;

static bool changeCheck = true;

static esp_mqtt_client_handle_t mqtt_client;

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
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}


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
                        mosenseEnabled = cJSON_IsTrue(enable);
                        ESP_LOGI(TAG, "MOSENSE %s (JSON)",
                        mosenseEnabled ? "ENABLED" : "DISABLED");
                        cJSON_Delete(root);
                        break;

                    }
                    cJSON_Delete(root);
                }
                if (strncmp(event->data, "ON", event->data_len) == 0)
                {
                    mosenseEnabled = true;
                    
                }
                else if (strncmp(event->data, "OFF", event->data_len) == 0)
                {
                    mosenseEnabled = false;
                }
                else 
                {
                    ESP_LOGW(TAG, "Uknown payload -> Unable to identify");

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

void print_ip(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "ESP32 IP: " IPSTR, IP2STR(&ip.ip));
    }
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

static void sensor_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SENPIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    gpio_config(&io);
}

static void fan_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << FANPIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    gpio_set_level(FANPIN, 0);
}

static void LED_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LEDPIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    gpio_set_level(LEDPIN, 0);
}

static void send_sensor_state(int gpio, int state)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "socket() failed");
        return;

    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(GATEWAY_PORT);
    inet_pton(AF_INET, GATEWAY_IP, &dest.sin_addr);

    if (connect(sock, (struct sockaddr*)&dest, sizeof(dest)) == 0)
    {
        char msg[64];
        snprintf(
            msg,
            sizeof(msg),
            "DEV=%d,TYPE=SENSOR,GPIO=%d,STATE=%d\n",
            DEVICE_ID,
            gpio,
            state
        );

        send(sock, msg, strlen(msg), 0);
        ESP_LOGI(TAG, "Sent: %s", msg);


    }
    else
    {
        ESP_LOGE(TAG, "TCP connect for sensor failed %d", errno);
    }
    close(sock);
}

static void sensor_task(void *arg)
{
    //delay to calibrate
    vTaskDelay(pdMS_TO_TICKS(50));
    int last_state = gpio_get_level(SENPIN);


while(1)
{
    if(mosenseEnabled)
    {

    if(changeCheck)
    {
        ESP_LOGI(TAG,"Motion Sensor Enabled");
        changeCheck = false;
    }
    
    
        int state = gpio_get_level(SENPIN);

        if (state != last_state)
        {
            if (state == 1)
            {
                ESP_LOGI("HRSR501", "MOTION DETECTED");
                gpio_set_level(FANPIN, 1);
                gpio_set_level(LEDPIN, 1);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            else
            {
                ESP_LOGI("HRSR501", "MOTION ENDED");
                gpio_set_level(FANPIN, 0);
                gpio_set_level(LEDPIN, 0);
            }
            send_sensor_state(SENPIN, state);
            last_state = state;
            
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    
    }
    else {
        if(!changeCheck)
        {
            ESP_LOGI(TAG,"Motion Sensor Disabled");
            changeCheck = true;
        }
    }
}
}




void app_main(void)
{

    ESP_LOGE(TAG, "Bootin up...");
    nvs_flash_init();
    sensor_gpio_init();
    
    LED_init();
    print_ip();

    wifi_init();

    while(!wifi_connected)
    {
        vTaskDelay(pdMS_TO_TICKS(200));

    }
    mqtt_start();

    xTaskCreate(
        sensor_task,
        "sensor_task",
        4096,
        NULL,
        5,
        NULL
    );
}
