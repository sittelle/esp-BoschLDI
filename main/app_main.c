#include "ble_gap.h"
#include "log_store.h"
#include "persistent_log.h"
#include "status_led.h"
#include "telemetry_export.h"
#include "wifi_admin.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "host/ble_hs.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "app";

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "POWERON";
    case ESP_RST_EXT:
        return "EXT";
    case ESP_RST_SW:
        return "SW";
    case ESP_RST_PANIC:
        return "PANIC";
    case ESP_RST_INT_WDT:
        return "INT_WDT";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT";
    case ESP_RST_WDT:
        return "WDT";
    case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT";
    case ESP_RST_SDIO:
        return "SDIO";
    case ESP_RST_USB:
        return "USB";
    case ESP_RST_JTAG:
        return "JTAG";
    case ESP_RST_EFUSE:
        return "EFUSE";
    case ESP_RST_PWR_GLITCH:
        return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP:
        return "CPU_LOCKUP";
    case ESP_RST_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_reset_reason_t reset_reason = esp_reset_reason();
    esp_rom_printf("bosch_ldi: app_main entered\r\n");
    esp_rom_printf("bosch_ldi: reset reason=%s(%d)\r\n",
                   reset_reason_name(reset_reason), (int)reset_reason);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    esp_rom_printf("bosch_ldi: nvs ready\r\n");
    log_store_init();
    ESP_LOGI(TAG, "log store initialized");

    err = persistent_log_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persistent log init failed; err=%s", esp_err_to_name(err));
    } else {
        persistent_log_event("info", "system", "boot reset_reason=%s(%d)",
                             reset_reason_name(reset_reason), (int)reset_reason);
    }

    err = status_led_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "status LED init failed; err=%s", esp_err_to_name(err));
        esp_rom_printf("bosch_ldi: status LED init failed err=0x%x\r\n", err);
    } else {
        esp_rom_printf("bosch_ldi: status LED init ok\r\n");
    }

    err = wifi_admin_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi admin init failed; err=%s", esp_err_to_name(err));
        esp_rom_printf("bosch_ldi: wifi admin init failed err=0x%x\r\n", err);
    } else {
        esp_rom_printf("bosch_ldi: wifi admin init ok\r\n");
    }

    err = telemetry_export_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "telemetry exporter init failed; err=%s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(nimble_port_init());
    esp_rom_printf("bosch_ldi: nimble init ok\r\n");
    app_ble_gap_init();

    ESP_LOGI(TAG, "boot complete; Bosch Live Data accessory starting");
    esp_rom_printf("bosch_ldi: boot complete\r\n");
    nimble_port_freertos_init(ble_host_task);
}
