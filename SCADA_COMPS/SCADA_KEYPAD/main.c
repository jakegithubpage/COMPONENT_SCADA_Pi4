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

static const char *TAG = "KEYPAD";

#define WIFI_SSID      "xxxxx"
#define WIFI_PASS      "xxxxx"

#define GATEWAY_IP     "x.x.x.x"  
#define GATEWAY_PORT   9100    

#define MQTT_URI       "mqtt://x.x.x.x:1885"
#define MQTT_SUB_TOPIC "sensors/keypad/0/cmd"
#define MQTT_PUB_TOPIC "sensors/keypad/0/status"

#define DEVICE_ID      1

/* -------------------------
 * Keypad geometry
 * ------------------------- */
#define KEYPAD_ROWS 4
#define KEYPAD_COLS 4

/* -------------------------
 * ESP32-S3 GPIO assignments
 * ------------------------- */
#define KP_R1 GPIO_NUM_4
#define KP_R2 GPIO_NUM_5
#define KP_R3 GPIO_NUM_6
#define KP_R4 GPIO_NUM_7

#define KP_C1 GPIO_NUM_8
#define KP_C2 GPIO_NUM_9
#define KP_C3 GPIO_NUM_10
#define KP_C4 GPIO_NUM_11

#define LEDG GPIO_NUM_15 
#define LEDR GPIO_NUM_16

/* -------------------------
 * Timing
 * ------------------------- */
#define KEYPAD_DEBOUNCE_MS     30
#define KEYPAD_SCAN_DELAY_US  5

//passcode and check vars
#define PASSCODE_LEN 4
static const char PASSCODE[PASSCODE_LEN + 1] = "0000";
static bool wifi_connected = false;
static bool keypadEnabled = false;
static bool changeCheck = true;
static esp_mqtt_client_handle_t mqtt_client;

//types
typedef enum {
    KEY_NONE = 0,
    KEY_1 = 1, KEY_2, KEY_3, KEY_A,
    KEY_4,     KEY_5, KEY_6, KEY_B,
    KEY_7,     KEY_8, KEY_9, KEY_C,
    KEY_STAR,  KEY_0, KEY_HASH, KEY_D
} keypad_key_t;

/* =========================================================
 * GPIO ARRAYS
 * ========================================================= */
static const gpio_num_t row_pins[KEYPAD_ROWS] = {
    KP_R1, KP_R2, KP_R3, KP_R4
};

static const gpio_num_t col_pins[KEYPAD_COLS] = {
    KP_C1, KP_C2, KP_C3, KP_C4
};

/* =========================================================
 * KEY MAPS
 * ========================================================= */
static const keypad_key_t keymap[4][4] = {
    {KEY_1, KEY_2, KEY_3, KEY_A},
    {KEY_4, KEY_5, KEY_6, KEY_B},
    {KEY_7, KEY_8, KEY_9, KEY_C},
    {KEY_STAR, KEY_0, KEY_HASH, KEY_D}
};

/* index = keypad_key_t (0..16) */
static const char key_to_char[17] = {
    '?',        // KEY_NONE
    '1','2','3','A',
    '4','5','6','B',
    '7','8','9','C',
    '*','0','#','D'
};




static void keypad_init(void)
{
    gpio_config_t io = {0};

    /* Rows: outputs */
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    io.pin_bit_mask = 0;

    for (int i = 0; i < KEYPAD_ROWS; i++) {
        io.pin_bit_mask |= (1ULL << row_pins[i]);
    }
    gpio_config(&io);

    /* Columns: inputs with pull-ups */
    io = (gpio_config_t){0};
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    io.pin_bit_mask = 0;

    for (int i = 0; i < KEYPAD_COLS; i++) {
        io.pin_bit_mask |= (1ULL << col_pins[i]);
    }
    gpio_config(&io);

    /* Idle rows HIGH */
    for (int i = 0; i < KEYPAD_ROWS; i++) {
        gpio_set_level(row_pins[i], 1);
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
                        keypadEnabled = cJSON_IsTrue(enable);
                        ESP_LOGI(TAG, "KEYPAD %s (JSON)",
                        keypadEnabled ? "ENABLED" : "DISABLED");
                        cJSON_Delete(root);
                        break;

                    }
                    cJSON_Delete(root);
                }
                if (strncmp(event->data, "ON", event->data_len) == 0)
                {
                    keypadEnabled = true;
                    
                }
                else if (strncmp(event->data, "OFF", event->data_len) == 0)
                {
                    keypadEnabled = false;
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

static keypad_key_t keypad_scan(void)
{
    for (int r = 0; r < KEYPAD_ROWS; r++) {

        gpio_set_level(row_pins[r], 0);
        esp_rom_delay_us(KEYPAD_SCAN_DELAY_US);

        for (int c = 0; c < KEYPAD_COLS; c++) {
            if (gpio_get_level(col_pins[c]) == 0) {
                gpio_set_level(row_pins[r], 1);
                return keymap[r][c];
            }
        }

        gpio_set_level(row_pins[r], 1);
    }

    return KEY_NONE;
}

static keypad_key_t keypad_getkey(void)
{
    static keypad_key_t last = KEY_NONE;
    keypad_key_t k = keypad_scan();

    if (k != KEY_NONE && k != last) {
        vTaskDelay(pdMS_TO_TICKS(KEYPAD_DEBOUNCE_MS));
        if (keypad_scan() == k) {
            last = k;
            return k;
        }
    }

    if (k == KEY_NONE) {
        last = KEY_NONE;
    }

    return KEY_NONE;
}

static void LED_init(void)
{
    gpio_config_t io = {0};

    io.mode = GPIO_MODE_OUTPUT;
    io.intr_type = GPIO_INTR_DISABLE;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pin_bit_mask = ((1ULL << LEDG) | (1ULL << LEDR));
    gpio_config(&io);

    //set defualt state
    gpio_set_level(LEDG, 0);
    gpio_set_level(LEDR, 1);

}

static void send_key(char key)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(GATEWAY_PORT);
    inet_pton(AF_INET, GATEWAY_IP, &dest.sin_addr);

    if(connect(sock, (struct sockaddr*)&dest, sizeof(dest)) == 0)
    {
        char msg[64];
        snprintf(msg, sizeof(msg),
        "DEV=%d,TYPE=KEYPAD,KEY=%c\n",
        DEVICE_ID, key);

        send(sock, msg, strlen(msg), 0);
    }
    close(sock);
}

