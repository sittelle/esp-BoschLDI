#include "gatt_client.h"

#include <string.h>

#include "bosch_ldi.h"
#include "esp_log.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "live_data_decode.h"
#include "os/os_mbuf.h"
#include "persistent_log.h"
#include "status_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "gatt_client";

typedef struct {
    uint16_t conn_handle;
    uint16_t service_start;
    uint16_t service_end;
    uint16_t live_data_handle;
    uint16_t live_data_cccd_handle;
    bool mtu_ok;
    bool discovery_started;
} client_state_t;

typedef struct {
    char source[12];
    uint16_t len;
    uint8_t data[244];
} live_data_event_t;

static client_state_t state;
static QueueHandle_t live_data_queue;

static int mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t mtu, void *arg);
static int service_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *service, void *arg);
static int chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg);
static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                  void *arg);
static int cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg);
static int read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg);
static void ensure_live_data_worker(void);

void gatt_client_reset(void)
{
    memset(&state, 0, sizeof(state));
}

void gatt_client_on_connect(uint16_t conn_handle)
{
    ensure_live_data_worker();
    gatt_client_reset();
    state.conn_handle = conn_handle;

    int rc = ble_gap_write_sugg_def_data_len(BOSCH_LDI_MIN_LL_OCTETS, 2120);
    if (rc != 0) {
        ESP_LOGW(TAG, "default DLE request failed; rc=%d", rc);
        persistent_log_event("warn", "bike_connection", "default DLE request failed rc=%d", rc);
    }

    rc = ble_gap_set_data_len(conn_handle, BOSCH_LDI_MIN_LL_OCTETS, 2120);
    if (rc != 0) {
        ESP_LOGW(TAG, "connection DLE request failed; rc=%d", rc);
        persistent_log_event("warn", "bike_connection",
                             "connection DLE request failed handle=%u rc=%d", conn_handle, rc);
    }

    rc = ble_gattc_exchange_mtu(conn_handle, mtu_cb, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "ATT MTU exchange requested; minimum=%u", BOSCH_LDI_MIN_ATT_MTU);
    } else {
        ESP_LOGE(TAG, "ATT MTU exchange request failed; rc=%d", rc);
        persistent_log_event("error", "bike_connection",
                             "ATT MTU exchange request failed handle=%u rc=%d", conn_handle, rc);
    }
}

void gatt_client_on_encrypted(uint16_t conn_handle)
{
    if (state.conn_handle != conn_handle) {
        return;
    }

    if (!state.mtu_ok) {
        ESP_LOGI(TAG, "encrypted; waiting for MTU before service discovery");
        return;
    }

    if (state.discovery_started) {
        return;
    }

    state.discovery_started = true;
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle,
                                        &bosch_ldi_service_uuid.u,
                                        service_cb, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "service discovery started; uuid=eb20");
    } else {
        ESP_LOGE(TAG, "service discovery start failed; rc=%d", rc);
    }
}

static int mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t mtu, void *arg)
{
    (void)arg;
    if (error->status != 0) {
        ESP_LOGE(TAG, "ATT MTU exchange failed; status=%u", error->status);
        persistent_log_event("error", "bike_connection",
                             "ATT MTU exchange failed handle=%u status=%u",
                             conn_handle, error->status);
        return 0;
    }

    state.mtu_ok = mtu >= BOSCH_LDI_MIN_ATT_MTU;
    ESP_LOGI(TAG, "ATT MTU negotiated; mtu=%u ok=%u", mtu, state.mtu_ok);
    if (!state.mtu_ok) {
        ESP_LOGW(TAG, "MTU below Bosch required minimum; bike may disconnect");
        persistent_log_event("warn", "bike_connection",
                             "MTU below Bosch minimum handle=%u mtu=%u", conn_handle, mtu);
    }

    gatt_client_on_encrypted(conn_handle);
    return 0;
}

static int service_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (state.service_start == 0) {
            ESP_LOGE(TAG, "Live Data service eb20 not found");
            persistent_log_event("error", "bike_connection", "Live Data service eb20 not found");
            status_led_set(STATUS_LED_ERROR);
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "service discovery failed; status=%u", error->status);
        persistent_log_event("error", "bike_connection",
                             "service discovery failed status=%u", error->status);
        status_led_set(STATUS_LED_ERROR);
        return 0;
    }

    state.service_start = service->start_handle;
    state.service_end = service->end_handle;
    ESP_LOGI(TAG, "service found; start=%u end=%u",
             state.service_start, state.service_end);

    int rc = ble_gattc_disc_chrs_by_uuid(conn_handle, state.service_start,
                                         state.service_end,
                                         &bosch_ldi_characteristic_uuid.u,
                                         chr_cb, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "characteristic discovery started; uuid=eb21");
    } else {
        ESP_LOGE(TAG, "characteristic discovery start failed; rc=%d", rc);
        persistent_log_event("error", "bike_connection",
                             "characteristic discovery start failed rc=%d", rc);
    }
    return 0;
}

static int chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (state.live_data_handle == 0) {
            ESP_LOGE(TAG, "Live Data characteristic eb21 not found");
            persistent_log_event("error", "bike_connection",
                                 "Live Data characteristic eb21 not found");
            status_led_set(STATUS_LED_ERROR);
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "characteristic discovery failed; status=%u", error->status);
        persistent_log_event("error", "bike_connection",
                             "characteristic discovery failed status=%u", error->status);
        status_led_set(STATUS_LED_ERROR);
        return 0;
    }

    state.live_data_handle = chr->val_handle;
    ESP_LOGI(TAG, "characteristic found; def=%u value=%u properties=0x%02x",
             chr->def_handle, chr->val_handle, chr->properties);

    ESP_LOGI(TAG, "descriptor discovery range; start=%u end=%u",
             chr->val_handle, state.service_end);
    persistent_log_event("info", "bike_connection",
                         "descriptor discovery range start=%u end=%u",
                         chr->val_handle, state.service_end);

    int rc = ble_gattc_disc_all_dscs(conn_handle, chr->val_handle,
                                     state.service_end, dsc_cb, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "descriptor discovery started");
    } else {
        ESP_LOGE(TAG, "descriptor discovery start failed; rc=%d", rc);
        persistent_log_event("error", "bike_connection",
                             "descriptor discovery start failed rc=%d", rc);
    }
    return 0;
}

