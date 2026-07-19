#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool live_data_decode_and_log(const uint8_t *buf, size_t len);
bool live_data_latest_summary(char *out, size_t out_len);
bool live_data_latest_json(char *out, size_t out_len);
bool live_data_latest_field_value(const char *field, double *out);
