#include "automation.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hue_integration.h"
#include "live_data_decode.h"
#include "nvs.h"
#include "persistent_log.h"

#define AUTOMATION_NAMESPACE "automation"
#define AUTOMATION_RULES_KEY "rules"
#define AUTOMATION_RULES_JSON_MAX_LEN 2048

static const char *TAG = "automation";
static int64_t last_fire_ms[AUTOMATION_MAX_RULES];

static bool ascii_token_valid(const char *value, size_t max_len)
{
    if (value == NULL || value[0] == '\0' || strlen(value) > max_len) {
        return false;
    }
    for (size_t i = 0; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

static bool op_valid(const char *op)
{
    return strcmp(op, "<") == 0 ||
           strcmp(op, "<=") == 0 ||
           strcmp(op, "==") == 0 ||
           strcmp(op, ">=") == 0 ||
           strcmp(op, ">") == 0;
}

static uint32_t clamp_cooldown(uint32_t seconds)
{
    if (seconds < AUTOMATION_MIN_COOLDOWN_SEC) {
        return AUTOMATION_MIN_COOLDOWN_SEC;
    }
    if (seconds > AUTOMATION_MAX_COOLDOWN_SEC) {
        return AUTOMATION_MAX_COOLDOWN_SEC;
    }
    return seconds;
}

static bool rule_valid(const automation_rule_t *rule)
{
    if (rule == NULL ||
        !ascii_token_valid(rule->field, AUTOMATION_FIELD_MAX_LEN) ||
        !ascii_token_valid(rule->op, AUTOMATION_OP_MAX_LEN) ||
        !ascii_token_valid(rule->light_id, AUTOMATION_LIGHT_ID_MAX_LEN) ||
        !op_valid(rule->op)) {
        return false;
    }
    for (size_t i = 0; rule->light_id[i] != '\0'; i++) {
        if (rule->light_id[i] < '0' || rule->light_id[i] > '9') {
            return false;
        }
    }
    return true;
}

static esp_err_t load_rules_text(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(out, "[]", out_len);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AUTOMATION_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    size_t len = out_len;
    err = nvs_get_str(nvs, AUTOMATION_RULES_KEY, out, &len);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strlcpy(out, "[]", out_len);
        return ESP_OK;
    }
    return err;
}

static esp_err_t save_rules_text(const char *rules_json)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(AUTOMATION_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, AUTOMATION_RULES_KEY, rules_json);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static bool parse_rule(cJSON *item, automation_rule_t *rule)
{
    if (!cJSON_IsObject(item) || rule == NULL) {
        return false;
    }
    memset(rule, 0, sizeof(*rule));

    cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
    cJSON *field = cJSON_GetObjectItem(item, "field");
    cJSON *op = cJSON_GetObjectItem(item, "op");
    cJSON *value = cJSON_GetObjectItem(item, "value");
    cJSON *light_id = cJSON_GetObjectItem(item, "light_id");
    cJSON *action_on = cJSON_GetObjectItem(item, "action_on");
    cJSON *cooldown = cJSON_GetObjectItem(item, "cooldown_sec");

    rule->enabled = !cJSON_IsFalse(enabled);
    if (!cJSON_IsString(field) || !cJSON_IsString(op) ||
        !cJSON_IsNumber(value) || !cJSON_IsString(light_id)) {
        return false;
    }
    strlcpy(rule->field, field->valuestring, sizeof(rule->field));
    strlcpy(rule->op, op->valuestring, sizeof(rule->op));
    rule->value = value->valuedouble;
    strlcpy(rule->light_id, light_id->valuestring, sizeof(rule->light_id));
    rule->action_on = cJSON_IsTrue(action_on);
    rule->cooldown_sec = clamp_cooldown(cJSON_IsNumber(cooldown) ?
                                        (uint32_t)cooldown->valuedouble :
                                        AUTOMATION_MIN_COOLDOWN_SEC);
    return rule_valid(rule);
}

static cJSON *rule_to_json(const automation_rule_t *rule)
{
    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(item, "enabled", rule->enabled);
    cJSON_AddStringToObject(item, "field", rule->field);
    cJSON_AddStringToObject(item, "op", rule->op);
    cJSON_AddNumberToObject(item, "value", rule->value);
    cJSON_AddStringToObject(item, "light_id", rule->light_id);
    cJSON_AddBoolToObject(item, "action_on", rule->action_on);
    cJSON_AddNumberToObject(item, "cooldown_sec", clamp_cooldown(rule->cooldown_sec));
    return item;
}

static bool compare_value(double actual, const char *op, double expected)
{
    if (strcmp(op, "<") == 0) {
        return actual < expected;
    }
    if (strcmp(op, "<=") == 0) {
        return actual <= expected;
    }
    if (strcmp(op, "==") == 0) {
        return fabs(actual - expected) < 0.0001;
    }
    if (strcmp(op, ">=") == 0) {
        return actual >= expected;
    }
    if (strcmp(op, ">") == 0) {
        return actual > expected;
    }
    return false;
}

esp_err_t automation_rules_json(char *out, size_t out_len)
{
    char rules[AUTOMATION_RULES_JSON_MAX_LEN];
    esp_err_t err = load_rules_text(rules, sizeof(rules));
    if (err != ESP_OK) {
        snprintf(out, out_len, "{\"type\":\"automation_rules\",\"rules\":[],\"error\":\"load failed\"}");
        return err;
    }

    snprintf(out, out_len,
             "{\"type\":\"automation_rules\",\"max_rules\":%u,"
             "\"min_cooldown_sec\":%u,\"max_cooldown_sec\":%u,\"rules\":",
             (unsigned)AUTOMATION_MAX_RULES,
             (unsigned)AUTOMATION_MIN_COOLDOWN_SEC,
             (unsigned)AUTOMATION_MAX_COOLDOWN_SEC);
    strlcat(out, rules, out_len);
    strlcat(out, "}", out_len);
    return ESP_OK;
}

esp_err_t automation_add_rule(const automation_rule_t *rule, char *out, size_t out_len)
{
    if (!rule_valid(rule)) {
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"invalid rule\"}");
        return ESP_ERR_INVALID_ARG;
    }

    char rules_text[AUTOMATION_RULES_JSON_MAX_LEN];
    esp_err_t err = load_rules_text(rules_text, sizeof(rules_text));
    if (err != ESP_OK) {
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"load failed\"}");
        return err;
    }

    cJSON *root = cJSON_Parse(rules_text);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateArray();
    }
    if (root == NULL) {
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_GetArraySize(root) >= AUTOMATION_MAX_RULES) {
        cJSON_Delete(root);
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"rule limit reached\"}");
        return ESP_ERR_NO_MEM;
    }

    automation_rule_t normalized = *rule;
    normalized.cooldown_sec = clamp_cooldown(normalized.cooldown_sec);
    cJSON *item = rule_to_json(&normalized);
    if (item == NULL || !cJSON_AddItemToArray(root, item)) {
        cJSON_Delete(item);
        cJSON_Delete(root);
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == NULL || strlen(printed) >= AUTOMATION_RULES_JSON_MAX_LEN) {
        cJSON_free(printed);
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"rules too large\"}");
        return ESP_ERR_NO_MEM;
    }

    err = save_rules_text(printed);
    cJSON_free(printed);
    if (err != ESP_OK) {
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"save failed\"}");
        return err;
    }

    snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":true}");
    persistent_log_event("info", "automation", "rule added field=%s light_id=%s action_on=%u",
                         normalized.field, normalized.light_id, normalized.action_on);
    return ESP_OK;
}

