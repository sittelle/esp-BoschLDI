#include "log_store.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define LOG_STORE_SIZE 8192
#define LOG_LINE_SIZE 256

static char log_buffer[LOG_STORE_SIZE];
static size_t log_head;
static bool log_wrapped;
static portMUX_TYPE log_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t previous_vprintf;

static void store_bytes(const char *data, size_t len)
{
    portENTER_CRITICAL(&log_lock);
    for (size_t i = 0; i < len; i++) {
        log_buffer[log_head] = data[i];
        log_head = (log_head + 1) % sizeof(log_buffer);
        if (log_head == 0) {
            log_wrapped = true;
        }
    }
    portEXIT_CRITICAL(&log_lock);
}

static int log_store_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    char line[LOG_LINE_SIZE];
    int len = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (len > 0) {
        size_t stored_len = (size_t)len;
        if (stored_len >= sizeof(line)) {
            stored_len = sizeof(line) - 1;
            line[stored_len - 1] = '\n';
        }
        store_bytes(line, stored_len);
    }

    if (previous_vprintf != NULL) {
        return previous_vprintf(fmt, args);
    }
    return vprintf(fmt, args);
}

void log_store_init(void)
{
    previous_vprintf = esp_log_set_vprintf(log_store_vprintf);
}

size_t log_store_copy(char *out, size_t out_len)
{
    if (out_len == 0) {
        return 0;
    }

    portENTER_CRITICAL(&log_lock);
    size_t len = log_wrapped ? sizeof(log_buffer) : log_head;
    size_t start = log_wrapped ? log_head : 0;
    if (len > out_len - 1) {
        size_t drop = len - (out_len - 1);
        start = (start + drop) % sizeof(log_buffer);
        len -= drop;
    }

    for (size_t i = 0; i < len; i++) {
        out[i] = log_buffer[(start + i) % sizeof(log_buffer)];
    }
    portEXIT_CRITICAL(&log_lock);

    out[len] = '\0';
    return len;
}