static void send_CorInc(const char *entered, const char *passcode, int passcode_len)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(GATEWAY_PORT);
    inet_pton(AF_INET, GATEWAY_IP, &dest.sin_addr);

    if(connect(sock, (struct sockaddr*)&dest, sizeof(dest)) == 0)
    {
        if(strncmp(entered, passcode, passcode_len) == 0)
        {
            char correct[17] = "PASSWORD_CORRECT";
            send(sock, correct, strlen(correct), 0);
        }
        else
        {
            char incorrect[19] = "PASSWORD_INCORRECT";
            send(sock, incorrect, strlen(incorrect), 0);
        }
    }
    close(sock);

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
        
    }
}
static void send_keypad(void *pvParameters)
{

    char entered[PASSCODE_LEN + 1] = {0};
    int entered_len = 0;

    while (1) {

        if(keypadEnabled){
        keypad_key_t key = keypad_getkey();
        if(changeCheck == true){
            ESP_LOGI(TAG, "Keypad enabled");
            changeCheck = false;
        }
        if (key != KEY_NONE) {
            char ch = (key < 17) ? key_to_char[key] : '?';
            ESP_LOGI(TAG, "Key pressed:%c", ch);

            send_key(ch);
            /* '*' resets input */
            if (ch == '*') {
                ESP_LOGI(TAG, "Input reset");
                entered_len = 0;
                memset(entered, 0, sizeof(entered));
                continue;
            }

            /* Only digits allowed in passcode */
            if (ch == KEY_NONE) {
                continue;
            }

            /* Append safely */
            if (entered_len < PASSCODE_LEN) {
                entered[entered_len++] = ch;
            }

            /* Check passcode */
            if (entered_len == PASSCODE_LEN) {
                if (strncmp(entered, PASSCODE, PASSCODE_LEN) == 0) {
                    ESP_LOGI(TAG, "PASSCODE CORRECT");
                    send_CorInc(entered, PASSCODE, PASSCODE_LEN);
                    for (int i = 0; i < 6; i++) {
                        gpio_set_level(LEDG, 1);
                        gpio_set_level(LEDR, 0);
                        vTaskDelay(pdMS_TO_TICKS(125));
                        gpio_set_level(LEDG, 0);
                        gpio_set_level(LEDR, 0);
                        vTaskDelay(pdMS_TO_TICKS(125));
                    }

                    gpio_set_level(LEDR, 1);

                } else {
                    ESP_LOGI(TAG, "PASSCODE INCORRECT");
                    send_CorInc(entered, PASSCODE, PASSCODE_LEN);
                    for (int i = 0; i < 6; i++) {
                    gpio_set_level(LEDR, 0);
                    vTaskDelay(pdMS_TO_TICKS(125));
                    gpio_set_level(LEDR, 1);
                    vTaskDelay(pdMS_TO_TICKS(125));
                    }
                }

                entered_len = 0;
                memset(entered, 0, sizeof(entered));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
    else{
        if(changeCheck == false)
        {
            ESP_LOGI(TAG, "Keypad Disabled");
            changeCheck = true;
        }
         vTaskDelay(pdMS_TO_TICKS(100));
    }
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

/* =========================================================
 * APP MAIN
 * ========================================================= */
void app_main(void)
{
    int retries = 0;
    ESP_LOGI(TAG, "system started");
    ESP_ERROR_CHECK(nvs_flash_init()); 
    keypad_init();
    LED_init();     
    print_ip();
    wifi_init();
   
    
    while(!wifi_connected && (retries <= 50)) {
        vTaskDelay(pdMS_TO_TICKS(200));
        retries++;
    }
    mqtt_start();

    xTaskCreate(
        send_keypad,
        "send_keypad",
        4096,
        NULL,
        5,
        NULL
    );
}
