#include "bike_history.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BIKE_HISTORY_PATH "/spiffs/bike_history.bin"
#define BIKE_HISTORY_MAGIC 0x31484c42u
#define BIKE_HISTORY_HEADER_SIZE 16
#define BIKE_HISTORY_RECORD_SIZE 10
#define BIKE_HISTORY_DATA_BYTES (64 * 1024)
#define BIKE_HISTORY_CAPACITY_BYTES \
    ((BIKE_HISTORY_DATA_BYTES / BIKE_HISTORY_RECORD_SIZE) * BIKE_HISTORY_RECORD_SIZE)
#define BIKE_HISTORY_MAX_RECORDS (BIKE_HISTORY_CAPACITY_BYTES / BIKE_HISTORY_RECORD_SIZE)
#define BIKE_HISTORY_FIELD_EXPORT_MAX_POINTS 720

typedef struct {
    uint32_t magic;
    uint32_t write_offset;
    uint32_t used_bytes;
    uint32_t reserved;
} bike_history_header_t;

typedef struct {
    uint32_t unix_time;
    int32_t value;
    uint8_t value_type;
} bike_history_point_t;

static const char *TAG = "bike_history";
static SemaphoreHandle_t history_mutex;
static bool history_ready;

static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
    dst[2] = (uint8_t)((value >> 16) & 0xff);
    dst[3] = (uint8_t)((value >> 24) & 0xff);
}

static uint32_t get_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static void header_to_bytes(const bike_history_header_t *header, uint8_t *bytes)
{
    put_u32_le(bytes, header->magic);
    put_u32_le(bytes + 4, header->write_offset);
    put_u32_le(bytes + 8, header->used_bytes);
    put_u32_le(bytes + 12, header->reserved);
}

static void bytes_to_header(const uint8_t *bytes, bike_history_header_t *header)
{
    header->magic = get_u32_le(bytes);
    header->write_offset = get_u32_le(bytes + 4);
    header->used_bytes = get_u32_le(bytes + 8);
    header->reserved = get_u32_le(bytes + 12);
}

static bike_history_header_t empty_header(void)
{
    return (bike_history_header_t){
        .magic = BIKE_HISTORY_MAGIC,
        .write_offset = 0,
        .used_bytes = 0,
        .reserved = 0,
    };
}

static bool header_valid(const bike_history_header_t *header)
{
    return header->magic == BIKE_HISTORY_MAGIC &&
           header->write_offset < BIKE_HISTORY_CAPACITY_BYTES &&
           header->used_bytes <= BIKE_HISTORY_CAPACITY_BYTES &&
           (header->write_offset % BIKE_HISTORY_RECORD_SIZE) == 0 &&
           (header->used_bytes % BIKE_HISTORY_RECORD_SIZE) == 0;
}

static esp_err_t write_header(FILE *file, const bike_history_header_t *header)
{
    uint8_t bytes[BIKE_HISTORY_HEADER_SIZE];
    header_to_bytes(header, bytes);
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fwrite(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) {
        return ESP_FAIL;
    }
    fflush(file);
    return ESP_OK;
}

