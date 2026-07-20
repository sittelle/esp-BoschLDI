#include "telemetry_export.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accessory_config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "live_data_decode.h"
#include "log_store.h"
#include "persistent_log.h"
#include "wifi_admin.h"

#define EXPORT_POLL_INTERVAL_MS 5000
#define EXPORT_LOG_BUFFER_SIZE 8192
#define EXPORT_JSON_BUFFER_SIZE 20000
#define EXPORT_BIKE_JSON_SIZE 1536
#define EXPORT_HTTP_TIMEOUT_MS 3000

static const char *TAG = "telemetry_export";
static bool exporter_started;

static void json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src != NULL && src[si] != '\0' && di + 1 < dst_len; si++) {
        unsigned char c = (unsigned char)src[si];
        if ((c == '"' || c == '\\') && di + 2 < dst_len) {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c == '\n' && di + 2 < dst_len) {
            dst[di++] = '\\';
            dst[di++] = 'n';
        } else if (c == '\r' && di + 2 < dst_len) {
            dst[di++] = '\\';
            dst[di++] = 'r';
        } else if (c == '\t' && di + 2 < dst_len) {
            dst[di++] = '\\';
            dst[di++] = 't';
        } else if (c >= 0x20) {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

static esp_err_t post_json(const char *url, const char *payload)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = EXPORT_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && (status < 200 || status >= 300)) {
        err = ESP_FAIL;
    }
    return err;
}

static void export_logs(const accessory_export_config_t *config, const char *device_name)
{
    char *logs = malloc(EXPORT_LOG_BUFFER_SIZE);
    char *escaped_logs = malloc(EXPORT_JSON_BUFFER_SIZE);
    char *payload = malloc(EXPORT_JSON_BUFFER_SIZE);
    if (logs == NULL || escaped_logs == NULL || payload == NULL) {
        free(logs);
        free(escaped_logs);
        free(payload);
        return;
    }

    log_store_copy(logs, EXPORT_LOG_BUFFER_SIZE);
    json_escape(escaped_logs, EXPORT_JSON_BUFFER_SIZE, logs);

    snprintf(payload, EXPORT_JSON_BUFFER_SIZE,
             "{\"type\":\"logs\",\"device_name\":\"%s\",\"boot_ms\":%lld,"
             "\"interval_sec\":%" PRIu32 ",\"logs\":\"%s\"}",
             device_name, (long long)(esp_timer_get_time() / 1000),
             config->logs_interval_sec, escaped_logs);

    esp_err_t err = post_json(config->logs_url, payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "log export failed; err=%s", esp_err_to_name(err));
    }

    free(logs);
    free(escaped_logs);
    free(payload);
}

static void export_bike_data(const accessory_export_config_t *config, const char *device_name)
{
    char bike_json[EXPORT_BIKE_JSON_SIZE];
    bool has_data = live_data_latest_json(bike_json, sizeof(bike_json));
    char payload[EXPORT_BIKE_JSON_SIZE + 256];

    snprintf(payload, sizeof(payload),
             "{\"type\":\"bike_data\",\"device_name\":\"%s\",\"boot_ms\":%lld,"
             "\"interval_sec\":%" PRIu32 ",\"has_data\":%s,\"data\":%s}",
             device_name, (long long)(esp_timer_get_time() / 1000),
             config->bike_interval_sec, has_data ? "true" : "false",
             has_data ? bike_json : "{}");

    esp_err_t err = post_json(config->bike_url, payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bike data export failed; err=%s", esp_err_to_name(err));
    }
}

static void telemetry_export_task(void *arg)
{
    (void)arg;
    int64_t last_logs_us = 0;
    int64_t last_bike_us = 0;

    while (true) {
        accessory_export_config_t config;
        char device_name[ACCESSORY_DEVICE_NAME_MAX_LEN + 1];
        accessory_config_load_export(&config);
        accessory_config_load_device_name(device_name, sizeof(device_name));

        int64_t now = esp_timer_get_time();
        if (!wifi_admin_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(EXPORT_POLL_INTERVAL_MS));
            continue;
        }

        if (config.logs_url[0] != '\0' &&
            (last_logs_us == 0 ||
             now - last_logs_us >= (int64_t)config.logs_interval_sec * 1000000LL)) {
            last_logs_us = now;
            export_logs(&config, device_name);
        }

        if (config.bike_url[0] != '\0' &&
            (last_bike_us == 0 ||
             now - last_bike_us >= (int64_t)config.bike_interval_sec * 1000000LL)) {
            last_bike_us = now;
            export_bike_data(&config, device_name);
        }

        vTaskDelay(pdMS_TO_TICKS(EXPORT_POLL_INTERVAL_MS));
    }
}

esp_err_t telemetry_export_start(void)
{
    if (exporter_started) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(telemetry_export_task, "telemetry_export",
                                6144, NULL, 2, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    exporter_started = true;
    persistent_log_event("info", "telemetry", "telemetry exporter started");
    return ESP_OK;
}
