#include "live_data_decode.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "automation.h"
#include "bike_history.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "persistent_log.h"

static const char *TAG = "live_data";
#define LIVE_DATA_LATEST_PATH "/spiffs/latest_bike.json"
#define LIVE_DATA_LATEST_JSON_MAX_LEN 1400
#define LIVE_DATA_MAX_UNKNOWN_FIELDS 12
#define LIVE_DATA_LATEST_PERSIST_INTERVAL_US (5LL * 1000LL * 1000LL)

bool live_data_latest_json(char *out, size_t out_len);

typedef struct {
    bool present;
    uint32_t field_id;
    uint32_t wire_type;
    uint64_t value;
    uint32_t length;
} live_data_unknown_field_t;

typedef struct {
    bool has_speed;
    uint32_t speed;
    bool has_cadence;
    int32_t cadence;
    bool has_rider_power;
    uint32_t rider_power;
    bool has_ambient_brightness;
    uint32_t ambient_brightness;
    bool has_battery_soc;
    uint32_t battery_soc;
    bool has_time;
    int64_t time;
    bool has_odometer;
    uint32_t odometer;
    bool has_bike_light;
    uint32_t bike_light;
    bool has_system_locked;
    bool system_locked;
    bool has_charger_connected;
    bool charger_connected;
    bool has_light_reserve_state;
    bool light_reserve_state;
    bool has_diagnosis_program_active;
    bool diagnosis_program_active;
    bool has_bike_not_driving;
    bool bike_not_driving;
    live_data_unknown_field_t unknown[LIVE_DATA_MAX_UNKNOWN_FIELDS];
    uint8_t unknown_count;
    int64_t last_update_boot_ms;
    int64_t last_update_unix_time;
} live_data_t;

static live_data_t latest_state;
static char persisted_latest_json[LIVE_DATA_LATEST_JSON_MAX_LEN];
static bool persisted_latest_loaded;
static int64_t last_latest_persist_us;

static bool read_fixed_le(const uint8_t *buf, size_t len, size_t *pos, size_t bytes, uint64_t *value)
{
    if (len - *pos < bytes || value == NULL) {
        return false;
    }
    uint64_t result = 0;
    for (size_t i = 0; i < bytes; i++) {
        result |= ((uint64_t)buf[*pos + i]) << (8 * i);
    }
    *pos += bytes;
    *value = result;
    return true;
}

static bool read_varint(const uint8_t *buf, size_t len, size_t *pos, uint64_t *value)
{
    uint64_t result = 0;
    uint8_t shift = 0;

    while (*pos < len && shift < 64) {
        uint8_t byte = buf[(*pos)++];
        result |= ((uint64_t)(byte & 0x7f)) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            return true;
        }
        shift += 7;
    }

    return false;
}

static bool skip_field(const uint8_t *buf, size_t len, size_t *pos, uint32_t wire_type)
{
    uint64_t discard;
    switch (wire_type) {
    case 0:
        return read_varint(buf, len, pos, &discard);
    case 1:
        if (len - *pos < 8) {
            return false;
        }
        *pos += 8;
        return true;
    case 2:
        if (!read_varint(buf, len, pos, &discard) || discard > len - *pos) {
            return false;
        }
        *pos += (size_t)discard;
        return true;
    case 5:
        if (len - *pos < 4) {
            return false;
        }
        *pos += 4;
        return true;
    default:
        return false;
    }
}

static const char *light_state_name(uint32_t value)
{
    switch (value) {
    case 0:
        return "invalid";
    case 1:
        return "off";
    case 2:
        return "on";
    default:
        return "unknown";
    }
}

