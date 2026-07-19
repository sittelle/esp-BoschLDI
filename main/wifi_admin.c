#include "wifi_admin.h"

#include "accessory_config.h"
#include "automation.h"
#include "ble_gap.h"
#include "hue_integration.h"
#include "live_data_decode.h"
#include "log_store.h"
#include "persistent_log.h"
#include "status_led.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs.h"

#define WIFI_ADMIN_NAMESPACE "wifi_admin"
#define WIFI_ADMIN_SSID_KEY "ssid"
#define WIFI_ADMIN_PASS_KEY "pass"
#define WIFI_ADMIN_HOSTNAME "boschldi"
#define WIFI_ADMIN_AP_PREFIX "BoschLDI-Setup"
#define WIFI_ADMIN_AP_PASS "boschldi"
#define WIFI_ADMIN_CONNECT_TIMEOUT_MS 12000
#define WIFI_ADMIN_MAX_SCAN_RESULTS 16
#define WIFI_ADMIN_LOG_RESPONSE_SIZE 8192
#define WIFI_ADMIN_HUE_RESPONSE_SIZE 2048
#define WIFI_ADMIN_HUE_DEVICES_RESPONSE_SIZE 20000
#define WIFI_ADMIN_HUE_RESOURCES_RESPONSE_SIZE 28000
#define WIFI_ADMIN_AUTOMATION_RESPONSE_SIZE 4096

static const char *TAG = "wifi_admin";
static const EventBits_t WIFI_CONNECTED_BIT = BIT0;
static const EventBits_t WIFI_FAILED_BIT = BIT1;

static EventGroupHandle_t wifi_events;
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;
static httpd_handle_t http_server;
static bool setup_ap_running;
static bool wifi_reconfiguring;
static bool mdns_running;
static int retry_count;
static char current_ssid[33];

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_credentials_t;

typedef struct {
    wifi_credentials_t next;
    wifi_credentials_t previous;
    bool has_previous;
} wifi_reconfigure_request_t;

static void start_setup_ap(void);

static esp_err_t load_credentials(wifi_credentials_t *creds)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_ADMIN_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = sizeof(creds->ssid);
    size_t pass_len = sizeof(creds->pass);
    err = nvs_get_str(nvs, WIFI_ADMIN_SSID_KEY, creds->ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, WIFI_ADMIN_PASS_KEY, creds->pass, &pass_len);
    }
    nvs_close(nvs);

    if (err == ESP_OK && creds->ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    return err;
}

static esp_err_t save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_ADMIN_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open NVS failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, WIFI_ADMIN_SSID_KEY, ssid),
                        TAG, "save ssid failed");
    ESP_RETURN_ON_ERROR(nvs_set_str(nvs, WIFI_ADMIN_PASS_KEY, pass),
                        TAG, "save password failed");
    ESP_RETURN_ON_ERROR(nvs_commit(nvs), TAG, "commit credentials failed");
    nvs_close(nvs);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        current_ssid[0] = '\0';
        if (wifi_reconfiguring) {
            ESP_LOGI(TAG, "Wi-Fi disconnected during reconfiguration");
            return;
        }
        if (retry_count < 5 && !setup_ap_running) {
            retry_count++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "Wi-Fi disconnected; retry=%d", retry_count);
            persistent_log_event("warn", "wifi", "disconnected retry=%d", retry_count);
        } else {
            persistent_log_event("error", "wifi", "connection failed after retries");
            xEventGroupSetBits(wifi_events, WIFI_FAILED_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            strlcpy(current_ssid, (const char *)ap_info.ssid, sizeof(current_ssid));
        }
        retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi connected; ip=" IPSTR, IP2STR(&event->ip_info.ip));
        persistent_log_event("info", "wifi", "connected ip=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void make_ap_ssid(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, out_len, WIFI_ADMIN_AP_PREFIX "-%02X%02X", mac[4], mac[5]);
}

static esp_err_t start_mdns_service(void)
{
    if (mdns_running) {
        return ESP_OK;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed; err=%s", esp_err_to_name(err));
        persistent_log_event("warn", "wifi", "mDNS init failed err=%s", esp_err_to_name(err));
        return err;
    }

    mdns_hostname_set(WIFI_ADMIN_HOSTNAME);
    mdns_instance_name_set("Bosch LDI Accessory");
    mdns_service_add("Bosch LDI Web UI", "_http", "_tcp", 80, NULL, 0);
    mdns_running = true;

    ESP_LOGI(TAG, "mDNS started; url=http://%s.local/", WIFI_ADMIN_HOSTNAME);
    persistent_log_event("info", "wifi", "mDNS started host=%s.local", WIFI_ADMIN_HOSTNAME);
    return ESP_OK;
}

static bool connect_sta(const wifi_credentials_t *creds)
{
    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, creds->ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, creds->pass, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (creds->pass[0] == '\0') {
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_ADMIN_CONNECT_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        return true;
    }

    ESP_LOGW(TAG, "saved Wi-Fi unavailable; starting setup AP");
    esp_wifi_disconnect();
    return false;
}

static esp_err_t apply_sta_credentials(const wifi_credentials_t *creds)
{
    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, creds->ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, creds->pass, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (creds->pass[0] == '\0') {
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t err = ESP_OK;
    err = esp_wifi_set_mode(setup_ap_running ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set Wi-Fi mode failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set Wi-Fi config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_ADMIN_CONNECT_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        err = ESP_ERR_TIMEOUT;
        persistent_log_event("error", "wifi", "reconfigure failed ssid=%s", creds->ssid);
        return err;
    }

    return ESP_OK;
}

static esp_err_t reconnect_sta_transactional(const wifi_reconfigure_request_t *request)
{
    wifi_reconfiguring = true;
    retry_count = 0;
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);

    esp_err_t disconnect_err = esp_wifi_disconnect();
    if (disconnect_err != ESP_OK && disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "disconnect before Wi-Fi reconfigure failed; err=%s",
                 esp_err_to_name(disconnect_err));
    }

    esp_err_t err = apply_sta_credentials(&request->next);
    if (err != ESP_OK) {
        if (request->has_previous) {
            ESP_LOGW(TAG, "new Wi-Fi failed; falling back to previous SSID");
            persistent_log_event("warn", "wifi", "fallback to previous ssid after failed ssid=%s",
                                 request->next.ssid);
            xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
            esp_wifi_disconnect();
            esp_err_t fallback_err = apply_sta_credentials(&request->previous);
            if (fallback_err == ESP_OK) {
                persistent_log_event("info", "wifi", "fallback connected ssid=%s",
                                     request->previous.ssid);
            } else {
                ESP_LOGE(TAG, "fallback Wi-Fi failed; err=%s", esp_err_to_name(fallback_err));
                persistent_log_event("error", "wifi", "fallback failed err=%s",
                                     esp_err_to_name(fallback_err));
            }
        }
        wifi_reconfiguring = false;
        return err;
    }

    err = save_credentials(request->next.ssid, request->next.pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "credential save after successful connect failed: %s", esp_err_to_name(err));
        wifi_reconfiguring = false;
        return err;
    }

    persistent_log_event("info", "wifi", "reconfigured ssid=%s", request->next.ssid);
    if (setup_ap_running) {
        setup_ap_running = false;
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "disable setup AP failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(start_mdns_service());

cleanup:
    wifi_reconfiguring = false;
    return err;
}

static char from_hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 0;
}

static void url_decode(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; si++) {
        if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) &&
            isxdigit((unsigned char)src[si + 2])) {
            dst[di++] = (from_hex(src[si + 1]) << 4) | from_hex(src[si + 2]);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static void form_value(const char *body, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    size_t key_len = strlen(key);
    const char *p = body;
    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            const char *end = strchr(p, '&');
            size_t raw_len = end ? (size_t)(end - p) : strlen(p);
            char raw[96];
            if (raw_len >= sizeof(raw)) {
                raw_len = sizeof(raw) - 1;
            }
            memcpy(raw, p, raw_len);
            raw[raw_len] = '\0';
            url_decode(out, out_len, raw);
            return;
        }
        p = strchr(p, '&');
        if (p != NULL) {
            p++;
        }
    }
}

static void json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; si++) {
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

static void normalize_log_text(char *text)
{
    if (text == NULL) {
        return;
    }

    for (size_t i = 0; text[i] != '\0'; i++) {
        if (text[i] == '\r') {
            text[i] = '\n';
        } else if ((unsigned char)text[i] < 0x20 &&
                   text[i] != '\n' &&
                   text[i] != '\t') {
            text[i] = ' ';
        }
    }
}

static void color_hex(char *out, size_t out_len, uint32_t color)
{
    snprintf(out, out_len, "#%06X", (unsigned)(color & 0x00ffffff));
}

static bool parse_color_hex(const char *text, uint32_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    if (text[0] == '#') {
        text++;
    }
    if (strlen(text) != 6) {
        return false;
    }
    for (size_t i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)text[i])) {
            return false;
        }
    }
    *out = (uint32_t)strtoul(text, NULL, 16) & 0x00ffffff;
    return true;
}

