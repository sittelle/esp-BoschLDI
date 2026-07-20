#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t hue_integration_bridge_discovery_json(char *out, size_t out_len);
esp_err_t hue_integration_status_json(char *out, size_t out_len);
esp_err_t hue_integration_pair_json(const char *bridge_host, char *out, size_t out_len);
esp_err_t hue_integration_pair_start_json(const char *bridge_host, char *out, size_t out_len);
esp_err_t hue_integration_pair_progress_json(char *out, size_t out_len);
esp_err_t hue_integration_pair_cancel_json(char *out, size_t out_len);
esp_err_t hue_integration_devices_json(char *out, size_t out_len);
esp_err_t hue_integration_resources_json(char *out, size_t out_len);
esp_err_t hue_integration_clear_pairing_json(char *out, size_t out_len);
esp_err_t hue_integration_set_light_state_json(const char *light_id, bool on,
                                               char *out, size_t out_len);