static void log_live_data(const live_data_t *data)
{
    if (data->has_speed) {
        ESP_LOGI(TAG, "speed=%.2f km/h", data->speed / 100.0);
    }
    if (data->has_cadence) {
        ESP_LOGI(TAG, "cadence=%" PRId32 " rpm", data->cadence);
    }
    if (data->has_rider_power) {
        ESP_LOGI(TAG, "rider_power=%" PRIu32 " W", data->rider_power);
    }
    if (data->has_ambient_brightness) {
        ESP_LOGI(TAG, "ambient_brightness=%.3f lux", data->ambient_brightness / 1000.0);
    }
    if (data->has_battery_soc) {
        ESP_LOGI(TAG, "battery_soc=%" PRIu32 " %%", data->battery_soc);
    }
    if (data->has_time) {
        ESP_LOGI(TAG, "time=%" PRId64 " unix_utc", data->time);
    }
    if (data->has_odometer) {
        ESP_LOGI(TAG, "odometer=%" PRIu32 " m", data->odometer);
    }
    if (data->has_bike_light) {
        ESP_LOGI(TAG, "bike_light=%s (%" PRIu32 ")",
                 light_state_name(data->bike_light), data->bike_light);
    }
    if (data->has_system_locked) {
        ESP_LOGI(TAG, "system_locked=%u", data->system_locked);
    }
    if (data->has_charger_connected) {
        ESP_LOGI(TAG, "charger_connected=%u", data->charger_connected);
    }
    if (data->has_light_reserve_state) {
        ESP_LOGI(TAG, "light_reserve_state=%u", data->light_reserve_state);
    }
    if (data->has_diagnosis_program_active) {
        ESP_LOGI(TAG, "diagnosis_program_active=%u", data->diagnosis_program_active);
    }
    if (data->has_bike_not_driving) {
        ESP_LOGI(TAG, "bike_not_driving=%u", data->bike_not_driving);
    }
}

static void append_field(char *out, size_t out_len, const char *fmt, ...)
{
    size_t used = strlen(out);
    if (used + 1 >= out_len) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(out + used, out_len - used, fmt, args);
    va_end(args);
}

static void upsert_unknown_field(live_data_t *data, uint32_t field_id, uint32_t wire_type,
                                 uint64_t value, uint32_t length)
{
    if (data == NULL) {
        return;
    }
    for (uint8_t i = 0; i < data->unknown_count; i++) {
        if (data->unknown[i].field_id == field_id &&
            data->unknown[i].wire_type == wire_type) {
            data->unknown[i].value = value;
            data->unknown[i].length = length;
            return;
        }
    }
    if (data->unknown_count >= LIVE_DATA_MAX_UNKNOWN_FIELDS) {
        return;
    }
    live_data_unknown_field_t *field = &data->unknown[data->unknown_count++];
    field->present = true;
    field->field_id = field_id;
    field->wire_type = wire_type;
    field->value = value;
    field->length = length;
}

static bool unknown_field_changed(const live_data_unknown_field_t *field)
{
    if (field == NULL || !field->present) {
        return false;
    }
    for (uint8_t i = 0; i < latest_state.unknown_count; i++) {
        const live_data_unknown_field_t *existing = &latest_state.unknown[i];
        if (existing->field_id == field->field_id &&
            existing->wire_type == field->wire_type) {
            return existing->value != field->value || existing->length != field->length;
        }
    }
    return true;
}

