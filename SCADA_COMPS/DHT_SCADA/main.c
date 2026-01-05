#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <dht.h>

/* ================= CONFIG ================= */

#define DHT_PIN        GPIO_NUM_13
#define DHT_TYPE       DHT_TYPE_DHT11
#define DEVICE_ID      0

#define WIFI_SSID      "xxxxx"
#define WIFI_PASS      "xxxxx"

#define GATEWAY_IP     "x.x.x.x"   // Linux outstation IP
#define GATEWAY_PORT   9100            // TCP listener port
#define DEVICE_ID 0
/* ========================================= */

static bool wifi_connected = false;

/* ================= WIFI EVENT HANDLER ================= */

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

/* ================= WIFI INIT ================= */

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

/* ================= MAIN ================= */

void app_main(void)
{
    int16_t tempRaw = 0;
    int16_t humRaw  = 0;

    nvs_flash_init();
    wifi_init();
   
    /*{
        puts("HELLO FROM ESP32");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }*/
    
    //Wait for REAL Wi-Fi connection 
    while (!wifi_connected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    printf("WiFi connected, starting sensor loop\n");

    while (1)
    {
        if (dht_read_data(DHT_TYPE, DHT_PIN, &humRaw, &tempRaw) == ESP_OK)
        {
            float temp = tempRaw / 10.0f;
            float hum  = humRaw  / 10.0f;

            printf("Temp=%.1f C  Hum=%.1f %%\n", temp, hum);

            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0)
            {
                struct sockaddr_in dest = {0};
                dest.sin_family = AF_INET;
                dest.sin_port   = htons(GATEWAY_PORT);
                inet_pton(AF_INET, GATEWAY_IP, &dest.sin_addr);

                if (connect(sock, (struct sockaddr*)&dest, sizeof(dest)) == 0)
                {
                    char msg[64];
                    snprintf(msg, sizeof(msg),
                             "DEV=%d,TYPE=ENV,TEMP=%.1f,HUM=%.1f\n",
                             DEVICE_ID, temp, hum);

                    send(sock, msg, strlen(msg), 0);
                }
                else
                {
                    printf("TCP connect failed: %d\n", errno);
                }

                close(sock);
            }
        }
        else
        {
            printf("DHT read failed\n");
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
}