static esp_err_t setup_root_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Bosch LDI Setup</title><style>"
        "body{font-family:system-ui,sans-serif;margin:24px;max-width:620px}"
        "label,input,button,select{font-size:16px}input,select{width:100%;padding:10px;margin:6px 0 14px}"
        "button{padding:10px 14px}.hint{color:#555}</style></head><body>"
        "<h1>Bosch LDI Setup</h1><p class=hint>Setup AP: BoschLDI-Setup. Try boschldi.local or 192.168.4.1.</p>"
        "<form method=post action=/save><label>Wi-Fi network</label><select id=ssid name=ssid></select>"
        "<label>Password</label><input name=pass type=password autocomplete=current-password>"
        "<button type=submit>Save and connect</button></form>"
        "<script>async function load(){let s=document.getElementById('ssid');s.innerHTML='<option>Scanning...</option>';"
        "try{let r=await fetch('/scan');let a=await r.json();s.innerHTML='';"
        "a.forEach(n=>{let o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';s.appendChild(o)});"
        "if(!a.length)s.innerHTML='<option value=\"\">No networks found</option>'}catch(e){s.innerHTML='<option value=\"\">Scan failed</option>'}}load()</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    char *logs = malloc(WIFI_ADMIN_LOG_RESPONSE_SIZE);
    if (logs == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "log buffer allocation failed\n");
    }

    log_store_copy(logs, WIFI_ADMIN_LOG_RESPONSE_SIZE);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = httpd_resp_sendstr(req, logs);
    free(logs);
    return err;
}

static esp_err_t latest_get_handler(httpd_req_t *req)
{
    char latest[384];
    bool has_data = live_data_latest_summary(latest, sizeof(latest));

    httpd_resp_set_type(req, "text/plain");
    if (!has_data) {
        httpd_resp_set_status(req, "204 No Content");
    }
    return httpd_resp_sendstr(req, latest);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char device_name[ACCESSORY_DEVICE_NAME_MAX_LEN + 1];
    accessory_export_config_t export_config;
    accessory_led_config_t led_config;
    wifi_credentials_t saved = {0};
    accessory_config_load_device_name(device_name, sizeof(device_name));
    accessory_config_load_export(&export_config);
    accessory_config_load_led(&led_config);
    esp_err_t saved_err = load_credentials(&saved);

    char escaped_name[64];
    char escaped_logs_url[192];
    char escaped_bike_url[192];
    char escaped_current_ssid[96];
    char escaped_saved_ssid[96];
    json_escape(escaped_name, sizeof(escaped_name), device_name);
    json_escape(escaped_logs_url, sizeof(escaped_logs_url), export_config.logs_url);
    json_escape(escaped_bike_url, sizeof(escaped_bike_url), export_config.bike_url);
    json_escape(escaped_current_ssid, sizeof(escaped_current_ssid), current_ssid);
    json_escape(escaped_saved_ssid, sizeof(escaped_saved_ssid),
                saved_err == ESP_OK ? saved.ssid : "");

    char boot_color[8];
    char advertising_color[8];
    char connected_color[8];
    char secured_color[8];
    char ready_color[8];
    char activity_color[8];
    char error_color[8];
    color_hex(boot_color, sizeof(boot_color), led_config.boot_color);
    color_hex(advertising_color, sizeof(advertising_color), led_config.advertising_color);
    color_hex(connected_color, sizeof(connected_color), led_config.connected_color);
    color_hex(secured_color, sizeof(secured_color), led_config.secured_color);
    color_hex(ready_color, sizeof(ready_color), led_config.ready_color);
    color_hex(activity_color, sizeof(activity_color), led_config.activity_color);
    color_hex(error_color, sizeof(error_color), led_config.error_color);

    char response[1200];
    snprintf(response, sizeof(response),
             "{\"device_name\":\"%s\",\"logs_url\":\"%s\",\"bike_url\":\"%s\","
             "\"logs_interval_sec\":%" PRIu32 ",\"bike_interval_sec\":%" PRIu32 ","
             "\"logs_min_interval_sec\":%u,\"bike_min_interval_sec\":%u,"
             "\"max_interval_sec\":%u,\"current_ssid\":\"%s\",\"saved_ssid\":\"%s\","
             "\"wifi_connected\":%s,"
             "\"led_enabled\":%s,\"led_brightness_percent\":%u,"
             "\"led_boot_color\":\"%s\",\"led_advertising_color\":\"%s\","
             "\"led_connected_color\":\"%s\",\"led_secured_color\":\"%s\","
             "\"led_ready_color\":\"%s\",\"led_activity_color\":\"%s\","
             "\"led_error_color\":\"%s\"}",
             escaped_name, escaped_logs_url, escaped_bike_url,
             export_config.logs_interval_sec, export_config.bike_interval_sec,
             ACCESSORY_EXPORT_LOG_MIN_INTERVAL_SEC,
             ACCESSORY_EXPORT_BIKE_MIN_INTERVAL_SEC,
             ACCESSORY_EXPORT_MAX_INTERVAL_SEC, escaped_current_ssid, escaped_saved_ssid,
             current_ssid[0] != '\0' ? "true" : "false",
             led_config.enabled ? "true" : "false",
             (unsigned)led_config.brightness_percent,
             boot_color, advertising_color, connected_color, secured_color,
             ready_color, activity_color, error_color);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t api_logs_get_handler(httpd_req_t *req)
{
    char *logs = malloc(WIFI_ADMIN_LOG_RESPONSE_SIZE);
    char *escaped_logs = malloc(WIFI_ADMIN_LOG_RESPONSE_SIZE * 2);
    if (logs == NULL || escaped_logs == NULL) {
        free(logs);
        free(escaped_logs);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"log buffer allocation failed\"}");
    }

    size_t bytes = log_store_copy(logs, WIFI_ADMIN_LOG_RESPONSE_SIZE);
    normalize_log_text(logs);
    json_escape(escaped_logs, WIFI_ADMIN_LOG_RESPONSE_SIZE * 2, logs);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"type\":\"logs\",\"boot_ms\":");
    char meta[80];
    snprintf(meta, sizeof(meta), "%lld,\"bytes\":%u,\"logs\":\"",
             (long long)(esp_timer_get_time() / 1000), (unsigned)bytes);
    httpd_resp_sendstr_chunk(req, meta);
    httpd_resp_sendstr_chunk(req, escaped_logs);
    httpd_resp_sendstr_chunk(req, "\"}");
    free(logs);
    free(escaped_logs);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t api_bike_get_handler(httpd_req_t *req)
{
    char bike_json[768];
    bool has_data = live_data_latest_json(bike_json, sizeof(bike_json));
    char response[1024];
    snprintf(response, sizeof(response),
             "{\"type\":\"bike_data\",\"boot_ms\":%lld,\"has_data\":%s,\"data\":%s}",
             (long long)(esp_timer_get_time() / 1000),
             has_data ? "true" : "false",
             has_data ? bike_json : "{}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t api_state_get_handler(httpd_req_t *req)
{
    char bike_json[768];
    char *logs = malloc(WIFI_ADMIN_LOG_RESPONSE_SIZE);
    char *escaped_logs = malloc(WIFI_ADMIN_LOG_RESPONSE_SIZE * 2);
    if (logs == NULL || escaped_logs == NULL) {
        free(logs);
        free(escaped_logs);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"state buffer allocation failed\"}");
    }

    bool has_data = live_data_latest_json(bike_json, sizeof(bike_json));
    size_t bytes = log_store_copy(logs, WIFI_ADMIN_LOG_RESPONSE_SIZE);
    normalize_log_text(logs);
    json_escape(escaped_logs, WIFI_ADMIN_LOG_RESPONSE_SIZE * 2, logs);

    httpd_resp_set_type(req, "application/json");
    char prefix[160];
    snprintf(prefix, sizeof(prefix),
             "{\"type\":\"state\",\"boot_ms\":%lld,\"bike\":{\"has_data\":%s,\"data\":%s},"
             "\"logs\":{\"bytes\":%u,\"logs\":\"",
             (long long)(esp_timer_get_time() / 1000),
             has_data ? "true" : "false", has_data ? bike_json : "{}",
             (unsigned)bytes);
    httpd_resp_sendstr_chunk(req, prefix);
    httpd_resp_sendstr_chunk(req, escaped_logs);
    httpd_resp_sendstr_chunk(req, "\"}}");
    free(logs);
    free(escaped_logs);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t api_automation_rules_get_handler(httpd_req_t *req)
{
    char *response = malloc(WIFI_ADMIN_AUTOMATION_RESPONSE_SIZE);
    if (response == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"automation response allocation failed\"}");
    }

    esp_err_t err = automation_rules_json(response, WIFI_ADMIN_AUTOMATION_RESPONSE_SIZE);
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
    err = httpd_resp_sendstr(req, response);
    free(response);
    return err;
}

static esp_err_t api_automation_rule_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"Invalid form\"}");
    }

    int read_len = httpd_req_recv(req, body, req->content_len);
    if (read_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"Could not read form\"}");
    }
    body[read_len] = '\0';

    automation_rule_t rule = {0};
    char enabled[4];
    char value[24];
    char action_on[8];
    char cooldown[12];
    form_value(body, "enabled", enabled, sizeof(enabled));
    form_value(body, "field", rule.field, sizeof(rule.field));
    form_value(body, "op", rule.op, sizeof(rule.op));
    form_value(body, "value", value, sizeof(value));
    form_value(body, "light_id", rule.light_id, sizeof(rule.light_id));
    form_value(body, "action_on", action_on, sizeof(action_on));
    form_value(body, "cooldown_sec", cooldown, sizeof(cooldown));

    rule.enabled = enabled[0] != '\0';
    rule.value = strtod(value, NULL);
    rule.action_on = strcmp(action_on, "1") == 0 ||
                     strcmp(action_on, "true") == 0 ||
                     strcmp(action_on, "on") == 0;
    rule.cooldown_sec = (uint32_t)strtoul(cooldown, NULL, 10);

    char *response = malloc(WIFI_ADMIN_AUTOMATION_RESPONSE_SIZE);
    if (response == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"automation response allocation failed\"}");
    }

    esp_err_t err = automation_add_rule(&rule, response, WIFI_ADMIN_AUTOMATION_RESPONSE_SIZE);
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    err = httpd_resp_sendstr(req, response);
    free(response);
    return err;
}

