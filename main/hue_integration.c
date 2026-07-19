#include "hue_integration.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accessory_config.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "persistent_log.h"

#define HUE_MDNS_SERVICE "_hue"
#define HUE_MDNS_PROTO "_tcp"
#define HUE_DISCOVERY_TIMEOUT_MS 3000
#define HUE_DISCOVERY_MAX_RESULTS 6
#define HUE_HTTP_TIMEOUT_MS 5000
#define HUE_PAIR_RESPONSE_SIZE 1024
#define HUE_DEVICES_RESPONSE_SIZE 20000
#define HUE_RESOURCES_RESPONSE_SIZE 28000
#define HUE_PAIR_WINDOW_ATTEMPTS 30
#define HUE_PAIR_RETRY_DELAY_MS 2000

static const char *TAG = "hue";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} hue_http_response_t;

typedef struct {
    char bridge_host[ACCESSORY_HUE_HOST_MAX_LEN + 1];
    char bridge_name[ACCESSORY_HUE_BRIDGE_NAME_MAX_LEN + 1];
} hue_pair_task_arg_t;

static SemaphoreHandle_t hue_pair_lock;
static bool hue_pair_running;
static bool hue_pair_success;
static unsigned hue_pair_attempt;
static char hue_pair_host[ACCESSORY_HUE_HOST_MAX_LEN + 1];
static char hue_pair_last_json[HUE_PAIR_RESPONSE_SIZE];

static void hue_pair_lock_init(void)
{
    if (hue_pair_lock == NULL) {
        hue_pair_lock = xSemaphoreCreateMutex();
    }
}

static void hue_pair_set_state(bool running, bool success, unsigned attempt,
                               const char *host, const char *last_json)
{
    hue_pair_lock_init();
    if (hue_pair_lock != NULL) {
        xSemaphoreTake(hue_pair_lock, portMAX_DELAY);
    }
    hue_pair_running = running;
    hue_pair_success = success;
    hue_pair_attempt = attempt;
    if (host != NULL) {
        strlcpy(hue_pair_host, host, sizeof(hue_pair_host));
    }
    if (last_json != NULL) {
        strlcpy(hue_pair_last_json, last_json, sizeof(hue_pair_last_json));
    }
    if (hue_pair_lock != NULL) {
        xSemaphoreGive(hue_pair_lock);
    }
}

