#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BIKE_HISTORY_FIELD_TIME 1
#define BIKE_HISTORY_FIELD_SPEED_KMH_CENTI 2
#define BIKE_HISTORY_FIELD_CADENCE_RPM 3
#define BIKE_HISTORY_FIELD_RIDER_POWER_W 4
#define BIKE_HISTORY_FIELD_AMBIENT_BRIGHTNESS_MILLILUX 5
#define BIKE_HISTORY_FIELD_BATTERY_SOC 6
#define BIKE_HISTORY_FIELD_ODOMETER_M 7
#define BIKE_HISTORY_FIELD_BIKE_LIGHT 8
#define BIKE_HISTORY_FIELD_SYSTEM_LOCKED 9
#define BIKE_HISTORY_FIELD_CHARGER_CONNECTED 10
#define BIKE_HISTORY_FIELD_LIGHT_RESERVE_STATE 11
#define BIKE_HISTORY_FIELD_DIAGNOSIS_PROGRAM_ACTIVE 12
#define BIKE_HISTORY_FIELD_BIKE_NOT_DRIVING 13

#define BIKE_HISTORY_TYPE_U32 1
#define BIKE_HISTORY_TYPE_I32 2
#define BIKE_HISTORY_TYPE_BOOL 3

esp_err_t bike_history_init(void);
esp_err_t bike_history_append(uint8_t field_id, uint8_t value_type,
                              int32_t value, uint32_t unix_time);
bool bike_history_has_records(void);
esp_err_t bike_history_export_json(char *out, size_t out_len);
esp_err_t bike_history_field_json(uint8_t field_id, char *out, size_t out_len);
esp_err_t bike_history_clear(void);