static esp_err_t api_automation_clear_post_handler(httpd_req_t *req)
{
    char *response = malloc(WIFI_ADMIN_AUTOMATION_RESPONSE_SIZE);
    if (response == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"automation response allocation failed\"}");
    }

    esp_err_t err = automation_clear_rules(response, WIFI_ADMIN_AUTOMATION_RESPONSE_SIZE);
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
    err = httpd_resp_sendstr(req, response);
    free(response);
    return err;
}

static esp_err_t api_hue_bridges_get_handler(httpd_req_t *req)
{
    char *response = malloc(WIFI_ADMIN_HUE_RESPONSE_SIZE);
    if (response == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"hue response allocation failed\"}");
    }

    esp_err_t err = hue_integration_bridge_discovery_json(response, WIFI_ADMIN_HUE_RESPONSE_SIZE);
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
    err = httpd_resp_sendstr(req, response);
    free(response);
    return err;
}

static esp_err_t send_hue_json_response(httpd_req_t *req, char *response, esp_err_t err)
{
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
    } else if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
    }
    err = httpd_resp_sendstr(req, response);
    free(response);
    return err;
}

static esp_err_t api_hue_status_get_handler(httpd_req_t *req)
{
    char *response = malloc(WIFI_ADMIN_HUE_RESPONSE_SIZE);
    if (response == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"hue response allocation failed\"}");
    }

    esp_err_t err = hue_integration_status_json(response, WIFI_ADMIN_HUE_RESPONSE_SIZE);
    return send_hue_json_response(req, response, err);
}

static esp_err_t api_hue_pair_post_handler(httpd_req_t *req)
{
    char body[128] = {0};
    char bridge_host[ACCESSORY_HUE_HOST_MAX_LEN + 1] = "";
    if (req->content_len > 0) {
        if (req->content_len >= (int)sizeof(body)) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"Invalid form\"}");
        }
        int read_len = httpd_req_recv(req, body, req->content_len);
        if (read_len <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"Could not read form\"}");
        }
        body[read_len] = '\0';
        form_value(body, "bridge_host", bridge_host, sizeof(bridge_host));
    }

    char *response = malloc(WIFI_ADMIN_HUE_RESPONSE_SIZE);
    if (response == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"hue response allocation failed\"}");
    }

    esp_err_t err = hue_integration_pair_json(bridge_host, response, WIFI_ADMIN_HUE_RESPONSE_SIZE);
    return send_hue_json_response(req, response, err);
}

static esp_err_t api_hue_pair_start_post_handler(httpd_req_t *req)
{
    char body[128] = {0};
    char bridge_host[ACCESSORY_HUE_HOST_MAX_LEN + 1] = "";
    if (req->content_len > 0) {
        if (req->content_len >= (int)sizeof(body)) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"Invalid form\"}");
        }
        int read_len = httpd_req_recv(req, body, req->content_len);
        if (read_len <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"Could not read form\"}");
        }
        body[read_len] = '\0';
        form_value(body, "bridge_host", bridge_host, sizeof(bridge_host));
    }

    char *response = malloc(WIFI_ADMIN_HUE_RESPONSE_SIZE);
    if (response == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"hue response allocation failed\"}");
    }

    esp_err_t err = hue_integration_pair_start_json(bridge_host, response, WIFI_ADMIN_HUE_RESPONSE_SIZE);
    return send_hue_json_response(req, response, err);
}

static esp_err_t api_hue_pair_progress_get_handler(httpd_req_t *req)
{
    char *response = malloc(WIFI_ADMIN_HUE_RESPONSE_SIZE);
    if (response == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"hue response allocation failed\"}");
    }

    esp_err_t err = hue_integration_pair_progress_json(response, WIFI_ADMIN_HUE_RESPONSE_SIZE);
    return send_hue_json_response(req, response, err);
}

static esp_err_t api_hue_devices_get_handler(httpd_req_t *req)
{
    char *response = malloc(WIFI_ADMIN_HUE_DEVICES_RESPONSE_SIZE);
    if (response == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"hue response allocation failed\"}");
    }

    esp_err_t err = hue_integration_devices_json(response, WIFI_ADMIN_HUE_DEVICES_RESPONSE_SIZE);
    return send_hue_json_response(req, response, err);
}

static esp_err_t api_hue_light_state_post_handler(httpd_req_t *req)
{
    char body[96] = {0};
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"Invalid form\"}");
    }

    int read_len = httpd_req_recv(req, body, req->content_len);
    if (read_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"Could not read form\"}");
    }
    body[read_len] = '\0';

    char light_id[12];
    char on_value[8];
    form_value(body, "light_id", light_id, sizeof(light_id));
    form_value(body, "on", on_value, sizeof(on_value));
    bool on = strcmp(on_value, "1") == 0 ||
              strcmp(on_value, "true") == 0 ||
              strcmp(on_value, "on") == 0;

    char *response = malloc(WIFI_ADMIN_HUE_RESPONSE_SIZE);
    if (response == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"hue response allocation failed\"}");
    }

    esp_err_t err = hue_integration_set_light_state_json(light_id, on, response,
                                                         WIFI_ADMIN_HUE_RESPONSE_SIZE);
    return send_hue_json_response(req, response, err);
}

static esp_err_t api_hue_resources_get_handler(httpd_req_t *req)
{
    char *response = malloc(WIFI_ADMIN_HUE_RESOURCES_RESPONSE_SIZE);
    if (response == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"hue response allocation failed\"}");
    }

    esp_err_t err = hue_integration_resources_json(response, WIFI_ADMIN_HUE_RESOURCES_RESPONSE_SIZE);
    return send_hue_json_response(req, response, err);
}

static esp_err_t api_hue_clear_post_handler(httpd_req_t *req)
{
    char *response = malloc(WIFI_ADMIN_HUE_RESPONSE_SIZE);
    if (response == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"hue response allocation failed\"}");
    }

    esp_err_t err = hue_integration_clear_pairing_json(response, WIFI_ADMIN_HUE_RESPONSE_SIZE);
    return send_hue_json_response(req, response, err);
}

