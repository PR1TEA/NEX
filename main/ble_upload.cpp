/*
 * NEX firmware — BLE wallpaper upload service
 * SPDX-License-Identifier: MIT
 *
 * Custom GATT service:
 *   control  (write)       0x01 + u32le length = START, 0x02 = COMMIT, 0x03 = ABORT
 *   data     (write / no response) raw JPEG chunks
 *   status   (read)        0 idle, 1 receiving, 2 success, 3 error
 */
#include "ble_upload.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "reminders.h"
#include "wallpaper.h"

namespace nex {
namespace {

constexpr const char *TAG = "NEX-BLE";
constexpr uint32_t kMaxLength = 512 * 1024;

ble_uuid_any_t s_service_uuid;
ble_uuid_any_t s_control_uuid;
ble_uuid_any_t s_data_uuid;
ble_uuid_any_t s_status_uuid;
ble_uuid_any_t s_rem_control_uuid;
ble_uuid_any_t s_rem_data_uuid;
ble_uuid_any_t s_rem_status_uuid;

uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
uint16_t s_control_handle = 0;
uint16_t s_data_handle = 0;
uint16_t s_status_handle = 0;
uint16_t s_rem_control_handle = 0;
uint16_t s_rem_data_handle = 0;
uint16_t s_rem_status_handle = 0;
uint8_t *s_rem_buffer = nullptr;
uint32_t s_rem_length = 0;
uint32_t s_rem_offset = 0;
uint8_t s_rem_status = 0;

uint8_t *s_buffer = nullptr;
uint32_t s_length = 0;
uint32_t s_offset = 0;
bool s_receiving = false;
uint8_t s_status = 0;
bool s_want_advertising = false;
uint8_t s_addr_type = BLE_OWN_ADDR_PUBLIC;

void notify_rem_data(const uint8_t *data, uint16_t length)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_rem_data_handle == 0) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
    if (om != nullptr) {
        ble_gatts_notify_custom(s_conn_handle, s_rem_data_handle, om);
    }
}

void send_reminders()
{
    char *json = static_cast<char *>(std::malloc(8192));
    if (json == nullptr) {
        s_rem_status = 3;
        return;
    }
    const int length = reminders_to_json(json, 8192);
    if (length <= 0) {
        std::free(json);
        s_rem_status = 3;
        return;
    }
    uint8_t header[4] = {
        static_cast<uint8_t>(length & 0xFF),
        static_cast<uint8_t>((length >> 8) & 0xFF),
        static_cast<uint8_t>((length >> 16) & 0xFF),
        static_cast<uint8_t>((length >> 24) & 0xFF),
    };
    notify_rem_data(header, sizeof(header));
    int offset = 0;
    while (offset < length) {
        const int chunk = (length - offset) > 500 ? 500 : (length - offset);
        notify_rem_data(reinterpret_cast<const uint8_t *>(json) + offset, static_cast<uint16_t>(chunk));
        offset += chunk;
    }
    std::free(json);
    s_rem_status = 2;
}

void handle_rem_control(struct ble_gatt_access_ctxt *ctxt)
{
    uint8_t command[5] = {};
    const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    os_mbuf_copydata(ctxt->om, 0, length < sizeof(command) ? length : sizeof(command), command);

    if (command[0] == 0x20) {
        send_reminders();
        return;
    }
    if (command[0] == 0x21 && length >= 5) {
        const uint32_t total = static_cast<uint32_t>(command[1]) |
                               (static_cast<uint32_t>(command[2]) << 8) |
                               (static_cast<uint32_t>(command[3]) << 16) |
                               (static_cast<uint32_t>(command[4]) << 24);
        if (s_rem_buffer != nullptr) {
            std::free(s_rem_buffer);
        }
        s_rem_buffer = static_cast<uint8_t *>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        s_rem_length = (s_rem_buffer != nullptr && total <= 8192) ? total : 0;
        s_rem_offset = 0;
        s_rem_status = s_rem_length > 0 ? 1 : 3;
        return;
    }
    if (command[0] == 0x23) {
        bool ok = s_rem_buffer != nullptr && s_rem_offset == s_rem_length && s_rem_length > 0;
        if (ok) {
            ok = reminders_set_json(reinterpret_cast<const char *>(s_rem_buffer), s_rem_length);
        }
        s_rem_status = ok ? 2 : 3;
        if (s_rem_buffer != nullptr) {
            std::free(s_rem_buffer);
            s_rem_buffer = nullptr;
        }
        s_rem_length = 0;
        s_rem_offset = 0;
    }
}

void release_buffer()
{
    if (s_buffer != nullptr) {
        heap_caps_free(s_buffer);
        s_buffer = nullptr;
    }
    s_length = 0;
    s_offset = 0;
    s_receiving = false;
}

void start_advertising()
{
    if (!s_want_advertising || s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_service_uuid.u128;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields response = {};
    const char *name = "NEX-Watch";
    response.name = reinterpret_cast<uint8_t *>(const_cast<char *>(name));
    response.name_len = std::strlen(name);
    response.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&response);

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, nullptr, BLE_HS_FOREVER, &params, [](struct ble_gap_event *event, void *) -> int {
        switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "phone connected");
            } else {
                start_advertising();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "phone disconnected");
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            release_buffer();
            s_status = 0;
            start_advertising();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising();
            break;
        default:
            break;
        }
        return 0;
    }, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising as NEX-Watch");
    }
}

