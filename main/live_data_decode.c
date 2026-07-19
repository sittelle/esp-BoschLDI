#include "live_data_decode.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "automation.h"
#include "esp_log.h"
#include "persistent_log.h"

static const char *TAG = "live_data";

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
} live_data_t;

static live_data_t latest_state;

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

bool live_data_latest_json(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }

    live_data_t data = latest_state;
    bool has_any = false;
    int written = snprintf(out, out_len, "{");
    if (written < 0 || (size_t)written >= out_len) {
        return false;
    }

#define ADD_JSON_FIELD(fmt, ...) do { \
        size_t used = strlen(out); \
        if (used + 1 < out_len) { \
            snprintf(out + used, out_len - used, "%s" fmt, has_any ? "," : "", __VA_ARGS__); \
            has_any = true; \
        } \
    } while (0)

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
            if (!skip_field(buf, len, &pos, wire_type)) {
                return false;
            }
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
            break;
        }
    }

    log_live_data(&data);
    merge_latest_state(&data);
    persist_live_data_sample(&latest_state);
    automation_evaluate_latest();
    return true;
}