static esp_err_t send_file_response(httpd_req_t *req, const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "No persistent log file yet.\n");
    }

    httpd_resp_set_type(req, "text/plain");
    char chunk[384];
    while (!feof(f)) {
        size_t read_len = fread(chunk, 1, sizeof(chunk), f);
        if (read_len > 0) {
            esp_err_t err = httpd_resp_send_chunk(req, chunk, read_len);
            if (err != ESP_OK) {
                fclose(f);
                return err;
            }
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t persistent_log_get_handler(httpd_req_t *req)
{
    return send_file_response(req, persistent_log_path_current());
}

static esp_err_t persistent_log_previous_get_handler(httpd_req_t *req)
{
    return send_file_response(req, persistent_log_path_previous());
}

static esp_err_t dashboard_root_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Bosch LDI</title><style>"
        "body{font-family:system-ui,sans-serif;margin:0;background:#f6f7f8;color:#171717}"
        "header{padding:16px 20px;background:#fff;border-bottom:1px solid #ddd}"
        "h1{font-size:20px;margin:0}.meta{color:#555;margin-top:4px}"
        "nav{display:flex;gap:4px;padding:0 20px;background:#fff;border-bottom:1px solid #ddd}"
        ".tabbtn{border:0;background:#fff;border-bottom:3px solid transparent;margin:0;padding:12px 14px;cursor:pointer}"
        ".tabbtn.active{border-bottom-color:#1769aa;color:#0f4f82;font-weight:700}"
        "main{padding:16px 20px}.tab{display:none}.tab.active{display:block}"
        ".label{font-size:13px;font-weight:700;margin:12px 0 6px}"
        ".hint{color:#555;margin:4px 0 10px}"
        "section{margin:0 0 18px}.wifi{display:grid;gap:8px;max-width:520px}"
        "label,input,select{font-size:15px}input,select{padding:8px;width:100%;box-sizing:border-box}"
        ".tools{display:grid;grid-template-columns:minmax(180px,1fr) 140px minmax(130px,220px) auto;gap:8px;align-items:end}"
        ".check{display:flex;gap:6px;align-items:center;white-space:nowrap}.check input{width:auto}"
        ".count{color:#555;font-size:13px;margin:0 0 6px}.empty{color:#aaa}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:10px;margin:10px 0 16px}"
        ".device{background:#fff;border:1px solid #d8dde2;border-radius:6px;padding:12px;display:grid;gap:7px}"
        ".device h3{font-size:16px;margin:0}.row{display:flex;justify-content:space-between;gap:10px;align-items:center}"
        ".muted{color:#666}.badge{font-size:12px;border-radius:999px;padding:2px 8px;background:#edf0f2;color:#333;white-space:nowrap}"
        ".on{background:#d7f4df;color:#125d25}.off{background:#eceff1;color:#4d555c}.warn{background:#ffe8c2;color:#6b4200}"
        ".plan{background:#fff;border-left:4px solid #1769aa;padding:12px 14px;margin:10px 0 16px}"
        ".ledgrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:8px;max-width:780px}"
        ".ledgrid input[type=color]{height:38px;padding:2px}"
        ".tablewrap{overflow:auto;background:#fff;border:1px solid #d8dde2;border-radius:6px;margin:10px 0 16px}"
        "table{width:100%;border-collapse:collapse;min-width:760px}th,td{text-align:left;padding:8px 10px;border-bottom:1px solid #e6e9ec;font-size:14px}th{background:#f1f3f5;font-weight:700}"
        ".doc{max-width:900px}.doc p,.doc li{line-height:1.45}.doc code{background:#e9ecef;padding:1px 4px;border-radius:3px}"
        "pre{white-space:pre-wrap;background:#111;color:#e8e8e8;"
        "padding:14px;border-radius:6px;overflow:auto;font:13px/1.45 ui-monospace,Consolas,monospace}"
        "#latest{min-height:42px;background:#fff;color:#171717;border:1px solid #ddd}"
        "#log{height:58vh;margin-top:0}"
        "@media(max-width:760px){.tools{grid-template-columns:1fr 1fr}.check{align-self:center}}"
        "button{font-size:15px;padding:8px 12px;margin:0 8px 12px 0}</style></head><body>"
        "<header><h1>Bosch LDI</h1><div class=meta>Host: boschldi.local</div></header>"
        "<nav><button class='tabbtn active' data-tab=bike onclick=\"showTab('bike')\">Bike data</button>"
        "<button class=tabbtn data-tab=logs onclick=\"showTab('logs')\">Logs</button>"
        "<button class=tabbtn data-tab=hue onclick=\"showTab('hue')\">Hue</button>"
        "<button id=automation_tab_btn class=tabbtn data-tab=automation onclick=\"showTab('automation')\" style='display:none'>Automation</button>"
        "<button class=tabbtn data-tab=config onclick=\"showTab('config')\">Configuration</button>"
        "<button class=tabbtn data-tab=docs onclick=\"showTab('docs')\">Documentation</button></nav>"
        "<main><section id=bike class='tab active'><button onclick=load()>Refresh</button>"
        "<button onclick=\"location.href='/bike-log'\">Bike file</button>"
        "<button onclick=\"location.href='/bike-log/previous'\">Previous file</button>"
        "<div class=label>Latest bike state</div><pre id=latest>Loading...</pre></section>"
        "<section id=logs class=tab><button onclick=load()>Refresh</button>"
        "<button onclick=\"location.href='/api/logs'\">Logs JSON</button>"
        "<button onclick=\"location.href='/logs'\">Raw runtime logs</button>"
        "<div class=label>Runtime logs</div><div class=tools>"
        "<input id=q placeholder=Search oninput=renderLogs()>"
        "<select id=level onchange=renderLogs()><option value=''>All levels</option><option value=E>Errors</option>"
        "<option value=W>Warnings</option><option value=I>Info</option><option value=D>Debug</option></select>"
        "<input id=tag placeholder='Filter source' oninput=renderLogs()>"
        "<label class=check><input id=follow type=checkbox checked>Follow</label></div>"
        "<div id=count class=count>0 lines</div><pre id=log>Loading...</pre></section>"
        "<section id=config class=tab><div class=label>Accessory device name</div>"
        "<form class=wifi method=post action=/device-name>"
        "<label>Name shown to the bike</label><input id=device_name name=device_name maxlength=24>"
        "<button type=submit>Save name</button></form>"
        "<div class=label>JSON export</div><form class=wifi method=post action=/export-config>"
        "<label>Logs URL</label><input id=logs_url name=logs_url placeholder='https://example.com/logs'>"
        "<label>Logs interval seconds</label><input id=logs_interval_sec name=logs_interval_sec type=number min=60 max=3600>"
        "<label>Bike data URL</label><input id=bike_url name=bike_url placeholder='https://example.com/bike'>"
        "<label>Bike data interval seconds</label><input id=bike_interval_sec name=bike_interval_sec type=number min=10 max=3600>"
        "<button type=submit>Save export</button></form>"
        "<div class=label>Internal RGB LED</div><form class=wifi method=post action=/led-config>"
        "<label class=check><input id=led_enabled name=led_enabled type=checkbox value=1>Enabled</label>"
        "<label>Brightness percent</label><input id=led_brightness_percent name=led_brightness_percent type=number min=1 max=100>"
        "<div class=ledgrid>"
        "<label>Boot<input id=led_boot_color name=led_boot_color type=color></label>"
        "<label>Bike pairing<input id=led_advertising_color name=led_advertising_color type=color></label>"
        "<label>Bike connected<input id=led_connected_color name=led_connected_color type=color></label>"
        "<label>Secured<input id=led_secured_color name=led_secured_color type=color></label>"
        "<label>Live data ready<input id=led_ready_color name=led_ready_color type=color></label>"
        "<label>Bike data activity<input id=led_activity_color name=led_activity_color type=color></label>"
        "<label>Error<input id=led_error_color name=led_error_color type=color></label>"
        "</div><button type=submit>Save LED</button></form>"
        "<div class=label>Wi-Fi</div><div id=wifi_status class=hint>Loading Wi-Fi status...</div>"
        "<button id=wifi_change_btn onclick=showWifiChange()>Change Wi-Fi</button>"
        "<form id=wifi_form class=wifi method=post action=/save onsubmit='return confirmWifiChange()' style='display:none'>"
        "<label>Network</label><select id=ssid name=ssid><option>Press Change Wi-Fi to scan</option></select>"
        "<label>Password</label><input name=pass type=password autocomplete=current-password>"
        "<button type=submit>Try and save if successful</button></form><div id=wifi_msg class=hint></div>"
        "</section><section id=hue class=tab><div class=label>Philips Hue</div>"
        "<div id=hue_status class=hint>Loading Hue status...</div>"
        "<div class=actions><button onclick=discoverHue()>Discover Bridges</button>"
        "<button onclick=startHuePairing()>Pair Bridge</button><button onclick=loadHueDevices()>Load Devices</button>"
        "<button onclick=clearHue()>Clear Pairing</button></div>"
        "<p class=hint>Click Pair Bridge, then press the physical Hue Bridge button. The ESP checks automatically for about one minute. Leave host empty to use the first discovered bridge.</p>"
        "<label>Bridge host</label><input id=hue_host placeholder='auto or bridge IP'>"
        "<div id=hue_devices class=tablewrap></div><pre id=hue_output>Hue Bridge discovery uses mDNS. Pairing stores a local Hue app key in NVS.</pre></section>"
        "<section id=automation class=tab><div class=label>Bike to Hue automation</div>"
        "<form class=wifi onsubmit='return addAutomationRule(event)'>"
        "<label><input id=auto_enabled name=enabled type=checkbox checked>Enabled</label>"
        "<label>If bike field</label><select id=auto_field name=field>"
        "<option value=speed_kmh>Speed km/h</option><option value=cadence_rpm>Cadence rpm</option>"
        "<option value=rider_power_w>Rider power W</option><option value=ambient_brightness_lux>Ambient brightness lux</option>"
        "<option value=battery_soc>Battery %</option><option value=odometer_m>Odometer m</option>"
        "<option value=bike_light>Bike light: off=1, on=2</option><option value=system_locked>System locked</option>"
        "<option value=charger_connected>Charger connected</option><option value=light_reserve_state>Light reserve</option>"
        "<option value=diagnosis_program_active>Diagnosis active</option><option value=bike_not_driving>Bike not driving</option>"
        "</select><label>Operator</label><select id=auto_op name=op><option>&lt;</option><option>&lt;=</option><option>==</option><option>&gt;=</option><option>&gt;</option></select>"
        "<label>Value</label><input id=auto_value name=value type=number step=0.001 value=1>"
        "<label>Hue device</label><select id=auto_light_id name=light_id><option value=''>Load Hue devices first</option></select>"
        "<label>Action</label><select id=auto_action_on name=action_on><option value=true>Turn on</option><option value=false>Turn off</option></select>"
        "<label>Cooldown seconds</label><input id=auto_cooldown_sec name=cooldown_sec type=number min=5 max=3600 value=30>"
        "<button type=submit>Add rule</button></form><button onclick=clearAutomationRules()>Clear rules</button>"
        "<div id=automation_rules class=grid></div><pre id=automation_preview>Rules run only when fresh bike data arrives.</pre></section>"
        "<section id=docs class='tab doc'><div class=label>How it works</div>"
        "<p>This ESP32 advertises itself over BLE as a Bosch Live Data accessory. After the bike pairs, the ESP subscribes to Live Data notifications, decodes known fields, keeps the latest state in RAM, and writes a small rotated persistent bike log to flash.</p>"
        "<p>The web UI is served by the ESP over Wi-Fi. The network hostname stays <code>boschldi.local</code>. If saved Wi-Fi is unavailable, the setup access point starts and the setup page is available at <code>192.168.4.1</code>.</p>"
        "<div class=label>Read APIs</div><ul>"
        "<li><code>GET /api/bike</code> latest decoded bike state as JSON.</li>"
        "<li><code>GET /api/logs</code> runtime log ring as JSON.</li>"
        "<li><code>GET /api/state</code> bike state and runtime logs in one JSON response.</li>"
        "<li><code>GET /api/automation/rules</code> current bike-to-Hue rules.</li>"
        "<li><code>GET /api/hue/bridges</code> discover Hue Bridges on the LAN with mDNS.</li>"
        "<li><code>GET /api/hue/status</code> local Hue pairing status without exposing the app key.</li>"
        "<li><code>POST /api/hue/pair</code> optional form field <code>bridge_host</code>. Press the Bridge button first.</li>"
        "<li><code>POST /api/hue/pair/start</code> starts a one-minute background pairing window.</li>"
        "<li><code>GET /api/hue/pair/progress</code> current background pairing status.</li>"
        "<li><code>GET /api/hue/devices</code> Hue v1 <code>lights</code> resource, including lamps and smart plugs exposed as controllable lights.</li>"
        "<li><code>POST /api/hue/light/state</code> form fields <code>light_id</code> and <code>on</code>; internal automation action endpoint.</li>"
        "<li><code>GET /api/hue/resources</code> full Hue v1 bridge state for debugging.</li>"
        "<li><code>GET /config</code> current device name, push export, LED, and Wi-Fi settings.</li>"
        "<li><code>GET /scan</code> nearby Wi-Fi networks.</li>"
        "<li><code>GET /bike-log</code> current rotated persistent bike log.</li>"
        "<li><code>GET /bike-log/previous</code> previous rotated persistent bike log.</li></ul>"
        "<div class=label>Configuration APIs</div><ul>"
        "<li><code>POST /device-name</code> form field <code>device_name</code>. Changes the BLE accessory name shown to the bike, not DNS.</li>"
        "<li><code>POST /save</code> form fields <code>ssid</code> and <code>pass</code>. Tries the new Wi-Fi without reboot, saves only after success, and falls back to the previous saved Wi-Fi on failure.</li>"
        "<li><code>POST /export-config</code> form fields <code>logs_url</code>, <code>logs_interval_sec</code>, <code>bike_url</code>, <code>bike_interval_sec</code>. Empty URLs disable push export.</li>"
        "<li><code>POST /api/automation/rule</code> form fields <code>enabled</code>, <code>field</code>, <code>op</code>, <code>value</code>, <code>light_id</code>, <code>action_on</code>, <code>cooldown_sec</code>.</li>"
        "<li><code>POST /api/automation/clear</code> removes all automation rules.</li></ul>"
        "<div class=label>Polling</div><p>Clients can poll <code>/api/bike</code> every 1-2 seconds. Poll <code>/api/logs</code> only while a user is watching logs. Push export remains optional and bounded by firmware interval limits.</p>"
        "</section></main>"
        "<script>let rawLogs='';let huePaired=false;function showTab(id){document.querySelectorAll('.tab').forEach(x=>x.classList.toggle('active',x.id==id));"
        "document.querySelectorAll('.tabbtn').forEach(x=>x.classList.toggle('active',x.dataset.tab==id));if(id=='logs')renderLogs();if(id=='config')loadConfig();if(id=='hue'){loadHueStatus();if(huePaired)loadHueDevices()}if(id=='automation')loadAutomation()}"
        "function logLevel(x){let m=x.match(/^[EWIDV]\\s*\\(/);if(m)return m[0][0];"
        "m=x.match(/\\b(error|warn|info|debug)\\b/i);return m?m[1][0].toUpperCase():''}"
        "function logTag(x){let m=x.match(/^[EWIDV]\\s*\\([^)]*\\)\\s+([^:]+):/);return m?m[1].toLowerCase():x.toLowerCase()}"
        "function renderLogs(){let e=document.getElementById('log'),q=document.getElementById('q').value.toLowerCase(),"
        "lv=document.getElementById('level').value,t=document.getElementById('tag').value.toLowerCase();"
        "let lines=rawLogs.replace(/\\r/g,'\\n').split('\\n').filter(x=>x.length);let shown=lines.filter(x=>(!q||x.toLowerCase().includes(q))&&"
        "(!lv||logLevel(x)==lv)&&(!t||logTag(x).includes(t)));"
        "e.textContent=shown.join('\\n')||(rawLogs?'No matching log lines.':'No runtime logs yet.');"
        "e.className=shown.length?'':'empty';document.getElementById('count').textContent=shown.length+' / '+lines.length+' lines';"
        "if(document.getElementById('follow').checked)e.scrollTop=e.scrollHeight}"
        "async function load(){let l=document.getElementById('latest');"
        "try{let r=await fetch('/api/bike',{cache:'no-store'});let b=await r.json();"
        "l.textContent=b.has_data?JSON.stringify(b.data,null,2):'No bike data received yet.'}"
        "catch(x){l.textContent='Could not load latest state'}"
        "try{let r=await fetch('/api/logs',{cache:'no-store'});let j=await r.json();rawLogs=j.logs||'';renderLogs()}"
        "catch(x){rawLogs='';document.getElementById('log').textContent='Could not load logs'}}"
        "async function scan(){let s=document.getElementById('ssid');document.getElementById('wifi_msg').textContent='Scanning nearby networks...';try{let r=await fetch('/scan',{cache:'no-store'});"
        "let a=await r.json();s.innerHTML='';a.forEach(n=>{let o=document.createElement('option');"
        "o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';s.appendChild(o)});"
        "if(!a.length)s.innerHTML='<option value=\"\">No networks found</option>';document.getElementById('wifi_msg').textContent='Select a network. The ESP will test it before saving.'}"
        "catch(e){s.innerHTML='<option value=\"\">Scan failed</option>';document.getElementById('wifi_msg').textContent='Wi-Fi scan failed.'}}"
        "function showWifiChange(){document.getElementById('wifi_form').style.display='grid';scan()}"
        "function confirmWifiChange(){let s=document.getElementById('ssid').value;"
        "return confirm('The ESP32 will now leave the current Wi-Fi and try '+s+'. If it cannot connect, it will fall back to the previous saved Wi-Fi. Continue?')}"
        "async function loadConfig(){try{let r=await fetch('/config',{cache:'no-store'});let c=await r.json();"
        "document.getElementById('device_name').value=c.device_name||'';"
        "document.getElementById('wifi_status').textContent=c.wifi_connected?'Connected to Wi-Fi: '+c.current_ssid:'Wi-Fi is not connected. Saved network: '+(c.saved_ssid||'none');"
        "document.getElementById('logs_url').value=c.logs_url||'';document.getElementById('bike_url').value=c.bike_url||'';"
        "let li=document.getElementById('logs_interval_sec'),bi=document.getElementById('bike_interval_sec');"
        "li.min=c.logs_min_interval_sec||60;bi.min=c.bike_min_interval_sec||10;li.max=bi.max=c.max_interval_sec||3600;"
        "li.value=c.logs_interval_sec||li.min;bi.value=c.bike_interval_sec||bi.min;"
        "document.getElementById('led_enabled').checked=!!c.led_enabled;"
        "document.getElementById('led_brightness_percent').value=c.led_brightness_percent||80;"
        "['boot','advertising','connected','secured','ready','activity','error'].forEach(k=>{let v=c['led_'+k+'_color'];if(v)document.getElementById('led_'+k+'_color').value=v})}catch(e){}}"
        "async function loadHueStatus(){try{let r=await fetch('/api/hue/status',{cache:'no-store'});let j=await r.json();huePaired=!!j.paired;"
        "document.getElementById('automation_tab_btn').style.display=huePaired?'block':'none';"
        "document.getElementById('hue_status').textContent=huePaired?'Connected to Hue Bridge: '+bridgeLabel(j):'Hue Bridge is not paired.';"
        "if(j.bridge_host)document.getElementById('hue_host').value=j.bridge_host}catch(e){document.getElementById('hue_status').textContent='Could not load Hue status'}}"
        "function bridgeLabel(j){let n=j.bridge_name||j.instance||'';let h=j.bridge_host||j.ip||j.hostname||'';return n?(n+(h?' ('+h+')':'')):(h||'unknown')}"
        "function hueDeviceRow(id,d){let reachable=!d.state||d.state.reachable!==false,on=!!(d.state&&d.state.on),plug=(d.type||'').toLowerCase().includes('plug')||(d.productname||'').toLowerCase().includes('plug');"
        "let state=reachable?(on?'On':'Off'):'Unreachable',cls=reachable?(on?'on':'off'):'warn',kind=plug?'Smart plug':(d.productname||d.type||'Light');"
        "let bri=d.state&&d.state.bri!=null?Math.round(d.state.bri/254*100)+'%':'-';"
        "return '<tr><td><strong>'+escapeHtml(d.name||('Device '+id))+'</strong></td><td><span class=\"badge '+cls+'\">'+state+'</span></td>'"
        "+'<td>'+escapeHtml(kind)+'</td><td><code>'+escapeHtml(id)+'</code></td><td>'+escapeHtml(d.type||'-')+'</td><td>'+bri+'</td><td>'+escapeHtml(d.modelid||'-')+'</td></tr>'}"
        "function escapeHtml(s){let e=document.createElement('div');e.textContent=String(s);return e.innerHTML}"
        "function isPlug(d){return (d.type||'').toLowerCase().includes('plug')||(d.productname||'').toLowerCase().includes('plug')}"
        "function renderHueDevices(j){let box=document.getElementById('hue_devices'),data=j.data||{},ids=Object.keys(data).sort((a,b)=>isPlug(data[b])-isPlug(data[a])||String(data[a].name||a).localeCompare(String(data[b].name||b)));"
        "box.innerHTML=ids.length?'<table><thead><tr><th>Name</th><th>State</th><th>Kind</th><th>Hue id</th><th>Type</th><th>Brightness</th><th>Model</th></tr></thead><tbody>'+ids.map(id=>hueDeviceRow(id,data[id])).join('')+'</tbody></table>':'<div class=device>No Hue devices returned by the Bridge.</div>';renderAutomationDeviceOptions(data)}"
        "function renderAutomationDeviceOptions(data){let s=document.getElementById('auto_light_id');if(!s)return;let ids=Object.keys(data||{}).sort((a,b)=>isPlug(data[b])-isPlug(data[a])||String(data[a].name||a).localeCompare(String(data[b].name||b)));"
        "s.innerHTML=ids.length?'':'<option value=\"\">No Hue devices loaded</option>';ids.forEach(id=>{let o=document.createElement('option');o.value=id;o.textContent=(data[id].name||('Device '+id))+' (#'+id+')';s.appendChild(o)})}"
        "async function discoverHue(){let h=document.getElementById('hue_output');h.textContent='Scanning...';"
        "try{let r=await fetch('/api/hue/bridges',{cache:'no-store'});let j=await r.json();"
        "if(j.bridges&&j.bridges[0]){document.getElementById('hue_host').value=j.bridges[0].ip||j.bridges[0].hostname||'';"
        "h.textContent='Found '+bridgeLabel(j.bridges[0])+'\\n\\n'+JSON.stringify(j,null,2)}else h.textContent='No Hue Bridge discovered.'}"
        "catch(e){h.textContent='Hue discovery failed'}}"
        "let huePairTimer=null;"
        "async function pollHuePair(){let h=document.getElementById('hue_output');try{let r=await fetch('/api/hue/pair/progress',{cache:'no-store'});"
        "let j=await r.json();h.textContent=JSON.stringify(j,null,2);if(j.paired){huePaired=true;loadHueStatus();loadHueDevices()}if(!j.running&&huePairTimer){clearInterval(huePairTimer);huePairTimer=null}}"
        "catch(e){h.textContent='Hue pairing progress failed';if(huePairTimer){clearInterval(huePairTimer);huePairTimer=null}}}"
        "async function startHuePairing(){let h=document.getElementById('hue_output');h.textContent='Press the Hue Bridge button now. The ESP is checking automatically...';"
        "let b=new URLSearchParams();b.set('bridge_host',document.getElementById('hue_host').value);"
        "try{let r=await fetch('/api/hue/pair/start',{method:'POST',body:b});let j=await r.json();h.textContent=JSON.stringify(j,null,2);"
        "if(huePairTimer)clearInterval(huePairTimer);huePairTimer=setInterval(pollHuePair,2000);setTimeout(pollHuePair,600)}"
        "catch(e){h.textContent='Hue pairing start failed'}}"
        "async function loadHueDevices(){let h=document.getElementById('hue_output');h.textContent='Loading Hue devices...';"
        "try{let r=await fetch('/api/hue/devices',{cache:'no-store'});let j=await r.json();renderHueDevices(j);h.textContent='Loaded '+Object.keys(j.data||{}).length+' Hue devices from '+bridgeLabel(j)+'.'}"
        "catch(e){h.textContent='Hue device load failed'}}"
        "function ruleHtml(r,i){return '<div class=device><div class=row><h3>Rule '+(i+1)+'</h3><span class=\"badge '+(r.enabled?'on':'off')+'\">'+(r.enabled?'Enabled':'Off')+'</span></div>'"
        "+'<div>If <strong>'+escapeHtml(r.field)+'</strong> '+escapeHtml(r.op)+' '+Number(r.value).toFixed(3)+'</div>'"
        "+'<div>Then Hue #'+escapeHtml(r.light_id)+' '+(r.action_on?'on':'off')+'</div><div class=muted>Cooldown '+r.cooldown_sec+'s</div></div>'}"
        "async function loadAutomation(){if(huePaired)loadHueDevices();let p=document.getElementById('automation_preview'),box=document.getElementById('automation_rules');"
        "try{let r=await fetch('/api/automation/rules',{cache:'no-store'});let j=await r.json();box.innerHTML=(j.rules||[]).length?j.rules.map(ruleHtml).join(''):'<div class=device>No automation rules yet.</div>';p.textContent='Rules run when new bike data arrives. Boolean fields use 0=false and 1=true.'}"
        "catch(e){p.textContent='Could not load automation rules'}}"
        "async function addAutomationRule(ev){ev.preventDefault();let p=document.getElementById('automation_preview'),b=new URLSearchParams();"
        "['field','op','value','light_id','cooldown_sec'].forEach(k=>b.set(k,document.getElementById('auto_'+k).value));"
        "b.set('enabled',document.getElementById('auto_enabled').checked?'1':'');b.set('action_on',document.getElementById('auto_action_on').value);"
        "try{let r=await fetch('/api/automation/rule',{method:'POST',body:b});let j=await r.json();p.textContent=JSON.stringify(j,null,2);loadAutomation()}catch(e){p.textContent='Rule save failed'}return false}"
        "async function clearAutomationRules(){let p=document.getElementById('automation_preview');try{let r=await fetch('/api/automation/clear',{method:'POST'});let j=await r.json();p.textContent=JSON.stringify(j,null,2);loadAutomation()}catch(e){p.textContent='Rule clear failed'}}"
        "async function clearHue(){let h=document.getElementById('hue_output');h.textContent='Clearing Hue pairing...';"
        "try{let r=await fetch('/api/hue/clear',{method:'POST'});let j=await r.json();h.textContent=JSON.stringify(j,null,2);huePaired=false;document.getElementById('automation_tab_btn').style.display='none';document.getElementById('hue_devices').innerHTML=''}"
        "catch(e){h.textContent='Hue pairing clear failed'}}"
        "load();loadConfig();loadHueStatus();setInterval(load,3000)</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed; err=%s", esp_err_to_name(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "[]");
    }

    uint16_t count = WIFI_ADMIN_MAX_SCAN_RESULTS;
    wifi_ap_record_t records[WIFI_ADMIN_MAX_SCAN_RESULTS] = {0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_records(&count, records));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (uint16_t i = 0; i < count; i++) {
        char escaped_ssid[96];
        json_escape(escaped_ssid, sizeof(escaped_ssid), (const char *)records[i].ssid);
        char item[128];
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                 i == 0 ? "" : ",", escaped_ssid, records[i].rssi);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void wifi_reconfigure_task(void *arg)
{
    wifi_reconfigure_request_t *request = (wifi_reconfigure_request_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = reconnect_sta_transactional(request);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi reconfigure failed; previous network restored if available; err=%s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi reconfigure complete; url=http://%s.local/", WIFI_ADMIN_HOSTNAME);
    }
    free(request);
    vTaskDelete(NULL);
}

static void device_name_apply_task(void *arg)
{
    char *device_name = (char *)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    app_ble_gap_set_device_name(device_name);
    free(device_name);
    vTaskDelete(NULL);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[192] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid form");
    }

    int read_len = httpd_req_recv(req, body, total);
    if (read_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Could not read form");
    }
    body[read_len] = '\0';

    char ssid[33];
    char pass[65];
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value(body, "pass", pass, sizeof(pass));
    if (ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "SSID required");
    }

    wifi_reconfigure_request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Reconnect task allocation failed");
    }
    strlcpy(request->next.ssid, ssid, sizeof(request->next.ssid));
    strlcpy(request->next.pass, pass, sizeof(request->next.pass));
    request->has_previous = load_credentials(&request->previous) == ESP_OK;

    BaseType_t task_ok = xTaskCreate(wifi_reconfigure_task, "wifi_reconfig", 4096, request, 3, NULL);
    if (task_ok != pdPASS) {
        free(request);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Reconnect task start failed");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Trying new Wi-Fi without saving it yet. If it fails, the ESP32 falls back to the previous saved network.");
    return ESP_OK;
}

