#include "ble_gap.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "accessory_config.h"
#include "bosch_ldi.h"
#include "esp_log.h"
#include "gatt_client.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "persistent_log.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "status_led.h"

static const char *TAG = "ble_gap";
static const char *ADV_SHORT_NAME = "LDI";
static uint8_t own_addr_type;
static char ble_device_name[ACCESSORY_DEVICE_NAME_MAX_LEN + 1] = ACCESSORY_DEVICE_NAME_DEFAULT;

const ble_uuid128_t bosch_ldi_service_uuid =
    BLE_UUID128_INIT(0xe4, 0xcc, 0xdb, 0xe2, 0x2a, 0x2a, 0xb4, 0x81,
                     0xe9, 0x11, 0xa2, 0xea, 0x20, 0xeb, 0x00, 0x00);

const ble_uuid128_t bosch_ldi_characteristic_uuid =
    BLE_UUID128_INIT(0xe4, 0xcc, 0xdb, 0xe2, 0x2a, 0x2a, 0xb4, 0x81,
                     0xe9, 0x11, 0xa2, 0xea, 0x21, 0xeb, 0x00, 0x00);

static int gap_event_cb(struct ble_gap_event *event, void *arg);

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset; reason=%d", reason);
    persistent_log_event("error", "ble", "NimBLE reset reason=%d", reason);
    status_led_set(STATUS_LED_ERROR);
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "address inference failed; rc=%d", rc);
        persistent_log_event("error", "ble", "address inference failed rc=%d", rc);
        return;
    }

    app_ble_gap_start_advertising();
}

void app_ble_gap_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    accessory_config_load_device_name(ble_device_name, sizeof(ble_device_name));
    ble_svc_gap_device_name_set(ble_device_name);
    ble_svc_gap_device_appearance_set(BOSCH_LDI_APPEARANCE_GENERIC_CYCLING);

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

}

void app_ble_gap_set_device_name(const char *name)
{
    if (!accessory_config_device_name_is_valid(name)) {
        return;
    }

    strlcpy(ble_device_name, name, sizeof(ble_device_name));
    ble_svc_gap_device_name_set(ble_device_name);
    ESP_LOGI(TAG, "BLE device name set to \"%s\"", ble_device_name);
    persistent_log_event("info", "ble", "device name set name=%s", ble_device_name);

    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "advertising restart after name change failed; rc=%d", rc);
    }
}

void app_ble_gap_start_advertising(void)
{
    struct ble_hs_adv_fields adv_fields;
    struct ble_hs_adv_fields rsp_fields;
    struct ble_gap_adv_params adv_params;
    const char *name = ble_svc_gap_device_name();
    int rc;

    memset(&adv_fields, 0, sizeof(adv_fields));
    adv_fields.flags = BLE_HS_ADV_F_DISC_LTD | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.sol_uuids128 = &bosch_ldi_service_uuid;
    adv_fields.sol_num_uuids128 = 1;
    adv_fields.appearance = BOSCH_LDI_APPEARANCE_GENERIC_CYCLING;
    adv_fields.appearance_is_present = 1;
    adv_fields.name = (const uint8_t *)ADV_SHORT_NAME;
    adv_fields.name_len = strlen(ADV_SHORT_NAME);
    adv_fields.name_is_complete = 0;

    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising field setup failed; rc=%d", rc);
        persistent_log_event("error", "ble", "advertising field setup failed rc=%d", rc);
        return;
    }

    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (const uint8_t *)name;
    rsp_fields.name_len = strlen(name);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan response setup failed; rc=%d", rc);
        persistent_log_event("error", "ble", "scan response setup failed rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_LTD;
    adv_params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL2_MIN;
    adv_params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL2_MAX;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "advertising started; name=\"%s\" soliciting service eb20", name);
        persistent_log_event("info", "ble", "advertising started name=%s short_name=%s",
                             name, ADV_SHORT_NAME);
        status_led_set(STATUS_LED_ADVERTISING);
    } else {
        ESP_LOGE(TAG, "advertising start failed; rc=%d", rc);
        persistent_log_event("error", "ble", "advertising start failed rc=%d", rc);
        status_led_set(STATUS_LED_ERROR);
    }
}