static void json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src != NULL && src[si] != '\0' && di + 1 < dst_len; si++) {
        unsigned char c = (unsigned char)src[si];
        if ((c == '"' || c == '\\') && di + 2 < dst_len) {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c >= 0x20 && c <= 0x7e) {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

static void append_json(char *out, size_t out_len, const char *fmt, ...)
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

static bool hue_light_id_is_valid(const char *light_id)
{
    if (light_id == NULL || light_id[0] == '\0') {
        return false;
    }
    for (size_t i = 0; light_id[i] != '\0'; i++) {
        if (i > 7 || light_id[i] < '0' || light_id[i] > '9') {
            return false;
        }
    }
    return true;
}

static const char *txt_value(mdns_result_t *result, const char *key)
{
    for (size_t i = 0; result != NULL && i < result->txt_count; i++) {
        if (result->txt[i].key != NULL &&
            result->txt[i].value != NULL &&
            strcmp(result->txt[i].key, key) == 0) {
            return result->txt[i].value;
        }
    }
    return "";
}

static esp_err_t hue_http_event_handler(esp_http_client_event_t *evt)
{
    hue_http_response_t *response = (hue_http_response_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || response == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t available = response->cap > response->len ? response->cap - response->len - 1 : 0;
    if ((size_t)evt->data_len > available) {
        response->overflow = true;
        return ESP_OK;
    }

    memcpy(response->data + response->len, evt->data, evt->data_len);
    response->len += evt->data_len;
    response->data[response->len] = '\0';
    return ESP_OK;
}

static esp_err_t hue_http_request(const char *url, esp_http_client_method_t method,
                                  const char *body, char *out, size_t out_len,
                                  int *status_out)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    hue_http_response_t response = {
        .data = out,
        .cap = out_len,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = HUE_HTTP_TIMEOUT_MS,
        .event_handler = hue_http_event_handler,
        .user_data = &response,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    if (body != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (status_out != NULL) {
        *status_out = status;
    }
    esp_http_client_cleanup(client);

    if (err == ESP_ERR_HTTP_INCOMPLETE_DATA &&
        status >= 200 && status < 300 && response.len > 0) {
        err = ESP_OK;
    }
    if (err == ESP_OK && response.overflow) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK && (status < 200 || status >= 300)) {
        err = ESP_FAIL;
    }
    return err;
}

static bool hue_discover_bridge(const char *requested_host,
                                char *host, size_t host_len,
                                char *bridge_id, size_t bridge_id_len,
                                char *bridge_name, size_t bridge_name_len)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(HUE_MDNS_SERVICE, HUE_MDNS_PROTO,
                                   HUE_DISCOVERY_TIMEOUT_MS,
                                   HUE_DISCOVERY_MAX_RESULTS,
                                   &results);
    if (err != ESP_OK || results == NULL) {
        if (results != NULL) {
            mdns_query_results_free(results);
        }
        return false;
    }

    bool found = false;
    for (mdns_result_t *r = results; r != NULL && !found; r = r->next) {
        char ip[20] = "";
        for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                break;
            }
        }
        if (ip[0] == '\0') {
            continue;
        }
        bool match = requested_host == NULL || requested_host[0] == '\0' ||
                     strcmp(requested_host, ip) == 0 ||
                     (r->hostname != NULL && strcmp(requested_host, r->hostname) == 0);
        if (match) {
            strlcpy(host, ip, host_len);
            strlcpy(bridge_id, txt_value(r, "bridgeid"), bridge_id_len);
            strlcpy(bridge_name, r->instance_name != NULL ? r->instance_name : "",
                    bridge_name_len);
            found = true;
        }
    }
    mdns_query_results_free(results);
    return found;
}

static bool hue_parse_pair_success(const char *json, char *app_key, size_t app_key_len,
                                   char *error, size_t error_len)
{
    app_key[0] = '\0';
    error[0] = '\0';
    cJSON *root = cJSON_Parse(json);
    if (root == NULL || !cJSON_IsArray(root)) {
        strlcpy(error, "invalid Hue pair response", error_len);
        cJSON_Delete(root);
        return false;
    }

    cJSON *first = cJSON_GetArrayItem(root, 0);
    cJSON *success = cJSON_GetObjectItem(first, "success");
    cJSON *username = cJSON_GetObjectItem(success, "username");
    if (cJSON_IsString(username) && username->valuestring != NULL) {
        strlcpy(app_key, username->valuestring, app_key_len);
        cJSON_Delete(root);
        return true;
    }

    cJSON *error_obj = cJSON_GetObjectItem(first, "error");
    cJSON *description = cJSON_GetObjectItem(error_obj, "description");
    if (cJSON_IsString(description) && description->valuestring != NULL) {
        strlcpy(error, description->valuestring, error_len);
    } else {
        strlcpy(error, "Hue Bridge did not authorize this device", error_len);
    }
    cJSON_Delete(root);
    return false;
}