static void merge_latest_state(const live_data_t *data)
{
    if (data->has_speed) {
        latest_state.has_speed = true;
        latest_state.speed = data->speed;
    }
    if (data->has_cadence) {
        latest_state.has_cadence = true;
        latest_state.cadence = data->cadence;
    }
    if (data->has_rider_power) {
        latest_state.has_rider_power = true;
        latest_state.rider_power = data->rider_power;
    }
    if (data->has_ambient_brightness) {
        latest_state.has_ambient_brightness = true;
        latest_state.ambient_brightness = data->ambient_brightness;
    }
    if (data->has_battery_soc) {
        latest_state.has_battery_soc = true;
        latest_state.battery_soc = data->battery_soc;
    }
    if (data->has_time) {
        latest_state.has_time = true;
        latest_state.time = data->time;
    }
    if (data->has_odometer) {
        latest_state.has_odometer = true;
        latest_state.odometer = data->odometer;
    }
    if (data->has_bike_light) {
        latest_state.has_bike_light = true;
        latest_state.bike_light = data->bike_light;
    }
    if (data->has_system_locked) {
        latest_state.has_system_locked = true;
        latest_state.system_locked = data->system_locked;
    }
    if (data->has_charger_connected) {
        latest_state.has_charger_connected = true;
        latest_state.charger_connected = data->charger_connected;
    }
    if (data->has_light_reserve_state) {
        latest_state.has_light_reserve_state = true;
        latest_state.light_reserve_state = data->light_reserve_state;
    }
    if (data->has_diagnosis_program_active) {
        latest_state.has_diagnosis_program_active = true;
        latest_state.diagnosis_program_active = data->diagnosis_program_active;
    }
    if (data->has_bike_not_driving) {
        latest_state.has_bike_not_driving = true;
        latest_state.bike_not_driving = data->bike_not_driving;
    }
    for (uint8_t i = 0; i < data->unknown_count; i++) {
        upsert_unknown_field(&latest_state, data->unknown[i].field_id,
                             data->unknown[i].wire_type, data->unknown[i].value,
                             data->unknown[i].length);
    }
    latest_state.last_update_boot_ms = esp_timer_get_time() / 1000;
    if (data->has_time) {
        latest_state.last_update_unix_time = data->time;
    }
}

static uint32_t changed_field_mask(const live_data_t *data)
{
    uint32_t changed = 0;
    if (data->has_speed && (!latest_state.has_speed || latest_state.speed != data->speed)) {
        changed |= LIVE_DATA_FIELD_SPEED;
    }
    if (data->has_cadence &&
        (!latest_state.has_cadence || latest_state.cadence != data->cadence)) {
        changed |= LIVE_DATA_FIELD_CADENCE;
    }
    if (data->has_rider_power &&
        (!latest_state.has_rider_power || latest_state.rider_power != data->rider_power)) {
        changed |= LIVE_DATA_FIELD_RIDER_POWER;
    }
    if (data->has_ambient_brightness &&
        (!latest_state.has_ambient_brightness ||
         latest_state.ambient_brightness != data->ambient_brightness)) {
        changed |= LIVE_DATA_FIELD_AMBIENT_BRIGHTNESS;
    }
    if (data->has_battery_soc &&
        (!latest_state.has_battery_soc || latest_state.battery_soc != data->battery_soc)) {
        changed |= LIVE_DATA_FIELD_BATTERY_SOC;
    }
    if (data->has_odometer &&
        (!latest_state.has_odometer || latest_state.odometer != data->odometer)) {
        changed |= LIVE_DATA_FIELD_ODOMETER;
    }
    if (data->has_bike_light &&
        (!latest_state.has_bike_light || latest_state.bike_light != data->bike_light)) {
        changed |= LIVE_DATA_FIELD_BIKE_LIGHT;
    }
    if (data->has_system_locked &&
        (!latest_state.has_system_locked || latest_state.system_locked != data->system_locked)) {
        changed |= LIVE_DATA_FIELD_SYSTEM_LOCKED;
    }
    if (data->has_charger_connected &&
        (!latest_state.has_charger_connected ||
         latest_state.charger_connected != data->charger_connected)) {
        changed |= LIVE_DATA_FIELD_CHARGER_CONNECTED;
    }
    if (data->has_light_reserve_state &&
        (!latest_state.has_light_reserve_state ||
         latest_state.light_reserve_state != data->light_reserve_state)) {
        changed |= LIVE_DATA_FIELD_LIGHT_RESERVE;
    }
    if (data->has_diagnosis_program_active &&
        (!latest_state.has_diagnosis_program_active ||
         latest_state.diagnosis_program_active != data->diagnosis_program_active)) {
        changed |= LIVE_DATA_FIELD_DIAGNOSIS_ACTIVE;
    }
    if (data->has_bike_not_driving &&
        (!latest_state.has_bike_not_driving ||
         latest_state.bike_not_driving != data->bike_not_driving)) {
        changed |= LIVE_DATA_FIELD_BIKE_NOT_DRIVING;
    }
    for (uint8_t i = 0; i < data->unknown_count; i++) {
        if (unknown_field_changed(&data->unknown[i])) {
            changed |= LIVE_DATA_FIELD_UNKNOWN;
            break;
        }
    }
    return changed;
}

