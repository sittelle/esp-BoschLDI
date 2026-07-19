#include "accessory_config.h"

#include <string.h>

#include "nvs.h"

#define ACCESSORY_CONFIG_NAMESPACE "accessory"
#define ACCESSORY_CONFIG_DEVICE_NAME_KEY "device_name"
#define ACCESSORY_CONFIG_LOGS_URL_KEY "logs_url"
#define ACCESSORY_CONFIG_BIKE_URL_KEY "bike_url"
#define ACCESSORY_CONFIG_LOGS_INTERVAL_KEY "logs_sec"
#define ACCESSORY_CONFIG_BIKE_INTERVAL_KEY "bike_sec"
#define ACCESSORY_CONFIG_HUE_HOST_KEY "hue_host"
#define ACCESSORY_CONFIG_HUE_BRIDGE_ID_KEY "hue_bridge"
#define ACCESSORY_CONFIG_HUE_BRIDGE_NAME_KEY "hue_name"
#define ACCESSORY_CONFIG_HUE_APP_KEY_KEY "hue_key"
#define ACCESSORY_CONFIG_LED_ENABLED_KEY "led_enabled"
#define ACCESSORY_CONFIG_LED_BRIGHTNESS_KEY "led_bright"
#define ACCESSORY_CONFIG_LED_BOOT_KEY "led_boot"
#define ACCESSORY_CONFIG_LED_ADVERTISING_KEY "led_adv"
#define ACCESSORY_CONFIG_LED_CONNECTED_KEY "led_conn"
#define ACCESSORY_CONFIG_LED_SECURED_KEY "led_sec"
#define ACCESSORY_CONFIG_LED_READY_KEY "led_ready"
#define ACCESSORY_CONFIG_LED_ACTIVITY_KEY "led_act"
#define ACCESSORY_CONFIG_LED_ERROR_KEY "led_err"

