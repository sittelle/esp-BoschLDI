#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ACCESSORY_DEVICE_NAME_DEFAULT "ESP32 Bosch LDI"
#define ACCESSORY_DEVICE_NAME_MAX_LEN 24
#define ACCESSORY_EXPORT_URL_MAX_LEN 128
#define ACCESSORY_EXPORT_LOG_MIN_INTERVAL_SEC 60
#define ACCESSORY_EXPORT_BIKE_MIN_INTERVAL_SEC 10
#define ACCESSORY_EXPORT_MAX_INTERVAL_SEC 3600
#define ACCESSORY_HUE_HOST_MAX_LEN 63
#define ACCESSORY_HUE_BRIDGE_ID_MAX_LEN 63
#define ACCESSORY_HUE_BRIDGE_NAME_MAX_LEN 63
#define ACCESSORY_HUE_APP_KEY_MAX_LEN 95
#define ACCESSORY_LED_BRIGHTNESS_MIN 1
#define ACCESSORY_LED_BRIGHTNESS_MAX 100

typedef struct {
    char logs_url[ACCESSORY_EXPORT_URL_MAX_LEN + 1];
    char bike_url[ACCESSORY_EXPORT_URL_MAX_LEN + 1];
    uint32_t logs_interval_sec;
    uint32_t bike_interval_sec;
} accessory_export_config_t;

typedef struct {
    char bridge_host[ACCESSORY_HUE_HOST_MAX_LEN + 1];
    char bridge_id[ACCESSORY_HUE_BRIDGE_ID_MAX_LEN + 1];
    char bridge_name[ACCESSORY_HUE_BRIDGE_NAME_MAX_LEN + 1];
    char app_key[ACCESSORY_HUE_APP_KEY_MAX_LEN + 1];
} accessory_hue_config_t;

typedef struct {
    bool enabled;
    uint8_t brightness_percent;
    uint32_t boot_color;
    uint32_t advertising_color;
    uint32_t connected_color;
    uint32_t secured_color;
    uint32_t ready_color;
    uint32_t activity_color;
    uint32_t error_color;
} accessory_led_config_t;

bool accessory_config_device_name_is_valid(const char *name);
bool accessory_config_export_url_is_valid(const char *url);
uint32_t accessory_config_clamp_logs_interval(uint32_t seconds);
uint32_t accessory_config_clamp_bike_interval(uint32_t seconds);
uint8_t accessory_config_clamp_led_brightness(uint32_t percent);
esp_err_t accessory_config_load_device_name(char *out, size_t out_len);
esp_err_t accessory_config_save_device_name(const char *name);
esp_err_t accessory_config_load_export(accessory_export_config_t *out);
esp_err_t accessory_config_save_export(const accessory_export_config_t *config);
esp_err_t accessory_config_load_hue(accessory_hue_config_t *out);
esp_err_t accessory_config_save_hue(const accessory_hue_config_t *config);
esp_err_t accessory_config_clear_hue(void);
esp_err_t accessory_config_load_led(accessory_led_config_t *out);
esp_err_t accessory_config_save_led(const accessory_led_config_t *config);
