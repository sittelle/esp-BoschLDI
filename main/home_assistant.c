#include "home_assistant.h"

#include <stdio.h>
#include <string.h>

#include "accessory_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "live_data_decode.h"
#include "mqtt_client.h"
#include "persistent_log.h"
#include "wifi_admin.h"

#define HA_WORKER_STACK_WORDS 8192
#define HA_DISABLED_POLL_SEC 5
#define HA_MQTT_URI_MAX_LEN 96
#define HA_TOPIC_MAX_LEN 160
#define HA_PAYLOAD_MAX_LEN 1024

static const char *TAG = "home_assistant";

typedef struct {
    const char *component;
    const char *object_id;
    const char *name;
    const char *field;
    const char *unit;
    const char *device_class;
    const char *state_class;
    const char *payload_on;
    const char *payload_off;
} ha_entity_t;

static const ha_entity_t HA_ENTITIES[] = {
    {"sensor", "speed", "Speed", "speed_kmh", "km/h", "speed", "measurement", NULL, NULL},
    {"sensor", "cadence", "Cadence", "cadence_rpm", "rpm", NULL, "measurement", NULL, NULL},
    {"sensor", "rider_power", "Rider power", "rider_power_w", "W", "power", "measurement", NULL, NULL},
    {"sensor", "ambient_brightness", "Ambient brightness", "ambient_brightness_lux", "lx", "illuminance", "measurement", NULL, NULL},
    {"sensor", "battery", "Battery", "battery_soc", "%", "battery", "measurement", NULL, NULL},
    {"sensor", "odometer", "Odometer", "odometer_m", "m", "distance", "total_increasing", NULL, NULL},
    {"sensor", "bike_light", "Bike light", "bike_light", NULL, NULL, NULL, NULL, NULL},
    {"binary_sensor", "system_locked", "System locked", "system_locked", NULL, "lock", NULL, "true", "false"},
    {"binary_sensor", "charger_connected", "Charger connected", "charger_connected", NULL, "plug", NULL, "true", "false"},
    {"binary_sensor", "light_reserve", "Light reserve", "light_reserve_state", NULL, NULL, NULL, "true", "false"},
    {"binary_sensor", "diagnosis_active", "Diagnosis active", "diagnosis_program_active", NULL, "problem", NULL, "true", "false"},
    {"binary_sensor", "bike_not_driving", "Bike not driving", "bike_not_driving", NULL, "moving", NULL, "true", "false"},
};

static esp_mqtt_client_handle_t mqtt_client;
static bool mqtt_started;
static bool mqtt_connected;
static bool discovery_published;
static accessory_ha_config_t active_config;
static TaskHandle_t ha_task_handle;

static void json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; si++) {
        unsigned char c = (unsigned char)src[si];
        if ((c == '"' || c == '\\') && di + 2 < dst_len) {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c >= 0x20) {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

static bool ha_config_same(const accessory_ha_config_t *a, const accessory_ha_config_t *b)
{
    return a->enabled == b->enabled &&
           a->port == b->port &&
           a->interval_sec == b->interval_sec &&
           strcmp(a->host, b->host) == 0 &&
           strcmp(a->username, b->username) == 0 &&
           strcmp(a->password, b->password) == 0 &&
           strcmp(a->discovery_prefix, b->discovery_prefix) == 0 &&
           strcmp(a->topic_base, b->topic_base) == 0;
}

static void publish_topic(const char *topic, const char *payload, int qos, bool retain)
{
    if (mqtt_client == NULL || !mqtt_connected) {
        return;
    }
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, qos, retain ? 1 : 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "MQTT publish failed topic=%s", topic);
    }
}

static void stop_client(void)
{
    if (mqtt_client == NULL) {
        return;
    }
    if (mqtt_connected) {
        char status_topic[HA_TOPIC_MAX_LEN];
        snprintf(status_topic, sizeof(status_topic), "%s/status", active_config.topic_base);
        publish_topic(status_topic, "offline", 0, true);
    }
    if (mqtt_started) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_mqtt_client_stop(mqtt_client));
    }
    esp_mqtt_client_destroy(mqtt_client);
    mqtt_client = NULL;
    mqtt_started = false;
    mqtt_connected = false;
    discovery_published = false;
    memset(&active_config, 0, sizeof(active_config));
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    if (event_id == MQTT_EVENT_CONNECTED) {
        mqtt_connected = true;
        discovery_published = false;
        ESP_LOGI(TAG, "MQTT connected");
        persistent_log_event("info", "home_assistant", "MQTT connected");
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        persistent_log_event("warn", "home_assistant", "MQTT disconnected");
    } else if (event_id == MQTT_EVENT_ERROR) {
        ESP_LOGW(TAG, "MQTT error");
        persistent_log_event("warn", "home_assistant", "MQTT error");
    }
}

static esp_err_t start_client(const accessory_ha_config_t *config)
{
    char uri[HA_MQTT_URI_MAX_LEN];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", config->host, (unsigned)config->port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = config->username[0] != '\0' ? config->username : NULL,
        .credentials.authentication.password =
            config->password[0] != '\0' ? config->password : NULL,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_mqtt_client_register_event(
        mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    esp_err_t err = esp_mqtt_client_start(mqtt_client);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
        return err;
    }
    mqtt_started = true;
    active_config = *config;
    persistent_log_event("info", "home_assistant", "MQTT client started host=%s port=%u",
                         config->host, (unsigned)config->port);
    return ESP_OK;
}

