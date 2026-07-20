#include "automation.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hue_integration.h"
#include "live_data_decode.h"
#include "nvs.h"
#include "persistent_log.h"
#include "wifi_admin.h"

#define AUTOMATION_NAMESPACE "automation"
#define AUTOMATION_RULES_KEY "rules"
#define AUTOMATION_RULES_JSON_MAX_LEN 6144
#define AUTOMATION_WORKER_STACK_WORDS 8192

static const char *TAG = "automation";
static int64_t last_fire_ms[AUTOMATION_MAX_RULES];
static QueueHandle_t automation_queue;
static bool automation_started;
static bool automation_rules_active;

static bool parse_rule(cJSON *item, automation_rule_t *rule);

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

static bool printable_ascii_valid(const char *value, size_t max_len, bool allow_empty)
{
    if (value == NULL || strlen(value) > max_len || (!allow_empty && value[0] == '\0')) {
        return false;
    }
    for (size_t i = 0; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20 || c > 0x7e) {
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

static bool condition_valid(const automation_condition_t *condition)
{
    return condition != NULL &&
           ascii_token_valid(condition->field, AUTOMATION_FIELD_MAX_LEN) &&
           ascii_token_valid(condition->op, AUTOMATION_OP_MAX_LEN) &&
           op_valid(condition->op);
}

static bool trigger_valid(const automation_trigger_t *trigger)
{
    if (trigger == NULL || !ascii_token_valid(trigger->light_id, AUTOMATION_LIGHT_ID_MAX_LEN)) {
        return false;
    }
    for (size_t i = 0; trigger->light_id[i] != '\0'; i++) {
        if (trigger->light_id[i] < '0' || trigger->light_id[i] > '9') {
            return false;
        }
    }
    return true;
}

static void sync_primary_condition(automation_rule_t *rule)
{
    if (rule == NULL || rule->condition_count == 0) {
        return;
    }
    strlcpy(rule->field, rule->conditions[0].field, sizeof(rule->field));
    strlcpy(rule->op, rule->conditions[0].op, sizeof(rule->op));
    rule->value = rule->conditions[0].value;
}

static bool rule_valid(const automation_rule_t *rule)
{
    if (rule == NULL ||
        rule->condition_count == 0 ||
        rule->condition_count > AUTOMATION_MAX_CONDITIONS ||
        rule->trigger_count == 0 ||
        rule->trigger_count > AUTOMATION_MAX_TRIGGERS ||
        !printable_ascii_valid(rule->name, AUTOMATION_NAME_MAX_LEN, true)) {
        return false;
    }
    for (uint8_t i = 0; i < rule->condition_count; i++) {
        if (!condition_valid(&rule->conditions[i])) {
            return false;
        }
    }
    for (uint8_t i = 0; i < rule->trigger_count; i++) {
        if (!trigger_valid(&rule->triggers[i])) {
            return false;
        }
    }
    return true;
}

static bool parse_trigger(cJSON *item, automation_trigger_t *trigger)
{
    if (!cJSON_IsObject(item) || trigger == NULL) {
        return false;
    }
    memset(trigger, 0, sizeof(*trigger));
    cJSON *light_id = cJSON_GetObjectItem(item, "light_id");
    cJSON *action_on = cJSON_GetObjectItem(item, "action_on");
    if (!cJSON_IsString(light_id)) {
        return false;
    }
    strlcpy(trigger->light_id, light_id->valuestring, sizeof(trigger->light_id));
    trigger->action_on = cJSON_IsTrue(action_on);
    return trigger_valid(trigger);
}

static bool parse_condition(cJSON *item, automation_condition_t *condition)
{
    if (!cJSON_IsObject(item) || condition == NULL) {
        return false;
    }
    memset(condition, 0, sizeof(*condition));

    cJSON *field = cJSON_GetObjectItem(item, "field");
    cJSON *op = cJSON_GetObjectItem(item, "op");
    cJSON *value = cJSON_GetObjectItem(item, "value");
    if (!cJSON_IsString(field) || !cJSON_IsString(op) || !cJSON_IsNumber(value)) {
        return false;
    }
    strlcpy(condition->field, field->valuestring, sizeof(condition->field));
    strlcpy(condition->op, op->valuestring, sizeof(condition->op));
    condition->value = value->valuedouble;
    return condition_valid(condition);
}

static bool json_logic_is_or(cJSON *item, const char *key)
{
    cJSON *logic = cJSON_GetObjectItem(item, key);
    return cJSON_IsString(logic) && strcmp(logic->valuestring, "OR") == 0;
}

static bool validate_conditions_array(cJSON *conditions, int max_count)
{
    if (!cJSON_IsArray(conditions)) {
        return false;
    }
    int count = cJSON_GetArraySize(conditions);
    if (count <= 0 || count > max_count) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        automation_condition_t condition;
        if (!parse_condition(cJSON_GetArrayItem(conditions, i), &condition)) {
            return false;
        }
    }
    return true;
}

static bool validate_rule_json(cJSON *item)
{
    if (!cJSON_IsObject(item)) {
        return false;
    }

    cJSON *name = cJSON_GetObjectItem(item, "name");
    if (cJSON_IsString(name) &&
        !printable_ascii_valid(name->valuestring, AUTOMATION_NAME_MAX_LEN, true)) {
        return false;
    }

    cJSON *triggers = cJSON_GetObjectItem(item, "triggers");
    if (cJSON_IsArray(triggers)) {
        int trigger_count = cJSON_GetArraySize(triggers);
        if (trigger_count <= 0 || trigger_count > AUTOMATION_MAX_TRIGGERS) {
            return false;
        }
        for (int i = 0; i < trigger_count; i++) {
            automation_trigger_t trigger;
            if (!parse_trigger(cJSON_GetArrayItem(triggers, i), &trigger)) {
                return false;
            }
        }
    } else {
        automation_rule_t legacy;
        if (!parse_rule(item, &legacy)) {
            return false;
        }
    }

    cJSON *groups = cJSON_GetObjectItem(item, "groups");
    if (cJSON_IsArray(groups)) {
        int group_count = cJSON_GetArraySize(groups);
        if (group_count <= 0 || group_count > AUTOMATION_MAX_GROUPS) {
            return false;
        }
        for (int i = 0; i < group_count; i++) {
            cJSON *group = cJSON_GetArrayItem(groups, i);
            if (!cJSON_IsObject(group) ||
                !validate_conditions_array(cJSON_GetObjectItem(group, "conditions"),
                                           AUTOMATION_MAX_GROUP_CONDITIONS)) {
                return false;
            }
        }
        return true;
    }

    cJSON *conditions = cJSON_GetObjectItem(item, "conditions");
    if (cJSON_IsArray(conditions)) {
        return validate_conditions_array(conditions, AUTOMATION_MAX_CONDITIONS);
    }

    automation_condition_t condition;
    return parse_condition(item, &condition);
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
    cJSON *name = cJSON_GetObjectItem(item, "name");
    cJSON *condition_logic = cJSON_GetObjectItem(item, "condition_logic");
    cJSON *light_id = cJSON_GetObjectItem(item, "light_id");
    cJSON *action_on = cJSON_GetObjectItem(item, "action_on");
    cJSON *cooldown = cJSON_GetObjectItem(item, "cooldown_sec");

    rule->enabled = !cJSON_IsFalse(enabled);
    if (cJSON_IsString(name)) {
        strlcpy(rule->name, name->valuestring, sizeof(rule->name));
    }
    rule->condition_any = cJSON_IsString(condition_logic) &&
                          strcmp(condition_logic->valuestring, "OR") == 0;

    cJSON *triggers = cJSON_GetObjectItem(item, "triggers");
    if (cJSON_IsArray(triggers)) {
        int trigger_count = cJSON_GetArraySize(triggers);
        if (trigger_count <= 0 || trigger_count > AUTOMATION_MAX_TRIGGERS) {
            return false;
        }
        for (int i = 0; i < trigger_count; i++) {
            if (!parse_trigger(cJSON_GetArrayItem(triggers, i),
                               &rule->triggers[rule->trigger_count])) {
                return false;
            }
            rule->trigger_count++;
        }
    } else {
        if (!cJSON_IsString(light_id)) {
            return false;
        }
        strlcpy(rule->triggers[0].light_id, light_id->valuestring,
                sizeof(rule->triggers[0].light_id));
        rule->triggers[0].action_on = cJSON_IsTrue(action_on);
        rule->trigger_count = 1;
    }

    cJSON *conditions = cJSON_GetObjectItem(item, "conditions");
    if (cJSON_IsArray(conditions)) {
        int count = cJSON_GetArraySize(conditions);
        if (count <= 0 || count > AUTOMATION_MAX_CONDITIONS) {
            return false;
        }
        for (int i = 0; i < count; i++) {
            if (!parse_condition(cJSON_GetArrayItem(conditions, i),
                                 &rule->conditions[rule->condition_count])) {
                return false;
            }
            rule->condition_count++;
        }
    } else if (!parse_condition(item, &rule->conditions[0])) {
        return false;
    } else {
        rule->condition_count = 1;
    }
    sync_primary_condition(rule);
    strlcpy(rule->light_id, rule->triggers[0].light_id, sizeof(rule->light_id));
    rule->action_on = rule->triggers[0].action_on;
    rule->cooldown_sec = clamp_cooldown(cJSON_IsNumber(cooldown) ?
                                        (uint32_t)cooldown->valuedouble :
                                        AUTOMATION_MIN_COOLDOWN_SEC);
    return rule_valid(rule);
}

static bool rules_text_has_enabled_rule(const char *rules_text)
{
    cJSON *root = cJSON_Parse(rules_text);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(root);
    if (count > AUTOMATION_MAX_RULES) {
        count = AUTOMATION_MAX_RULES;
    }
    bool has_enabled = false;
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
        if (validate_rule_json(item) && !cJSON_IsFalse(enabled)) {
            has_enabled = true;
            break;
        }
    }
    cJSON_Delete(root);
    return has_enabled;
}

