#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AUTOMATION_MAX_RULES 6
#define AUTOMATION_FIELD_MAX_LEN 31
#define AUTOMATION_OP_MAX_LEN 3
#define AUTOMATION_LIGHT_ID_MAX_LEN 11
#define AUTOMATION_MIN_COOLDOWN_SEC 5
#define AUTOMATION_MAX_COOLDOWN_SEC 3600

typedef struct {
    bool enabled;
    char field[AUTOMATION_FIELD_MAX_LEN + 1];
    char op[AUTOMATION_OP_MAX_LEN + 1];
    double value;
    char light_id[AUTOMATION_LIGHT_ID_MAX_LEN + 1];
    bool action_on;
    uint32_t cooldown_sec;
} automation_rule_t;

esp_err_t automation_rules_json(char *out, size_t out_len);
esp_err_t automation_add_rule(const automation_rule_t *rule, char *out, size_t out_len);
esp_err_t automation_clear_rules(char *out, size_t out_len);
esp_err_t automation_start(void);
void automation_request_evaluate(void);