static void format_live_data_sample(const live_data_t *data, char *summary, size_t summary_len)
{
    summary[0] = '\0';

    if (data->has_time) {
        append_field(summary, summary_len, "time=%" PRId64 " ", data->time);
    }
    if (data->has_speed) {
        append_field(summary, summary_len, "speed_kmh=%.2f ", data->speed / 100.0);
    }
    if (data->has_cadence) {
        append_field(summary, summary_len, "cadence_rpm=%" PRId32 " ", data->cadence);
    }
    if (data->has_rider_power) {
        append_field(summary, summary_len, "rider_power_w=%" PRIu32 " ", data->rider_power);
    }
    if (data->has_battery_soc) {
        append_field(summary, summary_len, "battery_soc=%" PRIu32 " ", data->battery_soc);
    }
    if (data->has_odometer) {
        append_field(summary, summary_len, "odometer_m=%" PRIu32 " ", data->odometer);
    }
    if (data->has_bike_light) {
        append_field(summary, summary_len, "bike_light=%s ", light_state_name(data->bike_light));
    }
    if (data->has_system_locked) {
        append_field(summary, summary_len, "system_locked=%u ", data->system_locked);
    }
    if (data->has_charger_connected) {
        append_field(summary, summary_len, "charger_connected=%u ", data->charger_connected);
    }
    if (data->has_light_reserve_state) {
        append_field(summary, summary_len, "light_reserve_state=%u ", data->light_reserve_state);
    }
    if (data->has_diagnosis_program_active) {
        append_field(summary, summary_len, "diagnosis_program_active=%u ",
                     data->diagnosis_program_active);
    }
    if (data->has_bike_not_driving) {
        append_field(summary, summary_len, "bike_not_driving=%u ", data->bike_not_driving);
    }
}

static void persist_live_data_sample(const live_data_t *data)
{
    char summary[320];

    format_live_data_sample(data, summary, sizeof(summary));
    if (summary[0] != '\0') {
        persistent_log_bike_data(summary);
    }
}

static uint32_t sample_unix_time(const live_data_t *data)
{
    if (data != NULL && data->has_time && data->time > 0) {
        return (uint32_t)data->time;
    }
    if (latest_state.last_update_unix_time > 0) {
        return (uint32_t)latest_state.last_update_unix_time;
    }
    return 0;
}