static bool rules_equal(const automation_rule_t *left, const automation_rule_t *right)
{
    if (left == NULL || right == NULL ||
        left->enabled != right->enabled ||
        left->condition_count != right->condition_count ||
        strcmp(left->name, right->name) != 0 ||
        left->condition_any != right->condition_any ||
        left->trigger_count != right->trigger_count ||
        clamp_cooldown(left->cooldown_sec) != clamp_cooldown(right->cooldown_sec)) {
        return false;
    }

    for (uint8_t i = 0; i < left->condition_count; i++) {
        if (strcmp(left->conditions[i].field, right->conditions[i].field) != 0 ||
            strcmp(left->conditions[i].op, right->conditions[i].op) != 0 ||
            fabs(left->conditions[i].value - right->conditions[i].value) >= 0.0001) {
            return false;
        }
    }
    for (uint8_t i = 0; i < left->trigger_count; i++) {
        if (strcmp(left->triggers[i].light_id, right->triggers[i].light_id) != 0 ||
            left->triggers[i].action_on != right->triggers[i].action_on) {
            return false;
        }
    }
    return true;
}

static void refresh_rules_active(void)
{
    char *rules_text = malloc(AUTOMATION_RULES_JSON_MAX_LEN);
    if (rules_text == NULL) {
        automation_rules_active = false;
        return;
    }
    if (load_rules_text(rules_text, AUTOMATION_RULES_JSON_MAX_LEN) == ESP_OK) {
        automation_rules_active = rules_text_has_enabled_rule(rules_text);
    } else {
        automation_rules_active = false;
    }
    free(rules_text);
}