static esp_err_t device_name_post_handler(httpd_req_t *req)
{
    char body[96] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid form");
    }

    int read_len = httpd_req_recv(req, body, total);
    if (read_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Could not read form");
    }
    body[read_len] = '\0';

    char device_name[ACCESSORY_DEVICE_NAME_MAX_LEN + 1];
    form_value(body, "device_name", device_name, sizeof(device_name));
    if (!accessory_config_device_name_is_valid(device_name)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Use 1-24 printable ASCII characters.");
    }

    esp_err_t err = accessory_config_save_device_name(device_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device name save failed; err=%s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Device name save failed");
    }

    char *apply_name = calloc(1, ACCESSORY_DEVICE_NAME_MAX_LEN + 1);
    if (apply_name == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Device name saved, but apply task allocation failed");
    }
    strlcpy(apply_name, device_name, ACCESSORY_DEVICE_NAME_MAX_LEN + 1);

    BaseType_t task_ok = xTaskCreate(device_name_apply_task, "ble_name_apply",
                                     3072, apply_name, 3, NULL);
    if (task_ok != pdPASS) {
        free(apply_name);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Device name saved, but apply task start failed");
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Device name saved. DNS hostname remains boschldi.local.");
}

static esp_err_t export_config_post_handler(httpd_req_t *req)
{
    char body[384] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid form");
    }

    int read_len = httpd_req_recv(req, body, total);
    if (read_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Could not read form");
    }
    body[read_len] = '\0';

    accessory_export_config_t config = {0};
    char logs_interval[12];
    char bike_interval[12];
    form_value(body, "logs_url", config.logs_url, sizeof(config.logs_url));
    form_value(body, "bike_url", config.bike_url, sizeof(config.bike_url));
    form_value(body, "logs_interval_sec", logs_interval, sizeof(logs_interval));
    form_value(body, "bike_interval_sec", bike_interval, sizeof(bike_interval));

    config.logs_interval_sec = accessory_config_clamp_logs_interval((uint32_t)strtoul(logs_interval, NULL, 10));
    config.bike_interval_sec = accessory_config_clamp_bike_interval((uint32_t)strtoul(bike_interval, NULL, 10));
    if (!accessory_config_export_url_is_valid(config.logs_url) ||
        !accessory_config_export_url_is_valid(config.bike_url)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "URLs must be empty or start with http:// or https://.");
    }

    esp_err_t err = accessory_config_save_export(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "export config save failed; err=%s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Export configuration save failed");
    }

    persistent_log_event("info", "telemetry",
                         "export config saved logs_enabled=%u bike_enabled=%u logs_sec=%u bike_sec=%u",
                         config.logs_url[0] != '\0', config.bike_url[0] != '\0',
                         (unsigned)config.logs_interval_sec, (unsigned)config.bike_interval_sec);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Export configuration saved.");
}

