#include "ble.h"
#include "identity/identity.h"
#include "led_matrix/led_matrix.h"
#include "display/display.h"

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

/* ── Company ID ─────────────────────────────────────────────────────────────
 * 0xFFFF is reserved for internal/test use in the Bluetooth spec.
 * Register a company ID with the Bluetooth SIG before shipping.
 */
#define EW_COMPANY_ID    0xFFFF

/* Minimum RSSI (dBm) to count as a proximity encounter (~1-2 m in open air) */
#define EW_ENCOUNTER_RSSI (-70)

/* ── Manufacturer-specific advertising data layout (9 bytes) ─────────────
 * [0..1] company_id  (LE uint16)
 * [2..5] watch_hash  (LE uint32)
 * [6]    dev_distance (uint8)
 * [7..8] encounter_count (LE uint16)
 */
#define MFR_LEN 9
static uint8_t mfr_data[MFR_LEN];

static void build_mfr_data(void)
{
    sys_put_le16(EW_COMPANY_ID,              mfr_data + 0);
    sys_put_le32(identity_hash(),            mfr_data + 2);
    mfr_data[6] = identity_dev_distance();
    sys_put_le16(identity_encounter_count(), mfr_data + 7);
}

static struct bt_data adv_data[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_MANUFACTURER_DATA, mfr_data, MFR_LEN),
};

static const struct bt_data scan_rsp[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE,
            CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static struct bt_conn *phone_conn;
static bool adv_running;

static void start_adv(void)
{
    build_mfr_data();
    int rc = bt_le_adv_start(BT_LE_ADV_CONN,
                             adv_data, ARRAY_SIZE(adv_data),
                             scan_rsp, ARRAY_SIZE(scan_rsp));
    if (rc) {
        LOG_ERR("adv_start failed: %d", rc);
        return;
    }
    adv_running = true;
    LOG_INF("Advertising started (hash=0x%08X)", identity_hash());
}

void ble_update_adv(void)
{
    if (!adv_running) {
        return;
    }
    bt_le_adv_stop();
    adv_running = false;
    start_adv();
}

/* ── Connection callbacks ──────────────────────────────────────────────── */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_WRN("Connection failed: %d", err);
        return;
    }
    phone_conn = bt_conn_ref(conn);
    adv_running = false;
    LOG_INF("Phone connected");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    bt_conn_unref(phone_conn);
    phone_conn = NULL;
    LOG_INF("Phone disconnected (reason=%d)", reason);
    start_adv();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = on_connected,
    .disconnected = on_disconnected,
};

/* ── Passive scanner — encounter detection ────────────────────────────── */

static bool parse_adv(struct bt_data *data, void *user_data)
{
    int8_t *rssi = user_data;

    if (data->type != BT_DATA_MANUFACTURER_DATA) {
        return true;  /* keep iterating */
    }
    if (data->data_len < MFR_LEN) {
        return true;
    }

    uint16_t company = sys_get_le16(data->data);
    if (company != EW_COMPANY_ID) {
        return true;
    }

    /* Another Every Watch found */
    uint32_t their_hash    = sys_get_le32(data->data + 2);
    uint8_t  their_dist    = data->data[6];

    if (*rssi < EW_ENCOUNTER_RSSI) {
        return false;  /* too far — skip */
    }

    bool was_new = (identity_encounter_count() ==
                    identity_encounter_count()); /* capture before */
    uint16_t before = identity_encounter_count();

    identity_on_encounter(their_hash, their_dist);

    if (identity_encounter_count() != before) {
        ble_update_adv();
    }

    ARG_UNUSED(was_new);
    return false;
}

static void on_scan_recv(const struct bt_le_scan_recv_info *info,
                         struct net_buf_simple *buf)
{
    int8_t rssi = info->rssi;
    bt_data_parse(buf, parse_adv, &rssi);
}

static struct bt_le_scan_cb scan_callbacks = {
    .recv = on_scan_recv,
};

/* ── GATT service — phone notification writes ─────────────────────────── */

/*
 * UUID base: EE55xxxx-0000-0000-0000-000000000000
 *   0001 = EveryWatch primary service
 *   0002 = Notification characteristic (WRITE)
 *   0003 = Watch info characteristic   (READ)
 */
#define BT_UUID_EW_SVC \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0xEE550001, 0x0000, 0x0000, 0x0000, 0x000000000000))

#define BT_UUID_EW_NOTIF \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0xEE550002, 0x0000, 0x0000, 0x0000, 0x000000000000))

#define BT_UUID_EW_INFO \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0xEE550003, 0x0000, 0x0000, 0x0000, 0x000000000000))

/*
 * Notification write payload:
 *   byte 0: category (0=generic, 1=call, 2=message, 3=alarm)
 *   bytes 1-N: title text (UTF-8, up to 19 bytes)
 *
 * The top row (row 0) of LED_LAYER_NOTIFICATION lights up in a
 * category-specific color and wakes the display.
 */
static const struct led_rgb notif_colors[] = {
    [0] = {  0,  80, 255},  /* generic  — blue  */
    [1] = {  0, 255,  0 },  /* call     — green */
    [2] = {255, 255, 255},  /* message  — white */
    [3] = {255,   0,  0 },  /* alarm    — red   */
};

static void show_notification(uint8_t category)
{
    if (category >= ARRAY_SIZE(notif_colors)) {
        category = 0;
    }

    /* Light the top row on the notification layer */
    memset(led_mask[LED_LAYER_NOTIFICATION], 0,
           sizeof(led_mask[LED_LAYER_NOTIFICATION]));
    for (int col = 0; col < LED_COLS; col++) {
        led_mask[LED_LAYER_NOTIFICATION][0][col] = 1;
    }
    led_layer_color[LED_LAYER_NOTIFICATION] = notif_colors[category];

    display_on();
    led_commit();
}

static ssize_t on_notif_write(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len,
                              uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset); ARG_UNUSED(flags);

    if (len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const uint8_t *data = buf;
    uint8_t category = data[0];

    if (len > 1) {
        LOG_INF("Notification cat=%u: %.*s", category, (int)(len - 1), data + 1);
    }

    show_notification(category);
    return len;
}

/* Watch info: [hash:4][dev_dist:1][enc_count:2] */
static ssize_t on_info_read(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset)
{
    uint8_t info[7];
    sys_put_le32(identity_hash(),            info + 0);
    info[4] = identity_dev_distance();
    sys_put_le16(identity_encounter_count(), info + 5);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, info, sizeof(info));
}

BT_GATT_SERVICE_DEFINE(ew_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_EW_SVC),
    BT_GATT_CHARACTERISTIC(BT_UUID_EW_NOTIF,
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL, on_notif_write, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_EW_INFO,
        BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ,
        on_info_read, NULL, NULL),
);

/* ── Init ──────────────────────────────────────────────────────────────── */

static void bt_ready(int err)
{
    if (err) {
        LOG_ERR("bt_enable failed: %d", err);
        return;
    }

    bt_le_scan_cb_register(&scan_callbacks);

    static const struct bt_le_scan_param scan_param = {
        .type     = BT_LE_SCAN_TYPE_PASSIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window   = BT_GAP_SCAN_FAST_WINDOW,
    };
    int rc = bt_le_scan_start(&scan_param, NULL);
    if (rc) {
        LOG_ERR("scan_start failed: %d", rc);
    }

    start_adv();
}

void ble_init(void)
{
    int rc = bt_enable(bt_ready);
    if (rc) {
        LOG_ERR("bt_enable failed: %d", rc);
    }
}

bool ble_is_connected(void)
{
    return phone_conn != NULL;
}