esp_err_t hue_integration_bridge_discovery_json(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = '\0';
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(HUE_MDNS_SERVICE, HUE_MDNS_PROTO,
                                   HUE_DISCOVERY_TIMEOUT_MS,
                                   HUE_DISCOVERY_MAX_RESULTS,
                                   &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Hue Bridge mDNS query failed; err=%s", esp_err_to_name(err));
        persistent_log_event("warn", "hue", "bridge discovery failed err=%s", esp_err_to_name(err));
        snprintf(out, out_len, "{\"type\":\"hue_bridges\",\"error\":\"mdns_query_failed\",\"bridges\":[]}");
        return err;
    }

    snprintf(out, out_len, "{\"type\":\"hue_bridges\",\"bridges\":[");
    size_t count = 0;
    for (mdns_result_t *r = results; r != NULL; r = r->next) {
        char instance[80];
        char hostname[80];
        char bridge_id[80];
        char model_id[40];
        char ip[20] = "";
        json_escape(instance, sizeof(instance), r->instance_name);
        json_escape(hostname, sizeof(hostname), r->hostname);
        json_escape(bridge_id, sizeof(bridge_id), txt_value(r, "bridgeid"));
        json_escape(model_id, sizeof(model_id), txt_value(r, "modelid"));

        for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                break;
            }
        }

        append_json(out, out_len,
                    "%s{\"instance\":\"%s\",\"hostname\":\"%s\",\"ip\":\"%s\","
                    "\"port\":%u,\"bridge_id\":\"%s\",\"model_id\":\"%s\"}",
                    count == 0 ? "" : ",", instance, hostname, ip,
                    (unsigned)r->port, bridge_id, model_id);
        count++;
    }
    append_json(out, out_len, "],\"count\":%u}", (unsigned)count);
    mdns_query_results_free(results);

    ESP_LOGI(TAG, "Hue Bridge discovery complete; count=%u", (unsigned)count);
    persistent_log_event("info", "hue", "bridge discovery count=%u", (unsigned)count);
    return ESP_OK;
}

esp_err_t hue_integration_status_json(char *out, size_t out_len)
{
    accessory_hue_config_t config;
    accessory_config_load_hue(&config);
    if (config.app_key[0] != '\0' && config.bridge_name[0] == '\0') {
        char discovered_host[ACCESSORY_HUE_HOST_MAX_LEN + 1] = "";
        char discovered_id[ACCESSORY_HUE_BRIDGE_ID_MAX_LEN + 1] = "";
        char discovered_name[ACCESSORY_HUE_BRIDGE_NAME_MAX_LEN + 1] = "";
        if (hue_discover_bridge(config.bridge_host, discovered_host, sizeof(discovered_host),
                                discovered_id, sizeof(discovered_id),
                                discovered_name, sizeof(discovered_name)) &&
            discovered_name[0] != '\0') {
            strlcpy(config.bridge_name, discovered_name, sizeof(config.bridge_name));
            if (config.bridge_id[0] == '\0') {
                strlcpy(config.bridge_id, discovered_id, sizeof(config.bridge_id));
            }
            ESP_ERROR_CHECK_WITHOUT_ABORT(accessory_config_save_hue(&config));
        }
    }

    char host[96];
    char bridge_id[96];
    char bridge_name[96];
    json_escape(host, sizeof(host), config.bridge_host);
    json_escape(bridge_id, sizeof(bridge_id), config.bridge_id);
    json_escape(bridge_name, sizeof(bridge_name), config.bridge_name);
    snprintf(out, out_len,
             "{\"type\":\"hue_status\",\"paired\":%s,\"bridge_host\":\"%s\","
             "\"bridge_id\":\"%s\",\"bridge_name\":\"%s\",\"api\":\"v1-local-http\"}",
             config.app_key[0] != '\0' ? "true" : "false", host, bridge_id, bridge_name);
    return ESP_OK;
}

