#pragma once

#include "esp_err.h"

esp_err_t persistent_log_init(void);
void persistent_log_event(const char *level, const char *area, const char *fmt, ...);
void persistent_log_bike_data(const char *summary);
const char *persistent_log_path_current(void);
const char *persistent_log_path_previous(void);