static bool normalize_rule_json(cJSON *item)
{
    if (!validate_rule_json(item)) {
        return false;
    }

    cJSON *cooldown = cJSON_GetObjectItem(item, "cooldown_sec");
    if (!cJSON_IsNumber(cooldown)) {
        cJSON_AddNumberToObject(item, "cooldown_sec", AUTOMATION_MIN_COOLDOWN_SEC);
    } else {
        cJSON_SetNumberValue(cooldown, clamp_cooldown((uint32_t)cooldown->valuedouble));
    }
    cJSON *name = cJSON_GetObjectItem(item, "name");
    if (!cJSON_IsString(name)) {
        cJSON_AddStringToObject(item, "name", "Automation");
    }
    cJSON *condition_logic = cJSON_GetObjectItem(item, "condition_logic");
    if (!cJSON_IsString(condition_logic)) {
        cJSON_AddStringToObject(item, "condition_logic", "AND");
    }
    return true;
}

static cJSON *load_rules_array(char *out, size_t out_len)
{
    char *rules_text = malloc(AUTOMATION_RULES_JSON_MAX_LEN);
    if (rules_text == NULL) {
        snprintf(out, out_len, "{\"type\":\"automation_rules_write\",\"saved\":false,\"error\":\"allocation failed\"}");
        return NULL;
    }

    esp_err_t err = load_rules_text(rules_text, AUTOMATION_RULES_JSON_MAX_LEN);
    if (err != ESP_OK) {
        free(rules_text);
        snprintf(out, out_len, "{\"type\":\"automation_rules_write\",\"saved\":false,\"error\":\"load failed\"}");
        return NULL;
    }

    cJSON *rules = cJSON_Parse(rules_text);
    free(rules_text);
    if (!cJSON_IsArray(rules)) {
        cJSON_Delete(rules);
        rules = cJSON_CreateArray();
    }
    if (rules == NULL) {
        snprintf(out, out_len, "{\"type\":\"automation_rules_write\",\"saved\":false,\"error\":\"allocation failed\"}");
    }
    return rules;
}