static esp_err_t read_header(FILE *file, bike_history_header_t *header)
{
    uint8_t bytes[BIKE_HISTORY_HEADER_SIZE];
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) {
        return ESP_FAIL;
    }
    bytes_to_header(bytes, header);
    return header_valid(header) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t create_empty_file(void)
{
    FILE *file = fopen(BIKE_HISTORY_PATH, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    bike_history_header_t header = empty_header();
    esp_err_t err = write_header(file, &header);
    fclose(file);
    return err;
}

static size_t base64_encoded_len(size_t raw_len)
{
    return ((raw_len + 2) / 3) * 4;
}

static bool base64_encode(char *out, size_t out_len, const uint8_t *raw, size_t raw_len)
{
    size_t encoded_len = base64_encoded_len(raw_len);
    if (out_len <= encoded_len) {
        return false;
    }

    size_t oi = 0;
    for (size_t i = 0; i < raw_len; i += 3) {
        uint32_t triple = ((uint32_t)raw[i]) << 16;
        bool has_b = i + 1 < raw_len;
        bool has_c = i + 2 < raw_len;
        if (has_b) {
            triple |= ((uint32_t)raw[i + 1]) << 8;
        }
        if (has_c) {
            triple |= raw[i + 2];
        }

        out[oi++] = base64_table[(triple >> 18) & 0x3f];
        out[oi++] = base64_table[(triple >> 12) & 0x3f];
        out[oi++] = has_b ? base64_table[(triple >> 6) & 0x3f] : '=';
        out[oi++] = has_c ? base64_table[triple & 0x3f] : '=';
    }
    out[oi] = '\0';
    return true;
}

static bool append_text(char *out, size_t out_len, const char *fmt, ...)
{
    size_t used = strlen(out);
    if (used + 1 >= out_len) {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out + used, out_len - used, fmt, args);
    va_end(args);
    return written >= 0 && (size_t)written < out_len - used;
}

static esp_err_t read_history_bytes(FILE *file, const bike_history_header_t *header,
                                    uint8_t *raw, size_t raw_len)
{
    if (raw_len < header->used_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (header->used_bytes == 0) {
        return ESP_OK;
    }

    uint32_t oldest = header->used_bytes == BIKE_HISTORY_CAPACITY_BYTES ?
                      header->write_offset : 0;
    uint32_t first_len = header->used_bytes;
    if (oldest + first_len > BIKE_HISTORY_CAPACITY_BYTES) {
        first_len = BIKE_HISTORY_CAPACITY_BYTES - oldest;
    }

    if (fseek(file, BIKE_HISTORY_HEADER_SIZE + oldest, SEEK_SET) != 0 ||
        fread(raw, 1, first_len, file) != first_len) {
        return ESP_FAIL;
    }
    if (first_len < header->used_bytes) {
        uint32_t second_len = header->used_bytes - first_len;
        if (fseek(file, BIKE_HISTORY_HEADER_SIZE, SEEK_SET) != 0 ||
            fread(raw + first_len, 1, second_len, file) != second_len) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t bike_history_init(void)
{
    if (history_mutex == NULL) {
        history_mutex = xSemaphoreCreateMutex();
        if (history_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    FILE *file = fopen(BIKE_HISTORY_PATH, "rb");
    if (file == NULL) {
        esp_err_t err = create_empty_file();
        history_ready = err == ESP_OK;
        return err;
    }

    bike_history_header_t header;
    esp_err_t err = read_header(file, &header);
    fclose(file);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "resetting invalid bike history ring");
        err = create_empty_file();
    }
    history_ready = err == ESP_OK;
    return err;
}

esp_err_t bike_history_append(uint8_t field_id, uint8_t value_type,
                              int32_t value, uint32_t unix_time)
{
    if (!history_ready || field_id == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(history_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    FILE *file = fopen(BIKE_HISTORY_PATH, "r+b");
    if (file == NULL) {
        xSemaphoreGive(history_mutex);
        return ESP_FAIL;
    }

    bike_history_header_t header;
    esp_err_t err = read_header(file, &header);
    if (err == ESP_OK) {
        uint8_t record[BIKE_HISTORY_RECORD_SIZE];
        put_u32_le(record, unix_time);
        put_u32_le(record + 4, (uint32_t)value);
        record[8] = field_id;
        record[9] = value_type;

        if (fseek(file, BIKE_HISTORY_HEADER_SIZE + header.write_offset, SEEK_SET) != 0 ||
            fwrite(record, 1, sizeof(record), file) != sizeof(record)) {
            err = ESP_FAIL;
        } else {
            header.write_offset += BIKE_HISTORY_RECORD_SIZE;
            if (header.write_offset >= BIKE_HISTORY_CAPACITY_BYTES) {
                header.write_offset = 0;
            }
            if (header.used_bytes < BIKE_HISTORY_CAPACITY_BYTES) {
                header.used_bytes += BIKE_HISTORY_RECORD_SIZE;
            }
            err = write_header(file, &header);
        }
    }

    fclose(file);
    xSemaphoreGive(history_mutex);
    return err;
}

bool bike_history_has_records(void)
{
    if (!history_ready || xSemaphoreTake(history_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    FILE *file = fopen(BIKE_HISTORY_PATH, "rb");
    if (file == NULL) {
        xSemaphoreGive(history_mutex);
        return false;
    }
    bike_history_header_t header;
    bool has_records = read_header(file, &header) == ESP_OK && header.used_bytes > 0;
    fclose(file);
    xSemaphoreGive(history_mutex);
    return has_records;
}

esp_err_t bike_history_export_json(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (!history_ready || xSemaphoreTake(history_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(BIKE_HISTORY_PATH, "rb");
    if (file == NULL) {
        xSemaphoreGive(history_mutex);
        return ESP_FAIL;
    }

    bike_history_header_t header;
    esp_err_t err = read_header(file, &header);
    if (err != ESP_OK) {
        fclose(file);
        xSemaphoreGive(history_mutex);
        return err;
    }

    uint8_t *raw = NULL;
    char *encoded = NULL;
    size_t encoded_len = base64_encoded_len(header.used_bytes);
    if (header.used_bytes > 0) {
        raw = malloc(header.used_bytes);
        encoded = malloc(encoded_len + 1);
        if (raw == NULL || encoded == NULL) {
            free(raw);
            free(encoded);
            fclose(file);
            xSemaphoreGive(history_mutex);
            return ESP_ERR_NO_MEM;
        }
        err = read_history_bytes(file, &header, raw, header.used_bytes);
        if (err == ESP_OK && !base64_encode(encoded, encoded_len + 1, raw, header.used_bytes)) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    fclose(file);
    xSemaphoreGive(history_mutex);

    if (err == ESP_OK) {
        int written = snprintf(out, out_len,
                               "{\"format\":\"boschldi-bike-history-v1\","
                               "\"record_size\":%u,\"record_count\":%u,"
                               "\"field_map\":\"1=time,2=speed_centi_kmh,3=cadence_rpm,"
                               "4=rider_power_w,5=ambient_millilux,6=battery_soc,"
                               "7=odometer_m,8=bike_light,9=system_locked,"
                               "10=charger_connected,11=light_reserve_state,"
                               "12=diagnosis_active,13=bike_not_driving\","
                               "\"value_types\":\"1=u32,2=i32,3=bool\","
                               "\"data_base64\":\"%s\"}",
                               BIKE_HISTORY_RECORD_SIZE,
                               (unsigned)(header.used_bytes / BIKE_HISTORY_RECORD_SIZE),
                               encoded != NULL ? encoded : "");
        if (written < 0 || (size_t)written >= out_len) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }

    free(raw);
    free(encoded);
    return err;
}

esp_err_t bike_history_field_json(uint8_t field_id, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0 || field_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    bike_history_point_t *points = calloc(BIKE_HISTORY_FIELD_EXPORT_MAX_POINTS,
                                          sizeof(*points));
    if (points == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (!history_ready || xSemaphoreTake(history_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        free(points);
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(BIKE_HISTORY_PATH, "rb");
    if (file == NULL) {
        xSemaphoreGive(history_mutex);
        free(points);
        return ESP_FAIL;
    }

    bike_history_header_t header;
    esp_err_t err = read_header(file, &header);
    uint32_t total_matching = 0;
    if (err == ESP_OK) {
        uint32_t record_count = header.used_bytes / BIKE_HISTORY_RECORD_SIZE;
        uint32_t oldest = header.used_bytes == BIKE_HISTORY_CAPACITY_BYTES ?
                          header.write_offset : 0;
        for (uint32_t i = 0; i < record_count; i++) {
            uint32_t offset = oldest + (i * BIKE_HISTORY_RECORD_SIZE);
            if (offset >= BIKE_HISTORY_CAPACITY_BYTES) {
                offset -= BIKE_HISTORY_CAPACITY_BYTES;
            }

            uint8_t record[BIKE_HISTORY_RECORD_SIZE];
            if (fseek(file, BIKE_HISTORY_HEADER_SIZE + offset, SEEK_SET) != 0 ||
                fread(record, 1, sizeof(record), file) != sizeof(record)) {
                err = ESP_FAIL;
                break;
            }
            if (record[8] != field_id) {
                continue;
            }

            uint32_t slot = total_matching % BIKE_HISTORY_FIELD_EXPORT_MAX_POINTS;
            points[slot].unix_time = get_u32_le(record);
            points[slot].value = (int32_t)get_u32_le(record + 4);
            points[slot].value_type = record[9];
            total_matching++;
        }
    }

    fclose(file);
    xSemaphoreGive(history_mutex);
    if (err != ESP_OK) {
        free(points);
        return err;
    }

    uint32_t emitted = total_matching > BIKE_HISTORY_FIELD_EXPORT_MAX_POINTS ?
                       BIKE_HISTORY_FIELD_EXPORT_MAX_POINTS : total_matching;
    uint32_t start = total_matching > BIKE_HISTORY_FIELD_EXPORT_MAX_POINTS ?
                     total_matching % BIKE_HISTORY_FIELD_EXPORT_MAX_POINTS : 0;

    bool ok = append_text(out, out_len,
                          "{\"format\":\"boschldi-bike-history-field-v1\","
                          "\"field_id\":%u,\"record_count\":%u,"
                          "\"total_matching\":%u,\"max_points\":%u,\"records\":[",
                          (unsigned)field_id, (unsigned)emitted,
                          (unsigned)total_matching,
                          (unsigned)BIKE_HISTORY_FIELD_EXPORT_MAX_POINTS);
    for (uint32_t i = 0; ok && i < emitted; i++) {
        uint32_t slot = (start + i) % BIKE_HISTORY_FIELD_EXPORT_MAX_POINTS;
        ok = append_text(out, out_len, "%s[%u,%ld,%u]",
                         i == 0 ? "" : ",",
                         (unsigned)points[slot].unix_time,
                         (long)points[slot].value,
                         (unsigned)points[slot].value_type);
    }
    if (ok) {
        ok = append_text(out, out_len, "]}");
    }

    free(points);
    return ok ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t bike_history_clear(void)
{
    if (!history_ready || xSemaphoreTake(history_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = create_empty_file();
    xSemaphoreGive(history_mutex);
    return err;
}