static void persist_live_data_history_sample(const live_data_t *data)
{
    if (data == NULL) {
        return;
    }

    uint32_t ts = sample_unix_time(data);
    if (data->has_time) {
        bike_history_append(BIKE_HISTORY_FIELD_TIME, BIKE_HISTORY_TYPE_U32,
                            (int32_t)(uint32_t)data->time, ts);
    }
    if (data->has_speed) {
        bike_history_append(BIKE_HISTORY_FIELD_SPEED_KMH_CENTI, BIKE_HISTORY_TYPE_U32,
                            (int32_t)data->speed, ts);
    }
    if (data->has_cadence) {
        bike_history_append(BIKE_HISTORY_FIELD_CADENCE_RPM, BIKE_HISTORY_TYPE_I32,
                            data->cadence, ts);
    }
    if (data->has_rider_power) {
        bike_history_append(BIKE_HISTORY_FIELD_RIDER_POWER_W, BIKE_HISTORY_TYPE_U32,
                            (int32_t)data->rider_power, ts);
    }
    if (data->has_ambient_brightness) {
        bike_history_append(BIKE_HISTORY_FIELD_AMBIENT_BRIGHTNESS_MILLILUX,
                            BIKE_HISTORY_TYPE_U32, (int32_t)data->ambient_brightness, ts);
    }
    if (data->has_battery_soc) {
        bike_history_append(BIKE_HISTORY_FIELD_BATTERY_SOC, BIKE_HISTORY_TYPE_U32,
                            (int32_t)data->battery_soc, ts);
    }
    if (data->has_odometer) {
        bike_history_append(BIKE_HISTORY_FIELD_ODOMETER_M, BIKE_HISTORY_TYPE_U32,
                            (int32_t)data->odometer, ts);
    }
    if (data->has_bike_light) {
        bike_history_append(BIKE_HISTORY_FIELD_BIKE_LIGHT, BIKE_HISTORY_TYPE_U32,
                            (int32_t)data->bike_light, ts);
    }
    if (data->has_system_locked) {
        bike_history_append(BIKE_HISTORY_FIELD_SYSTEM_LOCKED, BIKE_HISTORY_TYPE_BOOL,
                            data->system_locked ? 1 : 0, ts);
    }
    if (data->has_charger_connected) {
        bike_history_append(BIKE_HISTORY_FIELD_CHARGER_CONNECTED, BIKE_HISTORY_TYPE_BOOL,
                            data->charger_connected ? 1 : 0, ts);
    }
    if (data->has_light_reserve_state) {
        bike_history_append(BIKE_HISTORY_FIELD_LIGHT_RESERVE_STATE, BIKE_HISTORY_TYPE_BOOL,
                            data->light_reserve_state ? 1 : 0, ts);
    }
    if (data->has_diagnosis_program_active) {
        bike_history_append(BIKE_HISTORY_FIELD_DIAGNOSIS_PROGRAM_ACTIVE,
                            BIKE_HISTORY_TYPE_BOOL,
                            data->diagnosis_program_active ? 1 : 0, ts);
    }
    if (data->has_bike_not_driving) {
        bike_history_append(BIKE_HISTORY_FIELD_BIKE_NOT_DRIVING, BIKE_HISTORY_TYPE_BOOL,
                            data->bike_not_driving ? 1 : 0, ts);
    }
}

static bool live_data_state_has_any(const live_data_t *data)
{
    return data != NULL &&
           (data->has_speed || data->has_cadence || data->has_rider_power ||
            data->has_ambient_brightness || data->has_battery_soc || data->has_time ||
            data->has_odometer || data->has_bike_light || data->has_system_locked ||
            data->has_charger_connected || data->has_light_reserve_state ||
            data->has_diagnosis_program_active || data->has_bike_not_driving ||
            data->unknown_count > 0);
}

bool live_data_latest_summary(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }

    format_live_data_sample(&latest_state, out, out_len);
    if (out[0] != '\0') {
        return true;
    }
    snprintf(out, out_len, "No bike data received yet.");
    return false;
}

static void persist_latest_state_json(void)
{
    char *json = malloc(LIVE_DATA_LATEST_JSON_MAX_LEN);
    if (json == NULL) {
        return;
    }
    if (!live_data_latest_json(json, LIVE_DATA_LATEST_JSON_MAX_LEN)) {
        free(json);
        return;
    }

    FILE *f = fopen(LIVE_DATA_LATEST_PATH, "w");
    if (f == NULL) {
        free(json);
        return;
    }
    fwrite(json, 1, strlen(json), f);
    fclose(f);
    strlcpy(persisted_latest_json, json, sizeof(persisted_latest_json));
    persisted_latest_loaded = true;
    free(json);
}

static void persist_latest_state_json_if_due(uint32_t changed_mask)
{
    int64_t now = esp_timer_get_time();
    if (changed_mask == 0 &&
        last_latest_persist_us != 0 &&
        now - last_latest_persist_us < LIVE_DATA_LATEST_PERSIST_INTERVAL_US) {
        return;
    }
    last_latest_persist_us = now;
    persist_latest_state_json();
}