static esp_err_t save_rules_array(cJSON *rules, char *out, size_t out_len,
                                  const char *type, const char *success_key)
{
    char *printed = cJSON_PrintUnformatted(rules);
    if (printed == NULL || strlen(printed) >= AUTOMATION_RULES_JSON_MAX_LEN) {
        cJSON_free(printed);
        snprintf(out, out_len, "{\"type\":\"%s\",\"%s\":false,\"error\":\"rules too large\"}",
                 type, success_key);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = save_rules_text(printed);
    cJSON_free(printed);
    if (err != ESP_OK) {
        snprintf(out, out_len, "{\"type\":\"%s\",\"%s\":false,\"error\":\"save failed\"}",
                 type, success_key);
        return err;
    }

    refresh_rules_active();
    snprintf(out, out_len, "{\"type\":\"%s\",\"%s\":true}", type, success_key);
    return ESP_OK;
}

static cJSON *rule_to_json(const automation_rule_t *rule)
{
    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(item, "name", rule->name[0] != '\0' ? rule->name : "Automation");
    cJSON_AddBoolToObject(item, "enabled", rule->enabled);
    cJSON_AddStringToObject(item, "condition_logic", rule->condition_any ? "OR" : "AND");
    cJSON_AddStringToObject(item, "field", rule->field);
    cJSON_AddStringToObject(item, "op", rule->op);
    cJSON_AddNumberToObject(item, "value", rule->value);
    cJSON *conditions = cJSON_AddArrayToObject(item, "conditions");
    if (conditions == NULL) {
        cJSON_Delete(item);
        return NULL;
    }
    for (uint8_t i = 0; i < rule->condition_count; i++) {
        cJSON *condition = cJSON_CreateObject();
        if (condition == NULL ||
            !cJSON_AddItemToArray(conditions, condition)) {
            cJSON_Delete(condition);
            cJSON_Delete(item);
            return NULL;
        }
        cJSON_AddStringToObject(condition, "field", rule->conditions[i].field);
        cJSON_AddStringToObject(condition, "op", rule->conditions[i].op);
        cJSON_AddNumberToObject(condition, "value", rule->conditions[i].value);
    }
    cJSON *triggers = cJSON_AddArrayToObject(item, "triggers");
    if (triggers == NULL) {
        cJSON_Delete(item);
        return NULL;
    }
    for (uint8_t i = 0; i < rule->trigger_count; i++) {
        cJSON *trigger = cJSON_CreateObject();
        if (trigger == NULL || !cJSON_AddItemToArray(triggers, trigger)) {
            cJSON_Delete(trigger);
            cJSON_Delete(item);
            return NULL;
        }
        cJSON_AddStringToObject(trigger, "light_id", rule->triggers[i].light_id);
        cJSON_AddBoolToObject(trigger, "action_on", rule->triggers[i].action_on);
    }
    cJSON_AddStringToObject(item, "light_id", rule->triggers[0].light_id);
    cJSON_AddBoolToObject(item, "action_on", rule->triggers[0].action_on);
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

static bool evaluate_condition_json(cJSON *item)
{
    automation_condition_t condition;
    if (!parse_condition(item, &condition)) {
        return false;
    }
    double actual = 0.0;
    return live_data_latest_field_value(condition.field, &actual) &&
           compare_value(actual, condition.op, condition.value);
}

static bool evaluate_conditions_array_json(cJSON *conditions, bool any)
{
    if (!cJSON_IsArray(conditions)) {
        return false;
    }
    int count = cJSON_GetArraySize(conditions);
    if (count <= 0) {
        return false;
    }
    bool matches = any ? false : true;
    for (int i = 0; i < count; i++) {
        bool condition_matches = evaluate_condition_json(cJSON_GetArrayItem(conditions, i));
        if (any) {
            if (condition_matches) {
                return true;
            }
        } else if (!condition_matches) {
            return false;
        }
    }
    return matches;
}

static uint32_t condition_fields_mask_json(cJSON *conditions)
{
    if (!cJSON_IsArray(conditions)) {
        return 0;
    }
    uint32_t mask = 0;
    int count = cJSON_GetArraySize(conditions);
    for (int i = 0; i < count; i++) {
        cJSON *condition = cJSON_GetArrayItem(conditions, i);
        cJSON *field = cJSON_GetObjectItem(condition, "field");
        if (cJSON_IsString(field)) {
            mask |= live_data_field_mask(field->valuestring);
        }
    }
    return mask;
}

static bool rule_condition_fields_changed(cJSON *item, uint32_t changed_fields)
{
    if (changed_fields == 0) {
        return false;
    }

    cJSON *groups = cJSON_GetObjectItem(item, "groups");
    if (cJSON_IsArray(groups)) {
        uint32_t mask = 0;
        int count = cJSON_GetArraySize(groups);
        for (int i = 0; i < count; i++) {
            mask |= condition_fields_mask_json(
                cJSON_GetObjectItem(cJSON_GetArrayItem(groups, i), "conditions"));
        }
        return (mask & changed_fields) != 0;
    }

    cJSON *conditions = cJSON_GetObjectItem(item, "conditions");
    if (cJSON_IsArray(conditions)) {
        return (condition_fields_mask_json(conditions) & changed_fields) != 0;
    }

    cJSON *field = cJSON_GetObjectItem(item, "field");
    return cJSON_IsString(field) &&
           (live_data_field_mask(field->valuestring) & changed_fields) != 0;
}

static bool evaluate_rule_json(cJSON *item)
{
    cJSON *groups = cJSON_GetObjectItem(item, "groups");
    if (cJSON_IsArray(groups)) {
        bool any_group = json_logic_is_or(item, "group_logic");
        int count = cJSON_GetArraySize(groups);
        if (count <= 0) {
            return false;
        }
        bool matches = any_group ? false : true;
        for (int i = 0; i < count; i++) {
            cJSON *group = cJSON_GetArrayItem(groups, i);
            bool group_matches = evaluate_conditions_array_json(
                cJSON_GetObjectItem(group, "conditions"),
                json_logic_is_or(group, "condition_logic"));
            if (any_group) {
                if (group_matches) {
                    return true;
                }
            } else if (!group_matches) {
                return false;
            }
        }
        return matches;
    }

    cJSON *conditions = cJSON_GetObjectItem(item, "conditions");
    if (cJSON_IsArray(conditions)) {
        return evaluate_conditions_array_json(conditions, json_logic_is_or(item, "condition_logic"));
    }
    return evaluate_condition_json(item);
}

static bool fire_rule_triggers_json(cJSON *item, int rule_index)
{
    cJSON *triggers = cJSON_GetObjectItem(item, "triggers");
    if (!cJSON_IsArray(triggers)) {
        automation_rule_t legacy;
        if (!parse_rule(item, &legacy)) {
            return false;
        }
        triggers = cJSON_CreateArray();
        if (triggers == NULL) {
            return false;
        }
        bool ok = true;
        for (uint8_t i = 0; i < legacy.trigger_count; i++) {
            char response[512];
            esp_err_t err = hue_integration_set_light_state_json(legacy.triggers[i].light_id,
                                                                 legacy.triggers[i].action_on,
                                                                 response, sizeof(response));
            if (err != ESP_OK) {
                ok = false;
                ESP_LOGW(TAG, "rule action failed index=%d trigger=%u err=%s",
                         rule_index, (unsigned)i, esp_err_to_name(err));
            }
        }
        cJSON_Delete(triggers);
        return ok;
    }

    bool ok = true;
    int trigger_count = cJSON_GetArraySize(triggers);
    if (trigger_count > AUTOMATION_MAX_TRIGGERS) {
        trigger_count = AUTOMATION_MAX_TRIGGERS;
    }
    for (int i = 0; i < trigger_count; i++) {
        automation_trigger_t trigger;
        if (!parse_trigger(cJSON_GetArrayItem(triggers, i), &trigger)) {
            ok = false;
            continue;
        }
        char response[512];
        esp_err_t err = hue_integration_set_light_state_json(trigger.light_id,
                                                             trigger.action_on,
                                                             response, sizeof(response));
        if (err != ESP_OK) {
            ok = false;
            ESP_LOGW(TAG, "rule action failed index=%d trigger=%d err=%s",
                     rule_index, i, esp_err_to_name(err));
        }
    }
    return ok;
}

esp_err_t automation_rules_json(char *out, size_t out_len)
{
    char *rules = malloc(AUTOMATION_RULES_JSON_MAX_LEN);
    if (rules == NULL) {
        snprintf(out, out_len, "{\"type\":\"automation_rules\",\"rules\":[],\"error\":\"allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = load_rules_text(rules, AUTOMATION_RULES_JSON_MAX_LEN);
    if (err != ESP_OK) {
        free(rules);
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
    free(rules);
    return ESP_OK;
}

esp_err_t automation_add_rule(const automation_rule_t *rule, char *out, size_t out_len)
{
    if (rule == NULL) {
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"invalid rule\"}");
        return ESP_ERR_INVALID_ARG;
    }

    automation_rule_t normalized = *rule;
    if (normalized.condition_count == 0) {
        strlcpy(normalized.conditions[0].field, normalized.field,
                sizeof(normalized.conditions[0].field));
        strlcpy(normalized.conditions[0].op, normalized.op,
                sizeof(normalized.conditions[0].op));
        normalized.conditions[0].value = normalized.value;
        normalized.condition_count = 1;
    }
    if (normalized.trigger_count == 0) {
        strlcpy(normalized.triggers[0].light_id, normalized.light_id,
                sizeof(normalized.triggers[0].light_id));
        normalized.triggers[0].action_on = normalized.action_on;
        normalized.trigger_count = 1;
    }
    sync_primary_condition(&normalized);
    strlcpy(normalized.light_id, normalized.triggers[0].light_id, sizeof(normalized.light_id));
    normalized.action_on = normalized.triggers[0].action_on;
    normalized.cooldown_sec = clamp_cooldown(normalized.cooldown_sec);
    if (!rule_valid(&normalized)) {
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"invalid rule\"}");
        return ESP_ERR_INVALID_ARG;
    }

    char *rules_text = malloc(AUTOMATION_RULES_JSON_MAX_LEN);
    if (rules_text == NULL) {
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = load_rules_text(rules_text, AUTOMATION_RULES_JSON_MAX_LEN);
    if (err != ESP_OK) {
        free(rules_text);
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"load failed\"}");
        return err;
    }

    cJSON *root = cJSON_Parse(rules_text);
    free(rules_text);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateArray();
    }
    if (root == NULL) {
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }
    bool has_enabled_rule = normalized.enabled;
    int existing_count = cJSON_GetArraySize(root);
    if (existing_count > AUTOMATION_MAX_RULES) {
        existing_count = AUTOMATION_MAX_RULES;
    }
    for (int i = 0; i < existing_count; i++) {
        automation_rule_t existing;
        if (parse_rule(cJSON_GetArrayItem(root, i), &existing)) {
            has_enabled_rule = has_enabled_rule || existing.enabled;
            if (rules_equal(&existing, &normalized)) {
                cJSON_Delete(root);
                automation_rules_active = has_enabled_rule;
                snprintf(out, out_len,
                         "{\"type\":\"automation_add\",\"saved\":true,\"duplicate\":true}");
                return ESP_OK;
            }
        }
    }

    if (cJSON_GetArraySize(root) >= AUTOMATION_MAX_RULES) {
        cJSON_Delete(root);
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"rule limit reached\"}");
        return ESP_ERR_NO_MEM;
    }

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

    automation_rules_active = has_enabled_rule;
    snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":true}");
    persistent_log_event("info", "automation", "rule added field=%s light_id=%s action_on=%u",
                         normalized.field, normalized.light_id, normalized.action_on);
    return ESP_OK;
}

