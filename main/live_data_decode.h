#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define LIVE_DATA_FIELD_SPEED              (1u << 0)
#define LIVE_DATA_FIELD_CADENCE            (1u << 1)
#define LIVE_DATA_FIELD_RIDER_POWER        (1u << 2)
#define LIVE_DATA_FIELD_AMBIENT_BRIGHTNESS (1u << 3)
#define LIVE_DATA_FIELD_BATTERY_SOC        (1u << 4)
#define LIVE_DATA_FIELD_ODOMETER           (1u << 5)
#define LIVE_DATA_FIELD_BIKE_LIGHT         (1u << 6)
#define LIVE_DATA_FIELD_SYSTEM_LOCKED      (1u << 7)
#define LIVE_DATA_FIELD_CHARGER_CONNECTED  (1u << 8)
#define LIVE_DATA_FIELD_LIGHT_RESERVE      (1u << 9)
#define LIVE_DATA_FIELD_DIAGNOSIS_ACTIVE   (1u << 10)
#define LIVE_DATA_FIELD_BIKE_NOT_DRIVING   (1u << 11)
#define LIVE_DATA_FIELD_UNKNOWN            (1u << 31)

bool live_data_decode_and_log(const uint8_t *buf, size_t len);
bool live_data_latest_summary(char *out, size_t out_len);
bool live_data_latest_json(char *out, size_t out_len);
bool live_data_latest_field_value(const char *field, double *out);
uint32_t live_data_field_mask(const char *field);
void live_data_init(void);