static esp_err_t led_config_post_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid form");
    }

    int read_len = httpd_req_recv(req, body, total);
    if (read_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Could not read form");
    }
    body[read_len] = '\0';

    accessory_led_config_t config = {0};
    char enabled[4];
    char brightness[8];
    char boot_color[8];
    char advertising_color[8];
    char connected_color[8];
    char secured_color[8];
    char ready_color[8];
    char activity_color[8];
    char error_color[8];
    form_value(body, "led_enabled", enabled, sizeof(enabled));
    form_value(body, "led_brightness_percent", brightness, sizeof(brightness));
    form_value(body, "led_boot_color", boot_color, sizeof(boot_color));
    form_value(body, "led_advertising_color", advertising_color, sizeof(advertising_color));
    form_value(body, "led_connected_color", connected_color, sizeof(connected_color));
    form_value(body, "led_secured_color", secured_color, sizeof(secured_color));
    form_value(body, "led_ready_color", ready_color, sizeof(ready_color));
    form_value(body, "led_activity_color", activity_color, sizeof(activity_color));
    form_value(body, "led_error_color", error_color, sizeof(error_color));

    config.enabled = enabled[0] != '\0';
    config.brightness_percent = accessory_config_clamp_led_brightness((uint32_t)strtoul(brightness, NULL, 10));
    if (!parse_color_hex(boot_color, &config.boot_color) ||
        !parse_color_hex(advertising_color, &config.advertising_color) ||
        !parse_color_hex(connected_color, &config.connected_color) ||
        !parse_color_hex(secured_color, &config.secured_color) ||
        !parse_color_hex(ready_color, &config.ready_color) ||
        !parse_color_hex(activity_color, &config.activity_color) ||
        !parse_color_hex(error_color, &config.error_color)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "LED colors must be #RRGGBB values.");
    }

    esp_err_t err = accessory_config_save_led(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED config save failed; err=%s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "LED configuration save failed");
    }

    status_led_reload_config();
    persistent_log_event("info", "status_led", "LED config saved enabled=%u brightness=%u",
                         config.enabled, (unsigned)config.brightness_percent);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "LED configuration saved.");
}