void live_data_init(void)
{
    FILE *f = fopen(LIVE_DATA_LATEST_PATH, "r");
    if (f == NULL) {
        persisted_latest_json[0] = '\0';
        persisted_latest_loaded = false;
        return;
    }

    size_t len = fread(persisted_latest_json, 1, sizeof(persisted_latest_json) - 1, f);
    fclose(f);
    persisted_latest_json[len] = '\0';
    persisted_latest_loaded = len > 0;
    if (persisted_latest_loaded && strstr(persisted_latest_json, "last_update_boot_ms") != NULL) {
        persisted_latest_json[0] = '\0';
        persisted_latest_loaded = false;
        ESP_LOGW(TAG, "ignored legacy latest bike snapshot with ESP receive metadata");
        return;
    }
    if (persisted_latest_loaded) {
        ESP_LOGI(TAG, "loaded persisted latest bike data snapshot");
    }
}

bool live_data_latest_json(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }

    live_data_t data = latest_state;
    bool has_any = live_data_state_has_any(&data);
    if (!has_any && persisted_latest_loaded) {
        strlcpy(out, persisted_latest_json, out_len);
        return out[0] != '\0';
    }

    bool needs_comma = false;
    int written = snprintf(out, out_len, "{");
    if (written < 0 || (size_t)written >= out_len) {
        return false;
    }

#define ADD_JSON_FIELD(fmt, ...) do { \
        size_t used = strlen(out); \
        if (used + 1 < out_len) { \
            snprintf(out + used, out_len - used, "%s" fmt, needs_comma ? "," : "", __VA_ARGS__); \
            needs_comma = true; \
        } \
    } while (0)

    if (data.last_update_unix_time > 0) {
        char iso[32] = {0};
        time_t unix_time = (time_t)data.last_update_unix_time;
        struct tm tm_utc = {0};
        if (gmtime_r(&unix_time, &tm_utc) != NULL) {
            strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
        }
        ADD_JSON_FIELD("\"last_update_unix_time\":%" PRId64, data.last_update_unix_time);
        if (iso[0] != '\0') {
            ADD_JSON_FIELD("\"last_update_iso_utc\":\"%s\"", iso);
        }
    }

    if (data.has_time) {
        ADD_JSON_FIELD("\"time\":%" PRId64, data.time);
    }
    if (data.has_speed) {
        ADD_JSON_FIELD("\"speed_kmh\":%.2f", data.speed / 100.0);
    }
    if (data.has_cadence) {
        ADD_JSON_FIELD("\"cadence_rpm\":%" PRId32, data.cadence);
    }
    if (data.has_rider_power) {
        ADD_JSON_FIELD("\"rider_power_w\":%" PRIu32, data.rider_power);
    }
    if (data.has_ambient_brightness) {
        ADD_JSON_FIELD("\"ambient_brightness_lux\":%.3f", data.ambient_brightness / 1000.0);
    }
    if (data.has_battery_soc) {
        ADD_JSON_FIELD("\"battery_soc\":%" PRIu32, data.battery_soc);
    }
    if (data.has_odometer) {
        ADD_JSON_FIELD("\"odometer_m\":%" PRIu32, data.odometer);
    }
    if (data.has_bike_light) {
        ADD_JSON_FIELD("\"bike_light\":\"%s\"", light_state_name(data.bike_light));
    }
    if (data.has_system_locked) {
        ADD_JSON_FIELD("\"system_locked\":%s", data.system_locked ? "true" : "false");
    }
    if (data.has_charger_connected) {
        ADD_JSON_FIELD("\"charger_connected\":%s", data.charger_connected ? "true" : "false");
    }
    if (data.has_light_reserve_state) {
        ADD_JSON_FIELD("\"light_reserve_state\":%s", data.light_reserve_state ? "true" : "false");
    }
    if (data.has_diagnosis_program_active) {
        ADD_JSON_FIELD("\"diagnosis_program_active\":%s", data.diagnosis_program_active ? "true" : "false");
    }
    if (data.has_bike_not_driving) {
        ADD_JSON_FIELD("\"bike_not_driving\":%s", data.bike_not_driving ? "true" : "false");
    }