bool accessory_config_device_name_is_valid(const char *name)
{
    if (name == NULL) {
        return false;
    }

    size_t len = strlen(name);
    if (len == 0 || len > ACCESSORY_DEVICE_NAME_MAX_LEN) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

bool accessory_config_export_url_is_valid(const char *url)
{
    if (url == NULL) {
        return false;
    }
    if (url[0] == '\0') {
        return true;
    }

    size_t len = strlen(url);
    if (len > ACCESSORY_EXPORT_URL_MAX_LEN) {
        return false;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c < 0x21 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

static bool config_ascii_token_is_valid(const char *value, size_t max_len, bool allow_empty)
{
    if (value == NULL) {
        return false;
    }

    size_t len = strlen(value);
    if ((!allow_empty && len == 0) || len > max_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

static bool config_printable_ascii_is_valid(const char *value, size_t max_len, bool allow_empty)
{
    if (value == NULL) {
        return false;
    }

    size_t len = strlen(value);
    if ((!allow_empty && len == 0) || len > max_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

static uint32_t clamp_interval(uint32_t seconds, uint32_t min_seconds)
{
    if (seconds == 0) {
        return min_seconds;
    }
    if (seconds < min_seconds) {
        return min_seconds;
    }
    if (seconds > ACCESSORY_EXPORT_MAX_INTERVAL_SEC) {
        return ACCESSORY_EXPORT_MAX_INTERVAL_SEC;
    }
    return seconds;
}

uint32_t accessory_config_clamp_logs_interval(uint32_t seconds)
{
    return clamp_interval(seconds, ACCESSORY_EXPORT_LOG_MIN_INTERVAL_SEC);
}

uint32_t accessory_config_clamp_bike_interval(uint32_t seconds)
{
    return clamp_interval(seconds, ACCESSORY_EXPORT_BIKE_MIN_INTERVAL_SEC);
}

uint8_t accessory_config_clamp_led_brightness(uint32_t percent)
{
    if (percent < ACCESSORY_LED_BRIGHTNESS_MIN) {
        return ACCESSORY_LED_BRIGHTNESS_MIN;
    }
    if (percent > ACCESSORY_LED_BRIGHTNESS_MAX) {
        return ACCESSORY_LED_BRIGHTNESS_MAX;
    }
    return (uint8_t)percent;
}

esp_err_t accessory_config_load_device_name(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ACCESSORY_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        size_t len = out_len;
        err = nvs_get_str(nvs, ACCESSORY_CONFIG_DEVICE_NAME_KEY, out, &len);
        nvs_close(nvs);
        if (err == ESP_OK && accessory_config_device_name_is_valid(out)) {
            return ESP_OK;
        }
    }

    strlcpy(out, ACCESSORY_DEVICE_NAME_DEFAULT, out_len);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t accessory_config_save_device_name(const char *name)
{
    if (!accessory_config_device_name_is_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ACCESSORY_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, ACCESSORY_CONFIG_DEVICE_NAME_KEY, name);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void accessory_led_defaults(accessory_led_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = true;
    out->brightness_percent = 80;
    out->boot_color = 0x303030;
    out->advertising_color = 0x000060;
    out->connected_color = 0x603000;
    out->secured_color = 0x004848;
    out->ready_color = 0x006000;
    out->activity_color = 0x606060;
    out->error_color = 0x600000;
}

esp_err_t accessory_config_load_led(accessory_led_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    accessory_led_defaults(out);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ACCESSORY_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }

    uint8_t enabled = out->enabled ? 1 : 0;
    uint8_t brightness = out->brightness_percent;
    nvs_get_u8(nvs, ACCESSORY_CONFIG_LED_ENABLED_KEY, &enabled);
    nvs_get_u8(nvs, ACCESSORY_CONFIG_LED_BRIGHTNESS_KEY, &brightness);
    nvs_get_u32(nvs, ACCESSORY_CONFIG_LED_BOOT_KEY, &out->boot_color);
    nvs_get_u32(nvs, ACCESSORY_CONFIG_LED_ADVERTISING_KEY, &out->advertising_color);
    nvs_get_u32(nvs, ACCESSORY_CONFIG_LED_CONNECTED_KEY, &out->connected_color);
    nvs_get_u32(nvs, ACCESSORY_CONFIG_LED_SECURED_KEY, &out->secured_color);
    nvs_get_u32(nvs, ACCESSORY_CONFIG_LED_READY_KEY, &out->ready_color);
    nvs_get_u32(nvs, ACCESSORY_CONFIG_LED_ACTIVITY_KEY, &out->activity_color);
    nvs_get_u32(nvs, ACCESSORY_CONFIG_LED_ERROR_KEY, &out->error_color);
    nvs_close(nvs);

    out->enabled = enabled != 0;
    out->brightness_percent = accessory_config_clamp_led_brightness(brightness);
    out->boot_color &= 0x00ffffff;
    out->advertising_color &= 0x00ffffff;
    out->connected_color &= 0x00ffffff;
    out->secured_color &= 0x00ffffff;
    out->ready_color &= 0x00ffffff;
    out->activity_color &= 0x00ffffff;
    out->error_color &= 0x00ffffff;
    return ESP_OK;
}

esp_err_t accessory_config_save_led(const accessory_led_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ACCESSORY_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs, ACCESSORY_CONFIG_LED_ENABLED_KEY, config->enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, ACCESSORY_CONFIG_LED_BRIGHTNESS_KEY,
                         accessory_config_clamp_led_brightness(config->brightness_percent));
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, ACCESSORY_CONFIG_LED_BOOT_KEY, config->boot_color & 0x00ffffff);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, ACCESSORY_CONFIG_LED_ADVERTISING_KEY,
                          config->advertising_color & 0x00ffffff);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, ACCESSORY_CONFIG_LED_CONNECTED_KEY,
                          config->connected_color & 0x00ffffff);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, ACCESSORY_CONFIG_LED_SECURED_KEY,
                          config->secured_color & 0x00ffffff);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, ACCESSORY_CONFIG_LED_READY_KEY,
                          config->ready_color & 0x00ffffff);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, ACCESSORY_CONFIG_LED_ACTIVITY_KEY,
                          config->activity_color & 0x00ffffff);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, ACCESSORY_CONFIG_LED_ERROR_KEY,
                          config->error_color & 0x00ffffff);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void nvs_get_str_default(nvs_handle_t nvs, const char *key, char *out, size_t out_len)
{
    if (out_len == 0) {
        return;
    }
    out[0] = '\0';
    size_t len = out_len;
    if (nvs_get_str(nvs, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}

esp_err_t accessory_config_load_export(accessory_export_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->logs_interval_sec = ACCESSORY_EXPORT_LOG_MIN_INTERVAL_SEC;
    out->bike_interval_sec = ACCESSORY_EXPORT_BIKE_MIN_INTERVAL_SEC;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ACCESSORY_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }

    nvs_get_str_default(nvs, ACCESSORY_CONFIG_LOGS_URL_KEY,
                        out->logs_url, sizeof(out->logs_url));
    nvs_get_str_default(nvs, ACCESSORY_CONFIG_BIKE_URL_KEY,
                        out->bike_url, sizeof(out->bike_url));
    nvs_get_u32(nvs, ACCESSORY_CONFIG_LOGS_INTERVAL_KEY, &out->logs_interval_sec);
    nvs_get_u32(nvs, ACCESSORY_CONFIG_BIKE_INTERVAL_KEY, &out->bike_interval_sec);
    nvs_close(nvs);

    if (!accessory_config_export_url_is_valid(out->logs_url)) {
        out->logs_url[0] = '\0';
    }
    if (!accessory_config_export_url_is_valid(out->bike_url)) {
        out->bike_url[0] = '\0';
    }
    out->logs_interval_sec = accessory_config_clamp_logs_interval(out->logs_interval_sec);
    out->bike_interval_sec = accessory_config_clamp_bike_interval(out->bike_interval_sec);
    return ESP_OK;
}

esp_err_t accessory_config_save_export(const accessory_export_config_t *config)
{
    if (config == NULL ||
        !accessory_config_export_url_is_valid(config->logs_url) ||
        !accessory_config_export_url_is_valid(config->bike_url)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ACCESSORY_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, ACCESSORY_CONFIG_LOGS_URL_KEY, config->logs_url);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, ACCESSORY_CONFIG_BIKE_URL_KEY, config->bike_url);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, ACCESSORY_CONFIG_LOGS_INTERVAL_KEY,
                          accessory_config_clamp_logs_interval(config->logs_interval_sec));
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, ACCESSORY_CONFIG_BIKE_INTERVAL_KEY,
                          accessory_config_clamp_bike_interval(config->bike_interval_sec));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t accessory_config_load_hue(accessory_hue_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ACCESSORY_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }

    nvs_get_str_default(nvs, ACCESSORY_CONFIG_HUE_HOST_KEY,
                        out->bridge_host, sizeof(out->bridge_host));
    nvs_get_str_default(nvs, ACCESSORY_CONFIG_HUE_BRIDGE_ID_KEY,
                        out->bridge_id, sizeof(out->bridge_id));
    nvs_get_str_default(nvs, ACCESSORY_CONFIG_HUE_BRIDGE_NAME_KEY,
                        out->bridge_name, sizeof(out->bridge_name));
    nvs_get_str_default(nvs, ACCESSORY_CONFIG_HUE_APP_KEY_KEY,
                        out->app_key, sizeof(out->app_key));
    nvs_close(nvs);

    if (!config_ascii_token_is_valid(out->bridge_host, ACCESSORY_HUE_HOST_MAX_LEN, true)) {
        out->bridge_host[0] = '\0';
    }
    if (!config_ascii_token_is_valid(out->bridge_id, ACCESSORY_HUE_BRIDGE_ID_MAX_LEN, true)) {
        out->bridge_id[0] = '\0';
    }
    if (!config_printable_ascii_is_valid(out->bridge_name,
                                         ACCESSORY_HUE_BRIDGE_NAME_MAX_LEN, true)) {
        out->bridge_name[0] = '\0';
    }
    if (!config_ascii_token_is_valid(out->app_key, ACCESSORY_HUE_APP_KEY_MAX_LEN, true)) {
        out->app_key[0] = '\0';
    }
    return ESP_OK;
}