static void log_conn_state(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        ESP_LOGW(TAG, "connection state unavailable; handle=%u rc=%d", conn_handle, rc);
        return;
    }

    ESP_LOGI(TAG,
             "conn=%u role=%u itvl=%u latency=%u timeout=%u encrypted=%u bonded=%u key=%u",
             conn_handle, desc.role, desc.conn_itvl, desc.conn_latency,
             desc.supervision_timeout, desc.sec_state.encrypted,
             desc.sec_state.bonded, desc.sec_state.key_size);
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            struct ble_gap_conn_desc desc;
            bool already_encrypted = false;

            ESP_LOGI(TAG, "connection established; handle=%u", event->connect.conn_handle);
            persistent_log_event("info", "bike_connection",
                                 "connection established handle=%u", event->connect.conn_handle);
            status_led_set(STATUS_LED_CONNECTED);
            log_conn_state(event->connect.conn_handle);
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc == 0) {
                already_encrypted = desc.sec_state.encrypted;
            }
            gatt_client_on_connect(event->connect.conn_handle);

            if (already_encrypted) {
                ESP_LOGI(TAG, "connection already encrypted; skipping security request");
                persistent_log_event("info", "bike_connection",
                                     "connection already encrypted handle=%u",
                                     event->connect.conn_handle);
            } else {
                rc = ble_gap_security_initiate(event->connect.conn_handle);
                if (rc == 0 || rc == BLE_HS_EALREADY) {
                    ESP_LOGI(TAG, "security requested; handle=%u", event->connect.conn_handle);
                } else {
                    ESP_LOGW(TAG, "security request failed; rc=%d", rc);
                    persistent_log_event("error", "bike_connection",
                                         "security request failed handle=%u rc=%d",
                                         event->connect.conn_handle, rc);
                }
            }
        } else {
            ESP_LOGW(TAG, "connection failed; status=%d", event->connect.status);
            persistent_log_event("error", "bike_connection",
                                 "connection failed status=%d", event->connect.status);
            status_led_set(STATUS_LED_ERROR);
            app_ble_gap_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected; reason=%d", event->disconnect.reason);
        persistent_log_event("info", "bike_connection",
                             "disconnected reason=%d", event->disconnect.reason);
        gatt_client_reset();
        app_ble_gap_start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertising complete; reason=%d", event->adv_complete.reason);
        app_ble_gap_start_advertising();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "pairing/encryption result; status=%d handle=%u",
                 event->enc_change.status, event->enc_change.conn_handle);
        log_conn_state(event->enc_change.conn_handle);
        if (event->enc_change.status == 0) {
            persistent_log_event("info", "bike_connection",
                                 "encrypted handle=%u", event->enc_change.conn_handle);
            status_led_set(STATUS_LED_SECURED);
            gatt_client_on_encrypted(event->enc_change.conn_handle);
        } else {
            persistent_log_event("error", "bike_connection",
                                 "encryption failed handle=%u status=%d",
                                 event->enc_change.conn_handle, event->enc_change.status);
            status_led_set(STATUS_LED_ERROR);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        gatt_client_on_notify(event->notify_rx.conn_handle,
                              event->notify_rx.attr_handle,
                              event->notify_rx.om);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "ATT MTU update; conn=%u mtu=%u",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_DATA_LEN_CHG:
        ESP_LOGI(TAG, "LE data length changed; conn=%u max_tx_octets=%u max_rx_octets=%u",
                 event->data_len_chg.conn_handle,
                 event->data_len_chg.max_tx_octets,
                 event->data_len_chg.max_rx_octets);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            ESP_LOGW(TAG, "repeat pairing; deleting old bond and accepting new one");
            ble_gap_unpair(&desc.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }

    default:
        return 0;
    }
}
