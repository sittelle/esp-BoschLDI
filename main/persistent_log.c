#include "persistent_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define PERSISTENT_LOG_BASE "/spiffs"
#define PERSISTENT_LOG_CURRENT "/spiffs/bike.log"
#define PERSISTENT_LOG_PREVIOUS "/spiffs/bike.log.1"
#define PERSISTENT_LOG_MAX_BYTES (64 * 1024)
#define PERSISTENT_BIKE_SAMPLE_INTERVAL_US (5LL * 1000LL * 1000LL)

static const char *TAG = "persistent_log";
static SemaphoreHandle_t log_mutex;
static bool mounted;
static int64_t last_bike_sample_us;

static long file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return st.st_size;
}

static void rotate_if_needed(size_t incoming_len)
{
    long current_size = file_size(PERSISTENT_LOG_CURRENT);
    if (current_size + (long)incoming_len <= PERSISTENT_LOG_MAX_BYTES) {
        return;
    }

    remove(PERSISTENT_LOG_PREVIOUS);
    rename(PERSISTENT_LOG_CURRENT, PERSISTENT_LOG_PREVIOUS);
}

static void append_line(const char *line)
{
    if (!mounted || line == NULL) {
        return;
    }

    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }

    size_t len = strlen(line);
    rotate_if_needed(len + 1);

    FILE *f = fopen(PERSISTENT_LOG_CURRENT, "a");
    if (f != NULL) {
        fwrite(line, 1, len, f);
        fputc('\n', f);
        fclose(f);
    }

    xSemaphoreGive(log_mutex);
}

esp_err_t persistent_log_init(void)
{
    if (log_mutex == NULL) {
        log_mutex = xSemaphoreCreateMutex();
        if (log_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = PERSISTENT_LOG_BASE,
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed; err=%s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info(conf.partition_label, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "persistent log storage mounted; total=%u used=%u",
                 (unsigned)total, (unsigned)used);
    }
    mounted = true;
    persistent_log_event("info", "system", "persistent log mounted total=%u used=%u",
                         (unsigned)total, (unsigned)used);
    return ESP_OK;
}

void persistent_log_event(const char *level, const char *area, const char *fmt, ...)
{
    char message[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    char line[352];
    int64_t boot_ms = esp_timer_get_time() / 1000;
    snprintf(line, sizeof(line), "boot_ms=%lld level=%s area=%s %s",
             (long long)boot_ms,
             level != NULL ? level : "info",
             area != NULL ? area : "system",
             message);
    append_line(line);
}

void persistent_log_bike_data(const char *summary)
{
    int64_t now = esp_timer_get_time();
    if (last_bike_sample_us != 0 && now - last_bike_sample_us < PERSISTENT_BIKE_SAMPLE_INTERVAL_US) {
        return;
    }
    last_bike_sample_us = now;

    persistent_log_event("data", "bike", "%s", summary != NULL ? summary : "");
}

const char *persistent_log_path_current(void)
{
    return PERSISTENT_LOG_CURRENT;
}

const char *persistent_log_path_previous(void)
{
    return PERSISTENT_LOG_PREVIOUS;
}