esp_err_t automation_clear_rules(char *out, size_t out_len)
{
    esp_err_t err = save_rules_text("[]");
    if (err == ESP_OK) {
        memset(last_fire_ms, 0, sizeof(last_fire_ms));
    }
    snprintf(out, out_len, "{\"type\":\"automation_clear\",\"cleared\":%s}",
             err == ESP_OK ? "true" : "false");
    return err;
}

void automation_evaluate_latest(void)
{
    char rules_text[AUTOMATION_RULES_JSON_MAX_LEN];
    if (load_rules_text(rules_text, sizeof(rules_text)) != ESP_OK) {
        return;
    }
    cJSON *root = cJSON_Parse(rules_text);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    int count = cJSON_GetArraySize(root);
    if (count > AUTOMATION_MAX_RULES) {
        count = AUTOMATION_MAX_RULES;
    }
    for (int i = 0; i < count; i++) {
        automation_rule_t rule;
        if (!parse_rule(cJSON_GetArrayItem(root, i), &rule) || !rule.enabled) {
            continue;
        }

        double actual = 0.0;
        if (!live_data_latest_field_value(rule.field, &actual) ||
            !compare_value(actual, rule.op, rule.value)) {
            continue;
        }

        int64_t cooldown_ms = (int64_t)clamp_cooldown(rule.cooldown_sec) * 1000;
        if (last_fire_ms[i] != 0 && now_ms - last_fire_ms[i] < cooldown_ms) {
            continue;
        }

        char response[512];
        esp_err_t err = hue_integration_set_light_state_json(rule.light_id, rule.action_on,
                                                             response, sizeof(response));
        if (err == ESP_OK) {
            last_fire_ms[i] = now_ms;
            persistent_log_event("info", "automation",
                                 "rule fired index=%d field=%s actual=%.3f op=%s value=%.3f light_id=%s on=%u",
                                 i, rule.field, actual, rule.op, rule.value,
                                 rule.light_id, rule.action_on);
        } else {
            ESP_LOGW(TAG, "rule action failed index=%d err=%s", i, esp_err_to_name(err));
        }
    }
    cJSON_Delete(root);
}