static esp_err_t start_http_server(bool setup_mode)
{
    if (http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.max_uri_handlers = 34;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&http_server, &config), TAG, "HTTP start failed");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = setup_mode ? setup_root_get_handler : dashboard_root_get_handler,
    };
    httpd_register_uri_handler(http_server, &root);

    const httpd_uri_t logs = {
        .uri = "/logs",
        .method = HTTP_GET,
        .handler = logs_get_handler,
    };
    httpd_register_uri_handler(http_server, &logs);

    const httpd_uri_t latest = {
        .uri = "/latest",
        .method = HTTP_GET,
        .handler = latest_get_handler,
    };
    httpd_register_uri_handler(http_server, &latest);

    const httpd_uri_t config_uri = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
    };
    httpd_register_uri_handler(http_server, &config_uri);

    const httpd_uri_t api_logs = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = api_logs_get_handler,
    };
    httpd_register_uri_handler(http_server, &api_logs);

    const httpd_uri_t api_bike = {
        .uri = "/api/bike",
        .method = HTTP_GET,
        .handler = api_bike_get_handler,
    };
    httpd_register_uri_handler(http_server, &api_bike);

    const httpd_uri_t api_state = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = api_state_get_handler,
    };
    httpd_register_uri_handler(http_server, &api_state);

    const httpd_uri_t api_automation_rules = {
        .uri = "/api/automation/rules",
        .method = HTTP_GET,
        .handler = api_automation_rules_get_handler,
    };
    httpd_register_uri_handler(http_server, &api_automation_rules);

    const httpd_uri_t api_automation_rule = {
        .uri = "/api/automation/rule",
        .method = HTTP_POST,
        .handler = api_automation_rule_post_handler,
    };
    httpd_register_uri_handler(http_server, &api_automation_rule);

    const httpd_uri_t api_automation_clear = {
        .uri = "/api/automation/clear",
        .method = HTTP_POST,
        .handler = api_automation_clear_post_handler,
    };
    httpd_register_uri_handler(http_server, &api_automation_clear);

    const httpd_uri_t api_hue_bridges = {
        .uri = "/api/hue/bridges",
        .method = HTTP_GET,
        .handler = api_hue_bridges_get_handler,
    };
    httpd_register_uri_handler(http_server, &api_hue_bridges);

    const httpd_uri_t api_hue_status = {
        .uri = "/api/hue/status",
        .method = HTTP_GET,
        .handler = api_hue_status_get_handler,
    };
    httpd_register_uri_handler(http_server, &api_hue_status);

    const httpd_uri_t api_hue_pair = {
        .uri = "/api/hue/pair",
        .method = HTTP_POST,
        .handler = api_hue_pair_post_handler,
    };
    httpd_register_uri_handler(http_server, &api_hue_pair);

    const httpd_uri_t api_hue_pair_start = {
        .uri = "/api/hue/pair/start",
        .method = HTTP_POST,
        .handler = api_hue_pair_start_post_handler,
    };
    httpd_register_uri_handler(http_server, &api_hue_pair_start);

    const httpd_uri_t api_hue_pair_progress = {
        .uri = "/api/hue/pair/progress",
        .method = HTTP_GET,
        .handler = api_hue_pair_progress_get_handler,
    };
    httpd_register_uri_handler(http_server, &api_hue_pair_progress);

    const httpd_uri_t api_hue_devices = {
        .uri = "/api/hue/devices",
        .method = HTTP_GET,
        .handler = api_hue_devices_get_handler,
    };
    httpd_register_uri_handler(http_server, &api_hue_devices);

    const httpd_uri_t api_hue_light_state = {
        .uri = "/api/hue/light/state",
        .method = HTTP_POST,
        .handler = api_hue_light_state_post_handler,
    };
    httpd_register_uri_handler(http_server, &api_hue_light_state);

    const httpd_uri_t api_hue_resources = {
        .uri = "/api/hue/resources",
        .method = HTTP_GET,
        .handler = api_hue_resources_get_handler,
    };
    httpd_register_uri_handler(http_server, &api_hue_resources);

    const httpd_uri_t api_hue_clear = {
        .uri = "/api/hue/clear",
        .method = HTTP_POST,
        .handler = api_hue_clear_post_handler,
    };
    httpd_register_uri_handler(http_server, &api_hue_clear);

    const httpd_uri_t bike_log = {
        .uri = "/bike-log",
        .method = HTTP_GET,
        .handler = persistent_log_get_handler,
    };
    httpd_register_uri_handler(http_server, &bike_log);

    const httpd_uri_t bike_log_previous = {
        .uri = "/bike-log/previous",
        .method = HTTP_GET,
        .handler = persistent_log_previous_get_handler,
    };
    httpd_register_uri_handler(http_server, &bike_log_previous);

    const httpd_uri_t scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = scan_get_handler,
    };
    const httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };
    const httpd_uri_t device_name = {
        .uri = "/device-name",
        .method = HTTP_POST,
        .handler = device_name_post_handler,
    };
    const httpd_uri_t export_config = {
        .uri = "/export-config",
        .method = HTTP_POST,
        .handler = export_config_post_handler,
    };
    const httpd_uri_t led_config = {
        .uri = "/led-config",
        .method = HTTP_POST,
        .handler = led_config_post_handler,
    };
    httpd_register_uri_handler(http_server, &scan);
    httpd_register_uri_handler(http_server, &save);
    httpd_register_uri_handler(http_server, &device_name);
    httpd_register_uri_handler(http_server, &export_config);
    httpd_register_uri_handler(http_server, &led_config);

    if (!setup_mode) {
        ESP_LOGI(TAG, "log web service started; url=http://%s.local/ or device IP", WIFI_ADMIN_HOSTNAME);
        return ESP_OK;
    }

    const httpd_uri_t captive = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = setup_root_get_handler,
    };
    httpd_register_uri_handler(http_server, &captive);
    return ESP_OK;
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "DNS socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        ESP_LOGW(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t rx[256];
    uint8_t tx[320];
    while (true) {
        struct sockaddr_in source;
        socklen_t socklen = sizeof(source);
        int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&source, &socklen);
        if (len < 12) {
            continue;
        }

        memcpy(tx, rx, len);
        tx[2] = 0x81;
        tx[3] = 0x80;
        tx[6] = 0x00;
        tx[7] = 0x01;
        tx[8] = 0x00;
        tx[9] = 0x00;
        tx[10] = 0x00;
        tx[11] = 0x00;

        int pos = len;
        if (pos + 16 > (int)sizeof(tx)) {
            continue;
        }
        tx[pos++] = 0xc0;
        tx[pos++] = 0x0c;
        tx[pos++] = 0x00;
        tx[pos++] = 0x01;
        tx[pos++] = 0x00;
        tx[pos++] = 0x01;
        tx[pos++] = 0x00;
        tx[pos++] = 0x00;
        tx[pos++] = 0x00;
        tx[pos++] = 0x3c;
        tx[pos++] = 0x00;
        tx[pos++] = 0x04;
        tx[pos++] = 192;
        tx[pos++] = 168;
        tx[pos++] = 4;
        tx[pos++] = 1;

        sendto(sock, tx, pos, 0, (struct sockaddr *)&source, socklen);
    }
}

