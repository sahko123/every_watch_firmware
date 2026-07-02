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
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_ERR);

/* ── Company ID ─────────────────────────────────────────────────────────────
 * 0xFFFF is reserved for internal/test use in the Bluetooth spec.
 * TODO: Register a company ID with the Bluetooth SIG before shipping.
 *       https://www.bluetooth.com/specifications/assigned-numbers/
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
static atomic_t adv_running = ATOMIC_INIT(0);

/* Switch to slow advertising after this many ms of fast advertising */
#define ADV_FAST_DURATION_MS 30000

static const struct bt_le_adv_param adv_slow_param =
    BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_USE_IDENTITY,
                         BT_GAP_ADV_SLOW_INT_MIN,   /* 1000 ms */
                         BT_GAP_ADV_SLOW_INT_MAX,   /* 1280 ms */
                         NULL);

static void adv_slow_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(adv_slow_work, adv_slow_fn);

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
    atomic_set(&adv_running, 1);
    k_work_reschedule(&adv_slow_work, K_MSEC(ADV_FAST_DURATION_MS));
}

static void adv_slow_fn(struct k_work *w)
{
    ARG_UNUSED(w);
    if (!atomic_get(&adv_running)) {
        return;
    }
    bt_le_adv_stop();
    build_mfr_data();
    int rc = bt_le_adv_start(&adv_slow_param,
                             adv_data, ARRAY_SIZE(adv_data),
                             scan_rsp, ARRAY_SIZE(scan_rsp));
    if (rc) {
        LOG_ERR("slow adv_start failed: %d", rc);
        atomic_set(&adv_running, 0);
    }
}

/* adv_update_work: restart advertising from workqueue context.
 * Called via k_work_submit from parse_adv (BT scan callback) — never call
 * bt_le_adv_stop/start directly from the BT RX thread. */
static void adv_update_work_fn(struct k_work *w)
{
    ARG_UNUSED(w);
    if (!atomic_get(&adv_running)) {
        return;
    }
    bt_le_adv_stop();
    atomic_set(&adv_running, 0);
    start_adv();
}
static K_WORK_DEFINE(adv_update_work, adv_update_work_fn);

void ble_update_adv(void)
{
    if (!atomic_get(&adv_running)) {
        return;
    }
    k_work_submit(&adv_update_work);
}

/* ── Connection callbacks ──────────────────────────────────────────────── */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("Connection failed: %d", err);
        return;
    }
    phone_conn = bt_conn_ref(conn);
    atomic_set(&adv_running, 0);
    k_work_cancel_delayable(&adv_slow_work);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (phone_conn) {
        bt_conn_unref(phone_conn);
        phone_conn = NULL;
    }
    LOG_ERR("Phone disconnected (reason=%d)", reason);
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
    uint32_t their_hash = sys_get_le32(data->data + 2);
    uint8_t  their_dist = data->data[6];

    if (*rssi < EW_ENCOUNTER_RSSI) {
        return false;  /* too far — skip */
    }

    uint16_t before = identity_encounter_count();
    identity_on_encounter(their_hash, their_dist);
    if (identity_encounter_count() != before) {
        ble_update_adv();  /* submits adv_update_work — safe from scan callback */
    }
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

    /* Runs on BT RX thread — must hold led_mask_mutex before writing led_mask */
    k_mutex_lock(&led_mask_mutex, K_FOREVER);
    memset(led_mask[LED_LAYER_NOTIFICATION], 0,
           sizeof(led_mask[LED_LAYER_NOTIFICATION]));
    for (int col = 0; col < LED_COLS; col++) {
        led_mask[LED_LAYER_NOTIFICATION][0][col] = 1;
    }
    led_layer_color[LED_LAYER_NOTIFICATION] = notif_colors[category];
    k_mutex_unlock(&led_mask_mutex);

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
        LOG_ERR("Notification cat=%u text len=%d", category, (int)(len - 1));
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
        BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_WRITE_ENCRYPT,
        NULL, on_notif_write, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_EW_INFO,
        BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ_ENCRYPT,
        on_info_read, NULL, NULL),
);

/* ── Init ──────────────────────────────────────────────────────────────── */

static void bt_ready(int err)
{
    if (err) {
        LOG_ERR("bt_enable async failed: %d — rebooting", err);
        sys_reboot(SYS_REBOOT_COLD);
        return;
    }

    settings_load();

    bt_le_scan_cb_register(&scan_callbacks);

    /* 10% scan duty cycle: 500 ms interval, 50 ms window.
     * Down from 100% (30/30 ms) — reduces scan current ~6x. */
    static const struct bt_le_scan_param scan_param = {
        .type     = BT_LE_SCAN_TYPE_PASSIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = 0x0320,   /* 500 ms */
        .window   = 0x0050,   /* 50 ms  */
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
        LOG_ERR("bt_enable failed (%d) — rebooting", rc);
        sys_reboot(SYS_REBOOT_COLD);
    }
}
