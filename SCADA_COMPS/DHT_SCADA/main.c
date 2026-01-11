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
#include "mqtt_client.h"
#include "cJSON.h"
#include <dht.h>

/* ================= CONFIG ================= */

#define MAX_CS         GPIO_NUM_6
#define MAX_CLK        GPIO_NUM_5
#define MAX_DIN        GPIO_NUM_7

#define DHT_PIN        GPIO_NUM_13
#define DHT_TYPE       DHT_TYPE_DHT11
#define DEVICE_ID      0

#define WIFI_SSID      "@vanc1s"
#define WIFI_PASS      "ynot21769"

#define GATEWAY_IP     "192.168.1.52"   // Linux outstation IP
#define GATEWAY_PORT   9100            // TCP listener port
#define DEVICE_ID 0

#define MQTT_URI       "mqtt://192.168.1.52:1885"
#define MQTT_SUB_TOPIC "sensors/dht/0/cmd"
#define MQTT_PUB_TOPIC "sensors/dht/0/status"

static volatile bool enabCheck = false;


static bool wifi_connected = false;

static volatile bool dht_enabled = false;

static esp_mqtt_client_handle_t mqtt_client;



static void send_max(uint8_t reg, uint8_t data)
{
    gpio_set_level(MAX_CS, 0);

    for (int i = 7; i >= 0; i--){
        gpio_set_level(MAX_CLK, 0);
        gpio_set_level(MAX_DIN, (reg >> i) & 1);
        gpio_set_level(MAX_CLK, 1);
    }

    for (int i = 7; i >= 0; i--){
        gpio_set_level(MAX_CLK, 0);
        gpio_set_level(MAX_DIN, (data >> i) & 1);
        gpio_set_level(MAX_CLK, 1);
    }

    gpio_set_level(MAX_CS, 1);
}

static void max_init(void)
{
    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = 
            (1ULL << MAX_DIN) | 
            (1ULL << MAX_CLK) |
            (1ULL << MAX_CS),
    };

    gpio_config(&io);

    gpio_set_level(MAX_CS, 1);
    gpio_set_level(MAX_CLK, 0);

    send_max(0x0F, 0x00);
    send_max(0x0C, 0x01);
    send_max(0x0B, 0x07);
    send_max(0x09, 0x00);
    send_max(0x0A, 0x08);
}

static void clear_max(void)
{
    for (int row = 1; row <= 8; row++){
        send_max(row, 0x00);
    }
}

static void max_on(void)
{
    const uint8_t rows[8] = {
        0b00000111, //  ****
        0b00000101, // *    *
        0b00000111, // *    *
        0b00000000, // *    *
        0b00000111, // *  * *
        0b00000001, // * * **
        0b00000111, // **   *
        0b00000000  //  ****
    };

    for (int r = 0; r < 8; r++) {
        send_max(r + 1, rows[r]);
    }
}

static void max_off(void)
{
    const uint8_t rows[8] = {
        0b00000111, //  ****
        0b00000101, // *    *
        0b00000111, // *    *
        0b00000111, // *    *
        0b00000011, // * ****
        0b00000111, // *
        0b00000011, // *
        0b00000000  //  ****
    };

    for (int r = 0; r < 8; r++) {
        send_max(r + 1, rows[r]);
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
                        dht_enabled = cJSON_IsTrue(enable);

                        if (dht_enabled) {
                            max_on();
                        } else {
                            max_off();
                        }
                        ESP_LOGI("DHT", "DHT %s (JSON)",
                        dht_enabled ? "ENABLED" : "DISABLED");
                        cJSON_Delete(root);
                        break;

                    }
                    cJSON_Delete(root);
                }
                if (strncmp(event->data, "ON", event->data_len) == 0)
                {
                    dht_enabled = true;
                    
                }
                else if (strncmp(event->data, "OFF", event->data_len) == 0)
                {
                    dht_enabled = false;
                }
                else 
                {
                    ESP_LOGW("DHT", "Uknown payload -> Unable to identify");

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





//WIFI INIT

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

void print_ip(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        ESP_LOGI("IP", "ESP32 IP: " IPSTR, IP2STR(&ip.ip));
    }
}

static void dht_tcp_task(void *pvParameters)
{
    int16_t tempRaw = 0;
    int16_t humRaw  = 0;

    while (1)
    {
        if (dht_enabled)
        {
            if(enabCheck) {
                ESP_LOGI(TAG, "DHT ENABLED");
                enabCheck = false;
            }
            if (dht_read_data(DHT_TYPE, DHT_PIN, &humRaw, &tempRaw) == ESP_OK)
            {
                float temp = tempRaw / 10.0f;
                float hum  = humRaw  / 10.0f;

                ESP_LOGI("DHT", "Temp=%.1f C  Hum=%.1f %%", temp, hum);

                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock >= 0)
                {
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
                                 "DEV=%d,TYPE=ENV,TEMP=%.1f,HUM=%.1f\n",
                                 DEVICE_ID, temp, hum);

                        send(sock, msg, strlen(msg), 0);
                    }
                    else
                    {
                        ESP_LOGW("TCP", "Connect failed: %d", errno);
                    }

                    close(sock);
                }
            }
            else
            {
                ESP_LOGW("DHT", "Read failed");
            }
        } else {
            if(!enabCheck) {
                ESP_LOGI(TAG, "DHT DISABLED");
                enabCheck = true;
            }
        }

        // IMPORTANT: slow this task down
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ================= MAIN ================= */

void app_main(void)
{

    

    nvs_flash_init();
    wifi_init();
    print_ip();
   
    /*{
        puts("HELLO FROM ESP32");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }*/
    
    //Wait for REAL Wi-Fi connection 
    while (!wifi_connected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    max_init();
    max_off();
    mqtt_start();

    printf("WiFi connected, starting sensor loop\n");

    xTaskCreate(dht_tcp_task,
                "dht_tcp_task",
                4096,
                NULL,
                5,
                NULL
    );
    
}