esp_err_t accessory_config_save_hue(const accessory_hue_config_t *config)
{
    if (config == NULL ||
        !config_ascii_token_is_valid(config->bridge_host, ACCESSORY_HUE_HOST_MAX_LEN, false) ||
        !config_ascii_token_is_valid(config->bridge_id, ACCESSORY_HUE_BRIDGE_ID_MAX_LEN, true) ||
        !config_printable_ascii_is_valid(config->bridge_name,
                                         ACCESSORY_HUE_BRIDGE_NAME_MAX_LEN, true) ||
        !config_ascii_token_is_valid(config->app_key, ACCESSORY_HUE_APP_KEY_MAX_LEN, false)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ACCESSORY_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, ACCESSORY_CONFIG_HUE_HOST_KEY, config->bridge_host);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, ACCESSORY_CONFIG_HUE_BRIDGE_ID_KEY, config->bridge_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, ACCESSORY_CONFIG_HUE_BRIDGE_NAME_KEY, config->bridge_name);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, ACCESSORY_CONFIG_HUE_APP_KEY_KEY, config->app_key);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t accessory_config_clear_hue(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ACCESSORY_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }

    esp_err_t err_host = nvs_erase_key(nvs, ACCESSORY_CONFIG_HUE_HOST_KEY);
    esp_err_t err_bridge = nvs_erase_key(nvs, ACCESSORY_CONFIG_HUE_BRIDGE_ID_KEY);
    esp_err_t err_name = nvs_erase_key(nvs, ACCESSORY_CONFIG_HUE_BRIDGE_NAME_KEY);
    esp_err_t err_key = nvs_erase_key(nvs, ACCESSORY_CONFIG_HUE_APP_KEY_KEY);
    if ((err_host == ESP_OK || err_host == ESP_ERR_NVS_NOT_FOUND) &&
        (err_bridge == ESP_OK || err_bridge == ESP_ERR_NVS_NOT_FOUND) &&
        (err_name == ESP_OK || err_name == ESP_ERR_NVS_NOT_FOUND) &&
        (err_key == ESP_OK || err_key == ESP_ERR_NVS_NOT_FOUND)) {
        err = nvs_commit(nvs);
    } else {
        err = ESP_FAIL;
    }
    nvs_close(nvs);
    return err;
}