static void start_setup_ap(void)
{
    setup_ap_running = true;

    char ap_ssid[33];
    make_ap_ssid(ap_ssid, sizeof(ap_ssid));

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, WIFI_ADMIN_AP_PASS, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = false;

    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK) {
        ESP_LOGD(TAG, "Wi-Fi stop before setup AP returned %s", esp_err_to_name(stop_err));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(start_http_server(true));
    ESP_ERROR_CHECK_WITHOUT_ABORT(start_mdns_service());
    xTaskCreate(dns_task, "setup_dns", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "setup AP started; ssid=%s password=%s url=http://192.168.4.1 or http://%s.local",
             ap_ssid, WIFI_ADMIN_AP_PASS, WIFI_ADMIN_HOSTNAME);
    persistent_log_event("info", "wifi", "setup AP started ssid=%s", ap_ssid);
}

esp_err_t wifi_admin_start(void)
{
    wifi_events = xEventGroupCreate();
    if (wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    if (sta_netif != NULL) {
        esp_netif_set_hostname(sta_netif, WIFI_ADMIN_HOSTNAME);
    }
    if (ap_netif != NULL) {
        esp_netif_set_hostname(ap_netif, WIFI_ADMIN_HOSTNAME);
    }

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "wifi event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "ip event register failed");

    wifi_credentials_t creds = {0};
    err = load_credentials(&creds);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "trying saved Wi-Fi SSID \"%s\"", creds.ssid);
        if (connect_sta(&creds)) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(start_mdns_service());
            ESP_RETURN_ON_ERROR(start_http_server(false), TAG, "log web service start failed");
            return ESP_OK;
        }
    } else {
        ESP_LOGI(TAG, "no saved Wi-Fi credentials; starting setup AP");
        persistent_log_event("info", "wifi", "no saved credentials; starting setup AP");
    }

    start_setup_ap();
    return ESP_OK;
}

bool wifi_admin_is_connected(void)
{
    return current_ssid[0] != '\0';
}
