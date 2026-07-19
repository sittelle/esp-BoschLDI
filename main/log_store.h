#pragma once

#include <stddef.h>

void log_store_init(void);
size_t log_store_copy(char *out, size_t out_len);