esp_err_t hue_integration_pair_json(const char *bridge_host, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char selected_host[ACCESSORY_HUE_HOST_MAX_LEN + 1] = "";
    char bridge_id[ACCESSORY_HUE_BRIDGE_ID_MAX_LEN + 1] = "";
    char bridge_name[ACCESSORY_HUE_BRIDGE_NAME_MAX_LEN + 1] = "";
    if (bridge_host != NULL && bridge_host[0] != '\0') {
        strlcpy(selected_host, bridge_host, sizeof(selected_host));
        char discovered_host[ACCESSORY_HUE_HOST_MAX_LEN + 1] = "";
        hue_discover_bridge(selected_host, discovered_host, sizeof(discovered_host),
                            bridge_id, sizeof(bridge_id),
                            bridge_name, sizeof(bridge_name));
    } else if (!hue_discover_bridge(NULL, selected_host, sizeof(selected_host),
                                    bridge_id, sizeof(bridge_id),
                                    bridge_name, sizeof(bridge_name))) {
        snprintf(out, out_len, "{\"type\":\"hue_pair\",\"paired\":false,"
                 "\"error\":\"no Hue Bridge discovered\"}");
        return ESP_ERR_NOT_FOUND;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s/api", selected_host);
    ESP_LOGI(TAG, "starting Hue Bridge pairing request host=%s", selected_host);
    persistent_log_event("info", "hue", "pair request started host=%s", selected_host);

    char *response = malloc(HUE_PAIR_RESPONSE_SIZE);
    if (response == NULL) {
        snprintf(out, out_len, "{\"type\":\"hue_pair\",\"paired\":false,"
                 "\"error\":\"Hue pair response allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = hue_http_request(url, HTTP_METHOD_POST,
                                     "{\"devicetype\":\"boschldi#esp32\"}",
                                     response, HUE_PAIR_RESPONSE_SIZE, &status);
    ESP_LOGI(TAG, "Hue Bridge pairing HTTP finished host=%s status=%d err=%s",
             selected_host, status, esp_err_to_name(err));
    if (err != ESP_OK) {
        snprintf(out, out_len,
                 "{\"type\":\"hue_pair\",\"paired\":false,\"http_status\":%d,"
                 "\"error\":\"Hue pair request failed\"}", status);
        free(response);
        persistent_log_event("warn", "hue", "pair request failed host=%s err=%s status=%d",
                             selected_host, esp_err_to_name(err), status);
        return err;
    }

    char app_key[ACCESSORY_HUE_APP_KEY_MAX_LEN + 1];
    char pair_error[160];
    bool paired = hue_parse_pair_success(response, app_key, sizeof(app_key),
                                         pair_error, sizeof(pair_error));
    free(response);
    if (!paired) {
        char escaped_error[200];
        json_escape(escaped_error, sizeof(escaped_error), pair_error);
        snprintf(out, out_len,
                 "{\"type\":\"hue_pair\",\"paired\":false,\"bridge_host\":\"%s\","
                 "\"error\":\"%s\"}", selected_host, escaped_error);
        persistent_log_event("warn", "hue", "pair failed host=%s error=%s",
                             selected_host, pair_error);
        return ESP_ERR_INVALID_RESPONSE;
    }

    accessory_hue_config_t config = {0};
    strlcpy(config.bridge_host, selected_host, sizeof(config.bridge_host));
    strlcpy(config.bridge_id, bridge_id, sizeof(config.bridge_id));
    strlcpy(config.bridge_name, bridge_name, sizeof(config.bridge_name));
    strlcpy(config.app_key, app_key, sizeof(config.app_key));
    err = accessory_config_save_hue(&config);
    if (err != ESP_OK) {
        snprintf(out, out_len,
                 "{\"type\":\"hue_pair\",\"paired\":false,\"bridge_host\":\"%s\","
                 "\"error\":\"Hue key save failed\"}", selected_host);
        return err;
    }

    char escaped_bridge[96];
    char escaped_name[96];
    json_escape(escaped_bridge, sizeof(escaped_bridge), bridge_id);
    json_escape(escaped_name, sizeof(escaped_name), bridge_name);
    snprintf(out, out_len,
             "{\"type\":\"hue_pair\",\"paired\":true,\"bridge_host\":\"%s\","
             "\"bridge_id\":\"%s\",\"bridge_name\":\"%s\"}",
             selected_host, escaped_bridge, escaped_name);
    persistent_log_event("info", "hue", "paired with bridge host=%s bridge_id=%s name=%s",
                         selected_host, bridge_id, bridge_name);
    return ESP_OK;
}

static void hue_pair_task(void *arg)
{
    hue_pair_task_arg_t *task_arg = (hue_pair_task_arg_t *)arg;
    char host[ACCESSORY_HUE_HOST_MAX_LEN + 1];
    strlcpy(host, task_arg->bridge_host, sizeof(host));
    free(task_arg);

    char *response = malloc(HUE_PAIR_RESPONSE_SIZE);
    if (response == NULL) {
        hue_pair_set_state(false, false, 0, host,
                           "{\"type\":\"hue_pair_progress\",\"paired\":false,"
                           "\"error\":\"Hue pair response allocation failed\"}");
        vTaskDelete(NULL);
        return;
    }

    for (unsigned attempt = 1; attempt <= HUE_PAIR_WINDOW_ATTEMPTS; attempt++) {
        esp_err_t err = hue_integration_pair_json(host, response, HUE_PAIR_RESPONSE_SIZE);
        bool paired = err == ESP_OK;
        hue_pair_set_state(!paired, paired, attempt, host, response);
        if (paired) {
            free(response);
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(HUE_PAIR_RETRY_DELAY_MS));
    }

    snprintf(response, HUE_PAIR_RESPONSE_SIZE,
             "{\"type\":\"hue_pair\",\"paired\":false,\"bridge_host\":\"%s\","
             "\"error\":\"pairing timed out; press the Hue Bridge button and try again\"}",
             host);
    hue_pair_set_state(false, false, HUE_PAIR_WINDOW_ATTEMPTS, host, response);
    persistent_log_event("warn", "hue", "pairing window timed out host=%s", host);
    free(response);
    vTaskDelete(NULL);
}

esp_err_t hue_integration_pair_start_json(const char *bridge_host, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    accessory_hue_config_t config;
    accessory_config_load_hue(&config);
    if (config.app_key[0] != '\0') {
        char escaped_host[96];
        char escaped_name[96];
        json_escape(escaped_host, sizeof(escaped_host), config.bridge_host);
        json_escape(escaped_name, sizeof(escaped_name), config.bridge_name);
        snprintf(out, out_len,
                 "{\"type\":\"hue_pair_start\",\"running\":false,\"paired\":true,"
                 "\"bridge_host\":\"%s\",\"bridge_name\":\"%s\"}",
                 escaped_host, escaped_name);
        return ESP_OK;
    }

    hue_pair_lock_init();
    if (hue_pair_lock != NULL) {
        xSemaphoreTake(hue_pair_lock, portMAX_DELAY);
    }
    bool already_running = hue_pair_running;
    if (hue_pair_lock != NULL) {
        xSemaphoreGive(hue_pair_lock);
    }
    if (already_running) {
        snprintf(out, out_len,
                 "{\"type\":\"hue_pair_start\",\"running\":true,\"paired\":false,"
                 "\"message\":\"Hue pairing is already running\"}");
        return ESP_OK;
    }

    hue_pair_task_arg_t *task_arg = calloc(1, sizeof(*task_arg));
    if (task_arg == NULL) {
        snprintf(out, out_len, "{\"type\":\"hue_pair_start\",\"running\":false,"
                 "\"paired\":false,\"error\":\"Hue pair task allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }
    if (bridge_host != NULL) {
        strlcpy(task_arg->bridge_host, bridge_host, sizeof(task_arg->bridge_host));
    }
    if (task_arg->bridge_host[0] != '\0') {
        char discovered_host[ACCESSORY_HUE_HOST_MAX_LEN + 1] = "";
        char bridge_id[ACCESSORY_HUE_BRIDGE_ID_MAX_LEN + 1] = "";
        hue_discover_bridge(task_arg->bridge_host, discovered_host, sizeof(discovered_host),
                            bridge_id, sizeof(bridge_id),
                            task_arg->bridge_name, sizeof(task_arg->bridge_name));
    }

    hue_pair_set_state(true, false, 0, task_arg->bridge_host,
                       "{\"type\":\"hue_pair\",\"paired\":false,"
                       "\"message\":\"waiting for Hue Bridge button\"}");
    BaseType_t ok = xTaskCreate(hue_pair_task, "hue_pair", 6144, task_arg, 3, NULL);
    if (ok != pdPASS) {
        free(task_arg);
        hue_pair_set_state(false, false, 0, "",
                           "{\"type\":\"hue_pair\",\"paired\":false,"
                           "\"error\":\"Hue pair task start failed\"}");
        snprintf(out, out_len, "{\"type\":\"hue_pair_start\",\"running\":false,"
                 "\"paired\":false,\"error\":\"Hue pair task start failed\"}");
        return ESP_ERR_NO_MEM;
    }

    snprintf(out, out_len,
             "{\"type\":\"hue_pair_start\",\"running\":true,\"paired\":false,"
             "\"window_sec\":%u,\"retry_sec\":%u}",
             (unsigned)((HUE_PAIR_WINDOW_ATTEMPTS * HUE_PAIR_RETRY_DELAY_MS) / 1000),
             (unsigned)(HUE_PAIR_RETRY_DELAY_MS / 1000));
    persistent_log_event("info", "hue", "pairing window started");
    return ESP_OK;
}

esp_err_t hue_integration_pair_progress_json(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    hue_pair_lock_init();
    bool running;
    bool success;
    unsigned attempt;
    char host[ACCESSORY_HUE_HOST_MAX_LEN + 1];
    char bridge_name[ACCESSORY_HUE_BRIDGE_NAME_MAX_LEN + 1] = "";
    char last_json[HUE_PAIR_RESPONSE_SIZE];
    if (hue_pair_lock != NULL) {
        xSemaphoreTake(hue_pair_lock, portMAX_DELAY);
    }
    running = hue_pair_running;
    success = hue_pair_success;
    attempt = hue_pair_attempt;
    strlcpy(host, hue_pair_host, sizeof(host));
    strlcpy(last_json, hue_pair_last_json, sizeof(last_json));
    if (hue_pair_lock != NULL) {
        xSemaphoreGive(hue_pair_lock);
    }

    if (!running) {
        accessory_hue_config_t config;
        accessory_config_load_hue(&config);
        if (config.app_key[0] != '\0') {
            success = true;
            strlcpy(bridge_name, config.bridge_name, sizeof(bridge_name));
            if (host[0] == '\0') {
                strlcpy(host, config.bridge_host, sizeof(host));
            }
        }
    }

    char escaped_host[96];
    char escaped_name[96];
    json_escape(escaped_host, sizeof(escaped_host), host);
    json_escape(escaped_name, sizeof(escaped_name), bridge_name);
    snprintf(out, out_len,
             "{\"type\":\"hue_pair_progress\",\"running\":%s,\"paired\":%s,"
             "\"attempt\":%u,\"max_attempts\":%u,\"bridge_host\":\"%s\","
             "\"bridge_name\":\"%s\",\"last\":",
             running ? "true" : "false", success ? "true" : "false",
             attempt, (unsigned)HUE_PAIR_WINDOW_ATTEMPTS, escaped_host, escaped_name);
    append_json(out, out_len, "%s}", last_json[0] != '\0' ? last_json : "null");
    return ESP_OK;
}

static esp_err_t hue_bridge_get_json(const char *path, const char *type,
                                     char *out, size_t out_len)
{
    accessory_hue_config_t config;
    accessory_config_load_hue(&config);
    if (config.bridge_host[0] == '\0' || config.app_key[0] == '\0') {
        snprintf(out, out_len, "{\"type\":\"%s\",\"error\":\"Hue Bridge is not paired\"}", type);
        return ESP_ERR_INVALID_STATE;
    }

    char url[192];
    snprintf(url, sizeof(url), "http://%s/api/%s%s",
             config.bridge_host, config.app_key, path);

    char *response = malloc(out_len);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = hue_http_request(url, HTTP_METHOD_GET, NULL, response, out_len, &status);
    if (err != ESP_OK) {
        snprintf(out, out_len,
                 "{\"type\":\"%s\",\"bridge_host\":\"%s\",\"http_status\":%d,"
                 "\"error\":\"Hue Bridge request failed\"}",
                 type, config.bridge_host, status);
        free(response);
        persistent_log_event("warn", "hue", "request failed path=%s err=%s status=%d",
                             path, esp_err_to_name(err), status);
        return err;
    }

    char bridge_name[96];
    json_escape(bridge_name, sizeof(bridge_name), config.bridge_name);
    snprintf(out, out_len, "{\"type\":\"%s\",\"bridge_host\":\"%s\","
             "\"bridge_name\":\"%s\",\"api\":\"v1\",\"data\":",
             type, config.bridge_host, bridge_name);
    append_json(out, out_len, "%s}", response[0] != '\0' ? response : "{}");
    free(response);
    return ESP_OK;
}

esp_err_t hue_integration_devices_json(char *out, size_t out_len)
{
    return hue_bridge_get_json("/lights", "hue_devices", out, out_len);
}

esp_err_t hue_integration_resources_json(char *out, size_t out_len)
{
    return hue_bridge_get_json("", "hue_resources", out, out_len);
}

esp_err_t hue_integration_set_light_state_json(const char *light_id, bool on,
                                               char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!hue_light_id_is_valid(light_id)) {
        snprintf(out, out_len,
                 "{\"type\":\"hue_light_state\",\"changed\":false,"
                 "\"error\":\"invalid Hue light id\"}");
        return ESP_ERR_INVALID_ARG;
    }

    accessory_hue_config_t config;
    accessory_config_load_hue(&config);
    if (config.bridge_host[0] == '\0' || config.app_key[0] == '\0') {
        snprintf(out, out_len,
                 "{\"type\":\"hue_light_state\",\"changed\":false,"
                 "\"error\":\"Hue Bridge is not paired\"}");
        return ESP_ERR_INVALID_STATE;
    }

    char url[224];
    snprintf(url, sizeof(url), "http://%s/api/%s/lights/%s/state",
             config.bridge_host, config.app_key, light_id);

    char *response = malloc(HUE_PAIR_RESPONSE_SIZE);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = hue_http_request(url, HTTP_METHOD_PUT,
                                     on ? "{\"on\":true}" : "{\"on\":false}",
                                     response, HUE_PAIR_RESPONSE_SIZE, &status);
    if (err != ESP_OK) {
        snprintf(out, out_len,
                 "{\"type\":\"hue_light_state\",\"changed\":false,"
                 "\"light_id\":\"%s\",\"requested_on\":%s,\"http_status\":%d,"
                 "\"error\":\"Hue light state request failed\"}",
                 light_id, on ? "true" : "false", status);
        persistent_log_event("warn", "hue",
                             "light state request failed light_id=%s on=%u err=%s status=%d",
                             light_id, on, esp_err_to_name(err), status);
        free(response);
        return err;
    }

    snprintf(out, out_len,
             "{\"type\":\"hue_light_state\",\"changed\":true,"
             "\"bridge_host\":\"%s\",\"light_id\":\"%s\",\"requested_on\":%s,"
             "\"api\":\"v1\",\"response\":",
             config.bridge_host, light_id, on ? "true" : "false");
    append_json(out, out_len, "%s}", response[0] != '\0' ? response : "[]");
    persistent_log_event("info", "hue", "light state changed light_id=%s on=%u",
                         light_id, on);
    free(response);
    return ESP_OK;
}

esp_err_t hue_integration_clear_pairing_json(char *out, size_t out_len)
{
    esp_err_t err = accessory_config_clear_hue();
    snprintf(out, out_len, "{\"type\":\"hue_clear\",\"cleared\":%s}",
             err == ESP_OK ? "true" : "false");
    if (err == ESP_OK) {
        persistent_log_event("info", "hue", "local Hue pairing cleared");
    }
    return err;
}
