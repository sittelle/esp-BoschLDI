#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AUTOMATION_MAX_RULES 6
#define AUTOMATION_FIELD_MAX_LEN 31
#define AUTOMATION_OP_MAX_LEN 3
#define AUTOMATION_LIGHT_ID_MAX_LEN 11
#define AUTOMATION_NAME_MAX_LEN 39
#define AUTOMATION_MIN_COOLDOWN_SEC 5
#define AUTOMATION_MAX_COOLDOWN_SEC 3600
#define AUTOMATION_MAX_CONDITIONS 5
#define AUTOMATION_MAX_GROUPS 3
#define AUTOMATION_MAX_GROUP_CONDITIONS 3
#define AUTOMATION_MAX_TRIGGERS 3

typedef struct {
    char field[AUTOMATION_FIELD_MAX_LEN + 1];
    char op[AUTOMATION_OP_MAX_LEN + 1];
    double value;
} automation_condition_t;

typedef struct {
    char light_id[AUTOMATION_LIGHT_ID_MAX_LEN + 1];
    bool action_on;
} automation_trigger_t;

typedef struct {
    char name[AUTOMATION_NAME_MAX_LEN + 1];
    bool enabled;
    automation_condition_t conditions[AUTOMATION_MAX_CONDITIONS];
    uint8_t condition_count;
    bool condition_any;
    automation_trigger_t triggers[AUTOMATION_MAX_TRIGGERS];
    uint8_t trigger_count;
    char field[AUTOMATION_FIELD_MAX_LEN + 1];
    char op[AUTOMATION_OP_MAX_LEN + 1];
    double value;
    char light_id[AUTOMATION_LIGHT_ID_MAX_LEN + 1];
    bool action_on;
    uint32_t cooldown_sec;
} automation_rule_t;

esp_err_t automation_rules_json(char *out, size_t out_len);
esp_err_t automation_add_rule(const automation_rule_t *rule, char *out, size_t out_len);
esp_err_t automation_add_rule_json(const char *rule_json, char *out, size_t out_len);
esp_err_t automation_update_rule_json(uint8_t index, const char *rule_json, char *out, size_t out_len);
esp_err_t automation_set_rule_enabled(uint8_t index, bool enabled, char *out, size_t out_len);
esp_err_t automation_delete_rule(uint8_t index, char *out, size_t out_len);
esp_err_t automation_test_rule_triggers(uint8_t index, char *out, size_t out_len);
esp_err_t automation_clear_rules(char *out, size_t out_len);
esp_err_t automation_start(void);
void automation_request_evaluate(uint32_t changed_fields);