#undef ADD_JSON_FIELD

    if (data.unknown_count > 0) {
        strlcat(out, needs_comma ? ",\"unknown_fields\":[" : "\"unknown_fields\":[", out_len);
        for (uint8_t i = 0; i < data.unknown_count; i++) {
            char item[128];
            snprintf(item, sizeof(item),
                     "%s{\"field_id\":%" PRIu32 ",\"wire_type\":%" PRIu32
                     ",\"raw_value\":%" PRIu64 ",\"length\":%" PRIu32 "}",
                     i == 0 ? "" : ",",
                     data.unknown[i].field_id,
                     data.unknown[i].wire_type,
                     data.unknown[i].value,
                     data.unknown[i].length);
            strlcat(out, item, out_len);
        }
        strlcat(out, "]", out_len);
        has_any = true;
    }

    strlcat(out, "}", out_len);
    return has_any;
}

bool live_data_latest_field_value(const char *field, double *out)
{
    if (field == NULL || out == NULL) {
        return false;
    }

    live_data_t data = latest_state;
    if (strcmp(field, "speed_kmh") == 0 && data.has_speed) {
        *out = data.speed / 100.0;
        return true;
    }
    if (strcmp(field, "cadence_rpm") == 0 && data.has_cadence) {
        *out = data.cadence;
        return true;
    }
    if (strcmp(field, "rider_power_w") == 0 && data.has_rider_power) {
        *out = data.rider_power;
        return true;
    }
    if (strcmp(field, "ambient_brightness_lux") == 0 && data.has_ambient_brightness) {
        *out = data.ambient_brightness / 1000.0;
        return true;
    }
    if (strcmp(field, "battery_soc") == 0 && data.has_battery_soc) {
        *out = data.battery_soc;
        return true;
    }
    if (strcmp(field, "odometer_m") == 0 && data.has_odometer) {
        *out = data.odometer;
        return true;
    }
    if (strcmp(field, "bike_light") == 0 && data.has_bike_light) {
        *out = data.bike_light;
        return true;
    }
    if (strcmp(field, "system_locked") == 0 && data.has_system_locked) {
        *out = data.system_locked ? 1.0 : 0.0;
        return true;
    }
    if (strcmp(field, "charger_connected") == 0 && data.has_charger_connected) {
        *out = data.charger_connected ? 1.0 : 0.0;
        return true;
    }
    if (strcmp(field, "light_reserve_state") == 0 && data.has_light_reserve_state) {
        *out = data.light_reserve_state ? 1.0 : 0.0;
        return true;
    }
    if (strcmp(field, "diagnosis_program_active") == 0 && data.has_diagnosis_program_active) {
        *out = data.diagnosis_program_active ? 1.0 : 0.0;
        return true;
    }
    if (strcmp(field, "bike_not_driving") == 0 && data.has_bike_not_driving) {
        *out = data.bike_not_driving ? 1.0 : 0.0;
        return true;
    }
    return false;
}

uint32_t live_data_field_mask(const char *field)
{
    if (field == NULL) {
        return 0;
    }
    if (strcmp(field, "speed_kmh") == 0) {
        return LIVE_DATA_FIELD_SPEED;
    }
    if (strcmp(field, "cadence_rpm") == 0) {
        return LIVE_DATA_FIELD_CADENCE;
    }
    if (strcmp(field, "rider_power_w") == 0) {
        return LIVE_DATA_FIELD_RIDER_POWER;
    }
    if (strcmp(field, "ambient_brightness_lux") == 0) {
        return LIVE_DATA_FIELD_AMBIENT_BRIGHTNESS;
    }
    if (strcmp(field, "battery_soc") == 0) {
        return LIVE_DATA_FIELD_BATTERY_SOC;
    }
    if (strcmp(field, "odometer_m") == 0) {
        return LIVE_DATA_FIELD_ODOMETER;
    }
    if (strcmp(field, "bike_light") == 0) {
        return LIVE_DATA_FIELD_BIKE_LIGHT;
    }
    if (strcmp(field, "system_locked") == 0) {
        return LIVE_DATA_FIELD_SYSTEM_LOCKED;
    }
    if (strcmp(field, "charger_connected") == 0) {
        return LIVE_DATA_FIELD_CHARGER_CONNECTED;
    }
    if (strcmp(field, "light_reserve_state") == 0) {
        return LIVE_DATA_FIELD_LIGHT_RESERVE;
    }
    if (strcmp(field, "diagnosis_program_active") == 0) {
        return LIVE_DATA_FIELD_DIAGNOSIS_ACTIVE;
    }
    if (strcmp(field, "bike_not_driving") == 0) {
        return LIVE_DATA_FIELD_BIKE_NOT_DRIVING;
    }
    return 0;
}

