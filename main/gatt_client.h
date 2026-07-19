#pragma once

#include <stdint.h>
#include "host/ble_gap.h"

void gatt_client_reset(void);
void gatt_client_on_connect(uint16_t conn_handle);
void gatt_client_on_encrypted(uint16_t conn_handle);
void gatt_client_on_notify(uint16_t conn_handle, uint16_t attr_handle,
                           const struct os_mbuf *om);