static void append_json_field(char *payload, size_t payload_len,
                              const char *name, const char *value, bool *has_any)
{
    if (value == NULL || value[0] == '\0') {
        return;
    }
    size_t used = strlen(payload);
    snprintf(payload + used, payload_len - used, "%s\"%s\":\"%s\"",
             *has_any ? "," : "", name, value);
    *has_any = true;
}

static void publish_entity_discovery(const accessory_ha_config_t *config,
                                     const char *device_name,
                                     const ha_entity_t *entity)
{
    char topic[HA_TOPIC_MAX_LEN];
    char payload[HA_PAYLOAD_MAX_LEN];
    char escaped_device_name[64];
    char state_topic[HA_TOPIC_MAX_LEN];
    char availability_topic[HA_TOPIC_MAX_LEN];
    snprintf(topic, sizeof(topic), "%s/%s/%s_%s/config",
             config->discovery_prefix, entity->component, config->topic_base, entity->object_id);
    snprintf(state_topic, sizeof(state_topic), "%s/state", config->topic_base);
    snprintf(availability_topic, sizeof(availability_topic), "%s/status", config->topic_base);
    json_escape(escaped_device_name, sizeof(escaped_device_name), device_name);

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
             "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.%s }}\","
             "\"availability_topic\":\"%s\",\"payload_available\":\"online\","
             "\"payload_not_available\":\"offline\"",
             entity->name, config->topic_base, entity->object_id, state_topic,
             entity->field, availability_topic);

    bool has_any = true;
    append_json_field(payload, sizeof(payload), "unit_of_measurement", entity->unit, &has_any);
    append_json_field(payload, sizeof(payload), "device_class", entity->device_class, &has_any);
    append_json_field(payload, sizeof(payload), "state_class", entity->state_class, &has_any);
    append_json_field(payload, sizeof(payload), "payload_on", entity->payload_on, &has_any);
    append_json_field(payload, sizeof(payload), "payload_off", entity->payload_off, &has_any);
    size_t used = strlen(payload);
    snprintf(payload + used, sizeof(payload) - used,
             ",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
             "\"manufacturer\":\"Sittelle\",\"model\":\"ESP32 Bosch LDI\"}}",
             config->topic_base, escaped_device_name);

    publish_topic(topic, payload, 0, true);
}

static void publish_discovery(const accessory_ha_config_t *config)
{
    char device_name[ACCESSORY_DEVICE_NAME_MAX_LEN + 1];
    accessory_config_load_device_name(device_name, sizeof(device_name));

    for (size_t i = 0; i < sizeof(HA_ENTITIES) / sizeof(HA_ENTITIES[0]); i++) {
        publish_entity_discovery(config, device_name, &HA_ENTITIES[i]);
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    char status_topic[HA_TOPIC_MAX_LEN];
    snprintf(status_topic, sizeof(status_topic), "%s/status", config->topic_base);
    publish_topic(status_topic, "online", 0, true);
    discovery_published = true;
    persistent_log_event("info", "home_assistant", "MQTT discovery published prefix=%s topic=%s",
                         config->discovery_prefix, config->topic_base);
}

static void publish_state(const accessory_ha_config_t *config)
{
    char bike_json[1536];
    if (!live_data_latest_json(bike_json, sizeof(bike_json))) {
        return;
    }

    char topic[HA_TOPIC_MAX_LEN];
    snprintf(topic, sizeof(topic), "%s/state", config->topic_base);
    publish_topic(topic, bike_json, 0, false);
}

static void home_assistant_task(void *arg)
{
    (void)arg;
    accessory_ha_config_t config;

    while (true) {
        accessory_config_load_ha(&config);
        if (!config.enabled || config.host[0] == '\0' || !wifi_admin_is_connected()) {
            stop_client();
            vTaskDelay(pdMS_TO_TICKS(HA_DISABLED_POLL_SEC * 1000));
            continue;
        }

        if (mqtt_client == NULL || !ha_config_same(&active_config, &config)) {
            stop_client();
            esp_err_t err = start_client(&config);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "MQTT start failed; err=%s", esp_err_to_name(err));
                persistent_log_event("warn", "home_assistant", "MQTT start failed err=%s",
                                     esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(HA_DISABLED_POLL_SEC * 1000));
                continue;
            }
        }

        if (mqtt_connected) {
            if (!discovery_published) {
                publish_discovery(&config);
            }
            publish_state(&config);
        }
        vTaskDelay(pdMS_TO_TICKS(accessory_config_clamp_ha_interval(config.interval_sec) * 1000));
    }
}

esp_err_t home_assistant_start(void)
{
    if (ha_task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(home_assistant_task, "home_assistant",
                                HA_WORKER_STACK_WORDS, NULL, 3, &ha_task_handle);
    if (ok != pdPASS) {
        ha_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    persistent_log_event("info", "home_assistant", "Home Assistant worker started");
    return ESP_OK;
}
