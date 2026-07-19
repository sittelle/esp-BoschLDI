#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t wifi_admin_start(void);
bool wifi_admin_is_connected(void);
