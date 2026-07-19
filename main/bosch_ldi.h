#pragma once

#include <stdint.h>
#include "host/ble_uuid.h"

#define BOSCH_LDI_APPEARANCE_GENERIC_CYCLING 0x0480
#define BOSCH_LDI_MIN_ATT_MTU 247
#define BOSCH_LDI_MIN_LL_OCTETS 251

#define BOSCH_LDI_SERVICE_UUID16 0xeb20
#define BOSCH_LDI_CHARACTERISTIC_UUID16 0xeb21

extern const ble_uuid128_t bosch_ldi_service_uuid;
extern const ble_uuid128_t bosch_ldi_characteristic_uuid;
