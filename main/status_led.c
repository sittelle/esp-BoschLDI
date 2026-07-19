#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "accessory_config.h"
#include "driver/rmt_common.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef STATUS_LED_GPIO
#define STATUS_LED_GPIO 48
#endif

#ifndef STATUS_LED_GPIO_FALLBACK_1
#define STATUS_LED_GPIO_FALLBACK_1 38
#endif

#ifndef STATUS_LED_GPIO_FALLBACK_2
#define STATUS_LED_GPIO_FALLBACK_2 21
#endif

#ifndef STATUS_LED_GPIO_FALLBACK_3
#define STATUS_LED_GPIO_FALLBACK_3 47
#endif

#define STATUS_LED_CHANNEL_COUNT 4

static const char *TAG = "status_led";
static const int led_gpios[STATUS_LED_CHANNEL_COUNT] = {
    STATUS_LED_GPIO,
    STATUS_LED_GPIO_FALLBACK_1,
    STATUS_LED_GPIO_FALLBACK_2,
    STATUS_LED_GPIO_FALLBACK_3,
};
static rmt_channel_handle_t led_channels[STATUS_LED_CHANNEL_COUNT];
static rmt_encoder_handle_t led_encoders[STATUS_LED_CHANNEL_COUNT];
static bool led_ready;
static volatile status_led_state_t current_state = STATUS_LED_BOOT;
static volatile int activity_ticks_remaining;
static accessory_led_config_t led_config;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} rgb_t;

static rgb_t state_color(status_led_state_t state)
{
    uint32_t color;
    switch (state) {
    case STATUS_LED_BOOT:
        color = led_config.boot_color;
        break;
    case STATUS_LED_ADVERTISING:
        color = led_config.advertising_color;
        break;
    case STATUS_LED_CONNECTED:
        color = led_config.connected_color;
        break;
    case STATUS_LED_SECURED:
        color = led_config.secured_color;
        break;
    case STATUS_LED_READY:
        color = led_config.ready_color;
        break;
    case STATUS_LED_ACTIVITY:
        color = led_config.activity_color;
        break;
    case STATUS_LED_ERROR:
    default:
        color = led_config.error_color;
        break;
    }

    uint32_t brightness = led_config.brightness_percent;
    return (rgb_t){
        .red = (uint8_t)((((color >> 16) & 0xff) * brightness) / 100),
        .green = (uint8_t)((((color >> 8) & 0xff) * brightness) / 100),
        .blue = (uint8_t)(((color & 0xff) * brightness) / 100),
    };
}

static void write_color(rgb_t color)
{
    if (!led_ready) {
        return;
    }
    if (!led_config.enabled) {
        color = (rgb_t){0, 0, 0};
    }

    const uint8_t grb[3] = {color.green, color.red, color.blue};
    const rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    for (size_t i = 0; i < STATUS_LED_CHANNEL_COUNT; i++) {
        if (led_channels[i] == NULL || led_encoders[i] == NULL) {
            continue;
        }

        rmt_encoder_reset(led_encoders[i]);
        esp_err_t err = rmt_transmit(led_channels[i], led_encoders[i], grb, sizeof(grb), &tx_config);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "LED GPIO %d update failed; err=%s",
                     led_gpios[i], esp_err_to_name(err));
            continue;
        }
        err = rmt_tx_wait_all_done(led_channels[i], 20);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "LED GPIO %d update timeout; err=%s",
                     led_gpios[i], esp_err_to_name(err));
        }
    }
}

static void led_task(void *arg)
{
    (void)arg;
    const rgb_t off = {0, 0, 0};

    write_color((rgb_t){128, 0, 0});
    vTaskDelay(pdMS_TO_TICKS(300));
    write_color((rgb_t){0, 128, 0});
    vTaskDelay(pdMS_TO_TICKS(300));
    write_color((rgb_t){0, 0, 128});
    vTaskDelay(pdMS_TO_TICKS(300));
    write_color(off);
    vTaskDelay(pdMS_TO_TICKS(120));

    uint32_t tick = 0;
    while (true) {
        status_led_state_t state = current_state;
        bool on = true;

        if (activity_ticks_remaining > 0) {
            activity_ticks_remaining--;
            write_color(state_color(STATUS_LED_ACTIVITY));
            vTaskDelay(pdMS_TO_TICKS(100));
            tick++;
            continue;
        }

        switch (state) {
        case STATUS_LED_BOOT:
            on = (tick % 2) == 0;
            break;
        case STATUS_LED_ADVERTISING:
            on = (tick % 4) < 2;
            break;
        case STATUS_LED_CONNECTED:
            on = (tick % 6) < 3;
            break;
        case STATUS_LED_SECURED:
            on = (tick % 8) < 6;
            break;
        case STATUS_LED_READY:
            on = (tick % 20) != 0;
            break;
        case STATUS_LED_ERROR:
            on = (tick % 2) == 0;
            break;
        case STATUS_LED_ACTIVITY:
        default:
            on = true;
            break;
        }

        write_color(on ? state_color(state) : off);
        vTaskDelay(pdMS_TO_TICKS(250));
        tick++;
    }
}

esp_err_t status_led_init(void)
{
    size_t initialized = 0;
    accessory_config_load_led(&led_config);

    for (size_t i = 0; i < STATUS_LED_CHANNEL_COUNT; i++) {
        rmt_tx_channel_config_t channel_config = {
            .gpio_num = led_gpios[i],
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10000000,
            .mem_block_symbols = 64,
            .trans_queue_depth = 1,
        };

        esp_err_t err = rmt_new_tx_channel(&channel_config, &led_channels[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RMT channel init failed for GPIO %d; err=%s",
                     led_gpios[i], esp_err_to_name(err));
            continue;
        }

        rmt_bytes_encoder_config_t encoder_config = {
            .bit0 = {
                .level0 = 1,
                .duration0 = 4,
                .level1 = 0,
                .duration1 = 8,
            },
            .bit1 = {
                .level0 = 1,
                .duration0 = 8,
                .level1 = 0,
                .duration1 = 4,
            },
            .flags.msb_first = 1,
        };
        err = rmt_new_bytes_encoder(&encoder_config, &led_encoders[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RMT encoder init failed for GPIO %d; err=%s",
                     led_gpios[i], esp_err_to_name(err));
            continue;
        }

        err = rmt_enable(led_channels[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RMT enable failed for GPIO %d; err=%s",
                     led_gpios[i], esp_err_to_name(err));
            continue;
        }
        initialized++;
    }

    if (initialized == 0) {
        ESP_LOGE(TAG, "no RGB LED RMT channels initialized");
        return ESP_FAIL;
    }

    led_ready = true;
    BaseType_t task_ok = xTaskCreate(led_task, "status_led", 2048, NULL, 4, NULL);
    if (task_ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "RGB status LED initialized on %u candidate GPIOs", initialized);
    return ESP_OK;
}

void status_led_set(status_led_state_t state)
{
    if (!led_ready) {
        return;
    }

    if (state == STATUS_LED_ACTIVITY) {
        activity_ticks_remaining = 2;
        return;
    }
    current_state = state;
}

void status_led_reload_config(void)
{
    accessory_led_config_t next_config;
    if (accessory_config_load_led(&next_config) == ESP_OK) {
        led_config = next_config;
    }
}