bool live_data_decode_and_log(const uint8_t *buf, size_t len)
{
    live_data_t data = {0};
    size_t pos = 0;

    while (pos < len) {
        uint64_t key;
        uint64_t value;
        if (!read_varint(buf, len, &pos, &key)) {
            return false;
        }

        uint32_t field = key >> 3;
        uint32_t wire_type = key & 0x07;
        if (wire_type != 0) {
            uint64_t raw = 0;
            uint32_t length = 0;
            if (wire_type == 1) {
                if (!read_fixed_le(buf, len, &pos, 8, &raw)) {
                    return false;
                }
                length = 8;
            } else if (wire_type == 2) {
                uint64_t field_len = 0;
                if (!read_varint(buf, len, &pos, &field_len) || field_len > len - pos) {
                    return false;
                }
                length = (uint32_t)field_len;
                pos += (size_t)field_len;
            } else if (wire_type == 5) {
                if (!read_fixed_le(buf, len, &pos, 4, &raw)) {
                    return false;
                }
                length = 4;
            } else if (!skip_field(buf, len, &pos, wire_type)) {
                return false;
            }
            upsert_unknown_field(&data, field, wire_type, raw, length);
            continue;
        }

        if (!read_varint(buf, len, &pos, &value)) {
            return false;
        }

        switch (field) {
        case 1:
            data.has_speed = true;
            data.speed = (uint32_t)value;
            break;
        case 2:
            data.has_cadence = true;
            data.cadence = (int32_t)value;
            break;
        case 5:
            data.has_rider_power = true;
            data.rider_power = (uint32_t)value;
            break;
        case 9:
            data.has_ambient_brightness = true;
            data.ambient_brightness = (uint32_t)value;
            break;
        case 10:
            data.has_battery_soc = true;
            data.battery_soc = (uint32_t)value;
            break;
        case 11:
            data.has_time = true;
            data.time = (int64_t)value;
            break;
        case 12:
            data.has_odometer = true;
            data.odometer = (uint32_t)value;
            break;
        case 17:
            data.has_bike_light = true;
            data.bike_light = (uint32_t)value;
            break;
        case 21:
            data.has_system_locked = true;
            data.system_locked = value != 0;
            break;
        case 22:
            data.has_charger_connected = true;
            data.charger_connected = value != 0;
            break;
        case 23:
            data.has_light_reserve_state = true;
            data.light_reserve_state = value != 0;
            break;
        case 24:
            data.has_diagnosis_program_active = true;
            data.diagnosis_program_active = value != 0;
            break;
        case 25:
            data.has_bike_not_driving = true;
            data.bike_not_driving = value != 0;
            break;
        default:
            upsert_unknown_field(&data, field, wire_type, value, 0);
            break;
        }
    }

    uint32_t changed_mask = changed_field_mask(&data);
    log_live_data(&data);
    persist_live_data_history_sample(&data);
    merge_latest_state(&data);
    persist_live_data_sample(&latest_state);
    persist_latest_state_json_if_due(changed_mask);
    automation_request_evaluate(changed_mask);
    return true;
}
