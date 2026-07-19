#pragma once

#include "esp_err.h"

typedef enum {
    STATUS_LED_BOOT = 0,
    STATUS_LED_ADVERTISING,
    STATUS_LED_CONNECTED,
    STATUS_LED_SECURED,
    STATUS_LED_READY,
    STATUS_LED_ACTIVITY,
    STATUS_LED_ERROR,
} status_led_state_t;

esp_err_t status_led_init(void);
void status_led_set(status_led_state_t state);
void status_led_reload_config(void);