int gatt_access(uint16_t, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (attr_handle == s_rem_status_handle && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &s_rem_status, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (attr_handle == s_rem_control_handle && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        handle_rem_control(ctxt);
        return 0;
    }

    if (attr_handle == s_rem_data_handle && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
        if (s_rem_buffer == nullptr || length > s_rem_length - s_rem_offset) {
            s_rem_status = 3;
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (os_mbuf_copydata(ctxt->om, 0, length, s_rem_buffer + s_rem_offset) != 0) {
            s_rem_status = 3;
            return BLE_ATT_ERR_UNLIKELY;
        }
        s_rem_offset += length;
        return 0;
    }

    if (attr_handle == s_status_handle && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &s_status, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (attr_handle == s_control_handle && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t command[5] = {};
        const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
        os_mbuf_copydata(ctxt->om, 0, length < sizeof(command) ? length : sizeof(command), command);

        if (command[0] == 0x01 && length >= 5) {
            const uint32_t total = static_cast<uint32_t>(command[1]) |
                                   (static_cast<uint32_t>(command[2]) << 8) |
                                   (static_cast<uint32_t>(command[3]) << 16) |
                                   (static_cast<uint32_t>(command[4]) << 24);
            release_buffer();
            if (total == 0 || total > kMaxLength) {
                s_status = 3;
                return 0;
            }
            s_buffer = static_cast<uint8_t *>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (s_buffer == nullptr) {
                s_status = 3;
                return 0;
            }
            s_length = total;
            s_offset = 0;
            s_receiving = true;
            s_status = 1;
            ESP_LOGI(TAG, "upload start: %u bytes", (unsigned)total);
            return 0;
        }

        if (command[0] == 0x02) {
            const bool complete = s_receiving && s_offset == s_length;
            const bool saved = complete && wallpaper_save(s_buffer, s_length);
            s_status = saved ? 2 : 3;
            ESP_LOGI(TAG, "upload commit: %s (%u/%u)", saved ? "ok" : "failed",
                     (unsigned)s_offset, (unsigned)s_length);
            release_buffer();
            return 0;
        }

        if (command[0] == 0x03) {
            release_buffer();
            s_status = 0;
            return 0;
        }
        return 0;
    }

    if (attr_handle == s_data_handle && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
        if (!s_receiving || s_buffer == nullptr || length > s_length - s_offset) {
            s_status = 3;
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (os_mbuf_copydata(ctxt->om, 0, length, s_buffer + s_offset) != 0) {
            s_status = 3;
            return BLE_ATT_ERR_UNLIKELY;
        }
        s_offset += length;
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

const struct ble_gatt_svc_def kServices[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_control_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_control_handle,
            },
            {
                .uuid = &s_data_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_data_handle,
            },
            {
                .uuid = &s_status_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
                .val_handle = &s_status_handle,
            },
            {
                .uuid = &s_rem_control_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_rem_control_handle,
            },
            {
                .uuid = &s_rem_data_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_rem_data_handle,
            },
            {
                .uuid = &s_rem_status_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
                .val_handle = &s_rem_status_handle,
            },
            {0},
        },
    },
    {0},
};

void host_task(void *)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void on_sync()
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_addr_type);
    ESP_LOGI(TAG, "host synced, addr type %u", s_addr_type);
    start_advertising();
}

void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset: %d", reason);
}

}  // namespace

bool ble_upload_init()
{
    if (ble_uuid_from_str(&s_service_uuid, "6e657865-0001-4e45-582d-574154434800") != 0 ||
        ble_uuid_from_str(&s_control_uuid, "6e657865-0002-4e45-582d-574154434800") != 0 ||
        ble_uuid_from_str(&s_data_uuid, "6e657865-0003-4e45-582d-574154434800") != 0 ||
        ble_uuid_from_str(&s_status_uuid, "6e657865-0004-4e45-582d-574154434800") != 0 ||
        ble_uuid_from_str(&s_rem_control_uuid, "6e657865-0005-4e45-582d-574154434800") != 0 ||
        ble_uuid_from_str(&s_rem_data_uuid, "6e657865-0006-4e45-582d-574154434800") != 0 ||
        ble_uuid_from_str(&s_rem_status_uuid, "6e657865-0007-4e45-582d-574154434800") != 0) {
        ESP_LOGE(TAG, "invalid UUID");
        return false;
    }

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return false;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(kServices);
    if (ble_gatts_add_svcs(kServices) != 0) {
        ESP_LOGE(TAG, "add services failed");
        return false;
    }
    ble_svc_gap_device_name_set("NEX-Watch");

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE upload service initialized");
    return true;
}

void ble_upload_advertise(bool enabled)
{
    s_want_advertising = enabled;
    if (enabled) {
        start_advertising();
    } else {
        ble_gap_adv_stop();
    }
}

bool ble_upload_connected()
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

int ble_upload_progress()
{
    if (!s_receiving || s_length == 0) {
        return -1;
    }
    return static_cast<int>((static_cast<uint64_t>(s_offset) * 100) / s_length);
}

int ble_upload_status()
{
    return s_status;
}

}  // namespace nex
