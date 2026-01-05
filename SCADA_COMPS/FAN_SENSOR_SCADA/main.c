
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

static const char *TAG = "ESP32";
static bool wifi_connected = false;


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
    vTaskDelay(pdMS_TO_TICKS(15000));
    int last_state = gpio_get_level(SENPIN);

    while(1)
    {
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
}


void app_main(void)
{

    ESP_LOGE(TAG, "Bootin up...");
    nvs_flash_init();
    sensor_gpio_init();
    fan_init();
    LED_init();

    wifi_init();

    while(!wifi_connected)
    {
        vTaskDelay(pdMS_TO_TICKS(200));

    }

    xTaskCreate(
        sensor_task,
        "sensor_task",
        4096,
        NULL,
        5,
        NULL
    );
}
