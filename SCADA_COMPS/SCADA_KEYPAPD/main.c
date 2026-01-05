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

/* =========================================================
 * GLOBAL CONFIG (must be before use)
 * ========================================================= */
static const char *TAG = "KEYPAD";

#define WIFI_SSID      "xxxxx"
#define WIFI_PASS      "xxxxx"

#define GATEWAY_IP     "x.x.x.x"  
#define GATEWAY_PORT   9100          

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

/* -------------------------
 * Passcode
 * ------------------------- */
#define PASSCODE_LEN 4
static const char PASSCODE[PASSCODE_LEN + 1] = "0000";
static bool wifi_connected = false;

/* =========================================================
 * TYPES
 * ========================================================= */
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

/* =========================================================
 * KEYPAD DRIVER
 * ========================================================= */
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
    wifi_init();
    while(!wifi_connected && (retries <= 50)) {
        vTaskDelay(pdMS_TO_TICKS(200));
        retries++;
    }
    char entered[PASSCODE_LEN + 1] = {0};
    int entered_len = 0;

    while (1) {
        keypad_key_t key = keypad_getkey();

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

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