static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    (void)arg;
    (void)chr_val_handle;
    if (error->status == BLE_HS_EDONE) {
        if (state.live_data_cccd_handle == 0) {
            ESP_LOGE(TAG, "Live Data CCCD 0x2902 not found");
            persistent_log_event("error", "bike_connection", "Live Data CCCD 0x2902 not found");
            status_led_set(STATUS_LED_ERROR);
            return 0;
        }

        const uint8_t notify_enable[2] = {0x01, 0x00};
        int rc = ble_gattc_write_flat(conn_handle, state.live_data_cccd_handle,
                                      notify_enable, sizeof(notify_enable),
                                      cccd_write_cb, NULL);
        if (rc == 0) {
            ESP_LOGI(TAG, "CCCD subscription write started; handle=%u",
                     state.live_data_cccd_handle);
        } else {
            ESP_LOGE(TAG, "CCCD subscription write failed to start; rc=%d", rc);
            persistent_log_event("error", "bike_connection",
                                 "CCCD subscription write failed to start rc=%d", rc);
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "descriptor discovery failed; status=%u", error->status);
        persistent_log_event("error", "bike_connection",
                             "descriptor discovery failed status=%u", error->status);
        status_led_set(STATUS_LED_ERROR);
        return 0;
    }

    if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
        state.live_data_cccd_handle = dsc->handle;
        ESP_LOGI(TAG, "CCCD found; handle=%u", dsc->handle);
    } else {
        ESP_LOGI(TAG, "descriptor found; handle=%u", dsc->handle);
    }
    return 0;
}

static int cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        ESP_LOGE(TAG, "CCCD subscription failed; status=%u", error->status);
        persistent_log_event("error", "bike_connection",
                             "CCCD subscription failed status=%u", error->status);
        status_led_set(STATUS_LED_ERROR);
        return 0;
    }

    ESP_LOGI(TAG, "CCCD subscription complete; notifications enabled");
    persistent_log_event("info", "bike_connection", "Live Data notifications enabled");
    status_led_set(STATUS_LED_READY);
    int rc = ble_gattc_read(conn_handle, state.live_data_handle, read_cb, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "initial Live Data read started; handle=%u",
                 state.live_data_handle);
    } else {
        ESP_LOGW(TAG, "initial Live Data read failed to start; rc=%d", rc);
        persistent_log_event("warn", "bike_connection",
                             "initial Live Data read failed to start rc=%d", rc);
    }
    return 0;
}

static void live_data_worker_task(void *arg)
{
    (void)arg;
    live_data_event_t event;

    while (true) {
        if (xQueueReceive(live_data_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "%s raw length=%u", event.source, event.len);
        if (!live_data_decode_and_log(event.data, event.len)) {
            ESP_LOGW(TAG, "Live Data protobuf decode failed");
            persistent_log_event("warn", "bike_data",
                                 "Live Data protobuf decode failed len=%u", event.len);
        }
    }
}

static void ensure_live_data_worker(void)
{
    if (live_data_queue != NULL) {
        return;
    }

    live_data_queue = xQueueCreate(4, sizeof(live_data_event_t));
    if (live_data_queue == NULL) {
        ESP_LOGE(TAG, "Live Data queue allocation failed");
        return;
    }

    BaseType_t ok = xTaskCreate(live_data_worker_task, "live_data_worker",
                                4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Live Data worker task allocation failed");
        vQueueDelete(live_data_queue);
        live_data_queue = NULL;
    }
}

static void enqueue_mbuf(const char *source, const struct os_mbuf *om)
{
    uint16_t len = OS_MBUF_PKTLEN(om);
    live_data_event_t event = {0};

    if (len > sizeof(event.data)) {
        ESP_LOGW(TAG, "%s too large for Bosch minimum ATT payload buffer; len=%u", source, len);
        return;
    }
    if (live_data_queue == NULL) {
        ESP_LOGW(TAG, "Live Data queue unavailable; dropping %s len=%u", source, len);
        return;
    }

    strlcpy(event.source, source, sizeof(event.source));
    event.len = len;

    int rc = os_mbuf_copydata(om, 0, len, event.data);
    if (rc != 0) {
        ESP_LOGE(TAG, "mbuf copy failed; rc=%d", rc);
        return;
    }

    if (xQueueSend(live_data_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Live Data queue full; dropping %s len=%u", source, len);
    }
}

static int read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGW(TAG, "initial Live Data read failed; status=%u", error->status);
        persistent_log_event("warn", "bike_connection",
                             "initial Live Data read failed status=%u", error->status);
        return 0;
    }
    enqueue_mbuf("read", attr->om);
    return 0;
}

void gatt_client_on_notify(uint16_t conn_handle, uint16_t attr_handle,
                           const struct os_mbuf *om)
{
    if (conn_handle != state.conn_handle || attr_handle != state.live_data_handle) {
        ESP_LOGD(TAG, "notification ignored; conn=%u attr=%u", conn_handle, attr_handle);
        return;
    }

    enqueue_mbuf("notification", om);
}