esp_err_t automation_add_rule_json(const char *rule_json, char *out, size_t out_len)
{
    if (rule_json == NULL) {
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"invalid JSON\"}");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *item = cJSON_Parse(rule_json);
    if (!normalize_rule_json(item)) {
        cJSON_Delete(item);
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"invalid rule\"}");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *rules = load_rules_array(out, out_len);
    if (rules == NULL) {
        cJSON_Delete(item);
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }
    if (cJSON_GetArraySize(rules) >= AUTOMATION_MAX_RULES) {
        cJSON_Delete(item);
        cJSON_Delete(rules);
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"rule limit reached\"}");
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddItemToArray(rules, item)) {
        cJSON_Delete(item);
        cJSON_Delete(rules);
        snprintf(out, out_len, "{\"type\":\"automation_add\",\"saved\":false,\"error\":\"allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = save_rules_array(rules, out, out_len, "automation_add", "saved");
    cJSON_Delete(rules);
    if (err != ESP_OK) {
        return err;
    }
    persistent_log_event("info", "automation", "grouped rule added");
    return ESP_OK;
}

esp_err_t automation_update_rule_json(uint8_t index, const char *rule_json, char *out, size_t out_len)
{
    if (rule_json == NULL) {
        snprintf(out, out_len, "{\"type\":\"automation_update\",\"updated\":false,\"error\":\"invalid JSON\"}");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *item = cJSON_Parse(rule_json);
    if (!normalize_rule_json(item)) {
        cJSON_Delete(item);
        snprintf(out, out_len, "{\"type\":\"automation_update\",\"updated\":false,\"error\":\"invalid rule\"}");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *rules = load_rules_array(out, out_len);
    if (rules == NULL) {
        cJSON_Delete(item);
        snprintf(out, out_len, "{\"type\":\"automation_update\",\"updated\":false,\"error\":\"allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }

    if (index >= cJSON_GetArraySize(rules) || index >= AUTOMATION_MAX_RULES) {
        cJSON_Delete(item);
        cJSON_Delete(rules);
        snprintf(out, out_len, "{\"type\":\"automation_update\",\"updated\":false,\"error\":\"rule not found\"}");
        return ESP_ERR_NOT_FOUND;
    }
    if (!cJSON_ReplaceItemInArray(rules, index, item)) {
        cJSON_Delete(item);
        cJSON_Delete(rules);
        snprintf(out, out_len, "{\"type\":\"automation_update\",\"updated\":false,\"error\":\"allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = save_rules_array(rules, out, out_len, "automation_update", "updated");
    cJSON_Delete(rules);
    if (err == ESP_OK) {
        memset(last_fire_ms, 0, sizeof(last_fire_ms));
        persistent_log_event("info", "automation", "rule updated index=%u", (unsigned)index);
    }
    return err;
}

esp_err_t automation_set_rule_enabled(uint8_t index, bool enabled, char *out, size_t out_len)
{
    cJSON *rules = load_rules_array(out, out_len);
    if (rules == NULL) {
        snprintf(out, out_len, "{\"type\":\"automation_enable\",\"updated\":false,\"error\":\"allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }

    cJSON *item = cJSON_GetArrayItem(rules, index);
    if (item == NULL || index >= AUTOMATION_MAX_RULES) {
        cJSON_Delete(rules);
        snprintf(out, out_len, "{\"type\":\"automation_enable\",\"updated\":false,\"error\":\"rule not found\"}");
        return ESP_ERR_NOT_FOUND;
    }
    cJSON_DeleteItemFromObject(item, "enabled");
    cJSON_AddBoolToObject(item, "enabled", enabled);
    if (!validate_rule_json(item)) {
        cJSON_Delete(rules);
        snprintf(out, out_len, "{\"type\":\"automation_enable\",\"updated\":false,\"error\":\"invalid stored rule\"}");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = save_rules_array(rules, out, out_len, "automation_enable", "updated");
    cJSON_Delete(rules);
    if (err == ESP_OK) {
        memset(last_fire_ms, 0, sizeof(last_fire_ms));
        persistent_log_event("info", "automation", "rule enabled index=%u enabled=%u",
                             (unsigned)index, enabled);
    }
    return err;
}

esp_err_t automation_delete_rule(uint8_t index, char *out, size_t out_len)
{
    cJSON *rules = load_rules_array(out, out_len);
    if (rules == NULL) {
        snprintf(out, out_len, "{\"type\":\"automation_delete\",\"deleted\":false,\"error\":\"allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }

    if (index >= cJSON_GetArraySize(rules) || index >= AUTOMATION_MAX_RULES) {
        cJSON_Delete(rules);
        snprintf(out, out_len, "{\"type\":\"automation_delete\",\"deleted\":false,\"error\":\"rule not found\"}");
        return ESP_ERR_NOT_FOUND;
    }
    cJSON_DeleteItemFromArray(rules, index);
    esp_err_t err = save_rules_array(rules, out, out_len, "automation_delete", "deleted");
    cJSON_Delete(rules);
    if (err == ESP_OK) {
        memset(last_fire_ms, 0, sizeof(last_fire_ms));
        persistent_log_event("info", "automation", "rule deleted index=%u", (unsigned)index);
    }
    return err;
}

esp_err_t automation_test_rule_triggers(uint8_t index, char *out, size_t out_len)
{
    if (!wifi_admin_is_connected()) {
        snprintf(out, out_len,
                 "{\"type\":\"automation_test\",\"tested\":false,\"error\":\"Wi-Fi not connected\"}");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *rules = load_rules_array(out, out_len);
    if (rules == NULL) {
        snprintf(out, out_len,
                 "{\"type\":\"automation_test\",\"tested\":false,\"error\":\"allocation failed\"}");
        return ESP_ERR_NO_MEM;
    }

    cJSON *item = cJSON_GetArrayItem(rules, index);
    if (item == NULL || index >= AUTOMATION_MAX_RULES) {
        cJSON_Delete(rules);
        snprintf(out, out_len,
                 "{\"type\":\"automation_test\",\"tested\":false,\"error\":\"rule not found\"}");
        return ESP_ERR_NOT_FOUND;
    }
    if (!validate_rule_json(item)) {
        cJSON_Delete(rules);
        snprintf(out, out_len,
                 "{\"type\":\"automation_test\",\"tested\":false,\"error\":\"invalid stored rule\"}");
        return ESP_ERR_INVALID_STATE;
    }

    bool ok = fire_rule_triggers_json(item, index);
    cJSON_Delete(rules);
    snprintf(out, out_len, "{\"type\":\"automation_test\",\"tested\":%s}",
             ok ? "true" : "false");
    persistent_log_event(ok ? "info" : "warn", "automation",
                         "rule trigger test index=%u ok=%u", (unsigned)index, ok);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t automation_clear_rules(char *out, size_t out_len)
{
    esp_err_t err = save_rules_text("[]");
    if (err == ESP_OK) {
        memset(last_fire_ms, 0, sizeof(last_fire_ms));
        automation_rules_active = false;
    }
    snprintf(out, out_len, "{\"type\":\"automation_clear\",\"cleared\":%s}",
             err == ESP_OK ? "true" : "false");
    return err;
}

static void automation_evaluate_latest(uint32_t changed_fields)
{
    if (!wifi_admin_is_connected()) {
        ESP_LOGD(TAG, "Wi-Fi not connected; skipping automation evaluation");
        return;
    }

    char *rules_text = malloc(AUTOMATION_RULES_JSON_MAX_LEN);
    if (rules_text == NULL) {
        return;
    }
    if (load_rules_text(rules_text, AUTOMATION_RULES_JSON_MAX_LEN) != ESP_OK) {
        free(rules_text);
        return;
    }
    cJSON *root = cJSON_Parse(rules_text);
    free(rules_text);
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
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (!validate_rule_json(item)) {
            continue;
        }
        cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
        if (cJSON_IsFalse(enabled)) {
            continue;
        }
        if (!rule_condition_fields_changed(item, changed_fields)) {
            continue;
        }
        if (!evaluate_rule_json(item)) {
            continue;
        }

        cJSON *cooldown = cJSON_GetObjectItem(item, "cooldown_sec");
        uint32_t cooldown_sec = cJSON_IsNumber(cooldown) ?
                                (uint32_t)cooldown->valuedouble :
                                AUTOMATION_MIN_COOLDOWN_SEC;
        int64_t cooldown_ms = (int64_t)clamp_cooldown(cooldown_sec) * 1000;
        if (last_fire_ms[i] != 0 && now_ms - last_fire_ms[i] < cooldown_ms) {
            continue;
        }

        if (fire_rule_triggers_json(item, i)) {
            last_fire_ms[i] = now_ms;
            cJSON *name = cJSON_GetObjectItem(item, "name");
            cJSON *triggers = cJSON_GetObjectItem(item, "triggers");
            persistent_log_event("info", "automation",
                                 "rule fired index=%d name=%s triggers=%d",
                                 i, cJSON_IsString(name) ? name->valuestring : "Automation",
                                 cJSON_IsArray(triggers) ? cJSON_GetArraySize(triggers) : 1);
        }
    }
    cJSON_Delete(root);
}

static void automation_worker_task(void *arg)
{
    (void)arg;
    uint32_t changed_fields;

    while (true) {
        if (xQueueReceive(automation_queue, &changed_fields, portMAX_DELAY) == pdTRUE) {
            automation_evaluate_latest(changed_fields);
        }
    }
}

esp_err_t automation_start(void)
{
    if (automation_started) {
        return ESP_OK;
    }

    refresh_rules_active();
    automation_queue = xQueueCreate(4, sizeof(uint32_t));
    if (automation_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(automation_worker_task, "automation",
                                AUTOMATION_WORKER_STACK_WORDS, NULL, 3, NULL);
    if (ok != pdPASS) {
        vQueueDelete(automation_queue);
        automation_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    automation_started = true;
    persistent_log_event("info", "automation", "automation worker started");
    return ESP_OK;
}

void automation_request_evaluate(uint32_t changed_fields)
{
    if (automation_queue == NULL || !automation_rules_active || changed_fields == 0) {
        return;
    }

    if (xQueueSend(automation_queue, &changed_fields, 0) != pdTRUE) {
        uint32_t pending = 0;
        if (xQueueReceive(automation_queue, &pending, 0) == pdTRUE) {
            changed_fields |= pending;
        }
        (void)xQueueSend(automation_queue, &changed_fields, 0);
    }
}
