#include "ble.h"
#include "identity/identity.h"
#include "led_matrix/led_matrix.h"
#include "display/display.h"
#include "battery/battery.h"
#include "ui/ui.h"
#include "time_sync/time_sync.h"

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

/* INF, matching every other module. At ERR this reported only failures, so the
 * radio's entire working state — advertising, connections, pairing — was
 * invisible, and "no output" meant either healthy or dead with no way to tell.
 * Production caps the global level anyway, so this costs nothing there. */
LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

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

/*
 * BT_LE_ADV_OPT_USE_IDENTITY is not optional here, and leaving it off the fast
 * parameters meant this watch never advertised at all.
 *
 * bt_ready() starts the scanner before advertising, and the controller cannot
 * hold two different random addresses at once — so with a scan running, an
 * advertiser that wants its own private address is refused with -EACCES. The
 * slow parameters below always had the flag; the fast path used the stock
 * BT_LE_ADV_CONN, which does not. bt_le_adv_start() failed on every attempt,
 * and because start_adv() returns early on failure it never set adv_running
 * or scheduled the slow-advertising work either, so the parameter set that
 * would have worked was unreachable. Nothing on air, ever.
 *
 * The cost is that the watch advertises from its identity address rather than
 * a rotating one. That was already true for slow advertising, and the payload
 * carries a stable identity hash regardless, so there is no privacy left to
 * protect by using an RPA for the first thirty seconds.
 */
static const struct bt_le_adv_param adv_fast_param =
    BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_USE_IDENTITY,
                         BT_GAP_ADV_FAST_INT_MIN_2, /* 100 ms */
                         BT_GAP_ADV_FAST_INT_MAX_2, /* 150 ms */
                         NULL);

static const struct bt_le_adv_param adv_slow_param =
    BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_USE_IDENTITY,
                         BT_GAP_ADV_SLOW_INT_MIN,   /* 1000 ms */
                         BT_GAP_ADV_SLOW_INT_MAX,   /* 1280 ms */
                         NULL);

static void adv_slow_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(adv_slow_work, adv_slow_fn);

/* Tracks which param set is currently active, so adv_update_work_fn() below
 * can restart advertising in place without forcing a return to fast mode. */
static bool adv_is_slow;

static void start_adv(void)
{
    build_mfr_data();
    int rc = bt_le_adv_start(&adv_fast_param,
                             adv_data, ARRAY_SIZE(adv_data),
                             scan_rsp, ARRAY_SIZE(scan_rsp));
    if (rc) {
        LOG_ERR("adv_start failed: %d", rc);
        return;
    }
    atomic_set(&adv_running, 1);
    adv_is_slow = false;
    k_work_reschedule(&adv_slow_work, K_MSEC(ADV_FAST_DURATION_MS));

    /* Log the success, not just the failure. Silence on this path used to mean
     * either "advertising fine" or "never got here", which is the wrong place
     * to start from when a phone cannot see the watch. */
    LOG_INF("Advertising as '%s' (fast, %d s then slow)",
            CONFIG_BT_DEVICE_NAME, ADV_FAST_DURATION_MS / 1000);
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
        return;
    }
    adv_is_slow = true;

    /* Worth its own line: from here a scan can take seconds to turn the watch
     * up, which reads as "not advertising" if you do not know it happened. */
    LOG_INF("Advertising slowed to 1-1.28 s interval (still connectable)");
}

/* adv_update_work: rebuild mfr_data and restart advertising from workqueue
 * context, in whichever param set (fast or slow) is currently active.
 * Called via k_work_submit from parse_adv (BT scan callback) — never call
 * bt_le_adv_stop/start directly from the BT RX thread.
 *
 * Deliberately does NOT go through start_adv(): that always restarts in
 * fast mode and reschedules adv_slow_work a fresh ADV_FAST_DURATION_MS out.
 * This fires on every new unique encounter (ble_update_adv() -> here), so a
 * busy encounter event — several watches seen in quick succession — used to
 * pin advertising in the fast, higher-power interval indefinitely as long
 * as new encounters kept landing within any 30s window, silently defeating
 * the whole point of the fast->slow backoff this file otherwise implements
 * carefully (see the 10% scan duty cycle comment in bt_ready()). */
static void adv_update_work_fn(struct k_work *w)
{
    ARG_UNUSED(w);
    if (!atomic_get(&adv_running)) {
        return;
    }

    bt_le_adv_stop();
    build_mfr_data();

    /* Both sets carry USE_IDENTITY — see adv_fast_param. Restarting into
     * BT_LE_ADV_CONN here would fail the same way start_adv() did. */
    const struct bt_le_adv_param *param = adv_is_slow ? &adv_slow_param
                                                      : &adv_fast_param;
    int rc = bt_le_adv_start(param, adv_data, ARRAY_SIZE(adv_data),
                             scan_rsp, ARRAY_SIZE(scan_rsp));
    if (rc) {
        LOG_ERR("adv restart failed: %d", rc);
        atomic_set(&adv_running, 0);
    }
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

    LOG_INF("Phone connected — advertising stopped until disconnect");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    /* CONFIG_BT_MAX_CONN=1 makes conn always match phone_conn today, but
     * check anyway: unref'ing/clearing phone_conn without checking would be
     * a live bug (clearing a ref to a still-live connection) the moment
     * BT_MAX_CONN is ever raised. */
    if (phone_conn == conn) {
        bt_conn_unref(phone_conn);
        phone_conn = NULL;
    }
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
    /*
     * Antenna health readout — see CONFIG_EW_BLE_SCAN_DEBUG.
     *
     * Receive and transmit share the antenna and matching network, so this
     * cannot show that transmit works. What it can do is rule the antenna
     * out as the cause, which is otherwise pure guesswork. The strongest
     * RSSI is the number worth reading: a front end coupled to a working
     * antenna hears a nearby handset at around -50 dBm, and one that only
     * ever manages -90 with a phone alongside it is barely connected to
     * anything.
     *
     * Rate-limited to 3 s. Scan reports arrive far faster and would flood
     * RTT badly enough to stall the log backend.
     */
    if (IS_ENABLED(CONFIG_EW_BLE_SCAN_DEBUG)) {
        static int64_t last_log;
        static uint32_t seen;
        static int8_t   best = -128;

        seen++;
        if (info->rssi > best) {
            best = info->rssi;
        }

        if (k_uptime_get() - last_log >= 3000) {
            last_log = k_uptime_get();
            LOG_INF("scan: %u adverts heard, strongest %d dBm (latest %d dBm)",
                    seen, best, info->rssi);
        }
    }

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
 *   0004 = Time characteristic         (READ | WRITE)
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

#define BT_UUID_EW_TIME \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0xEE550004, 0x0000, 0x0000, 0x0000, 0x000000000000))

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

/*
 * Notifications are a UI page, not a direct display write.
 *
 * This used to paint LED_LAYER_NOTIFICATION straight from the BT RX thread
 * and then commit from a work item, which meant it could silently clobber
 * whatever battery.c had on that same layer — and battery.c would carry on
 * believing it still owned the screen. Now the category is stashed, the UI
 * is asked for the notification page, and the page draws it. ui_goto()
 * blanks first, so there is nothing left of the previous page to fight with.
 *
 * Still deferred to the workqueue: ui_goto() blanks, commits and may suspend
 * threads, none of which belongs on the BT RX thread — that starves the
 * controller and risks dropped connection events.
 */
static uint8_t pending_category;

static void notif_show_fn(struct k_work *w)
{
    ARG_UNUSED(w);
    ui_goto(UI_PAGE_NOTIFICATION);
}
static K_WORK_DEFINE(notif_show_work, notif_show_fn);

/* Called by the notification page once ui_goto() has blanked and is ready
 * for content. Runs on the workqueue, so plain mutex use is fine. */
void ble_paint_notification(void)
{
    uint8_t category = pending_category;

    if (category >= ARRAY_SIZE(notif_colors)) {
        category = 0;
    }

    k_mutex_lock(&led_mask_mutex, K_FOREVER);
    for (int col = 0; col < LED_COLS; col++) {
        led_mask[LED_LAYER_NOTIFICATION][0][col] = 1;
    }
    led_layer_color[LED_LAYER_NOTIFICATION] = notif_colors[category];
    k_mutex_unlock(&led_mask_mutex);

    led_commit();
}

static void show_notification(uint8_t category)
{
    pending_category = category;
    k_work_submit(&notif_show_work);
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
        LOG_INF("Notification cat=%u text len=%d", category, (int)(len - 1));
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

/*
 * Time characteristic — little-endian Unix epoch seconds, UTC.
 *
 * Accepts 4 or 8 bytes on write. Eight is the honest width and what a phone
 * should send; four is accepted because it is far less to type by hand in a
 * generic BLE tool, and unsigned it stays valid past this firmware's own year
 * 2100 ceiling. Reads always return eight.
 *
 * Readable as well as writable so a client can check what actually landed.
 * A write it cannot verify is a write it has to trust, and the RTC is exactly
 * the thing where a silently-wrong value goes unnoticed for a long time.
 *
 * Encrypted like the rest of the service: the watch is bondable, and mixing an
 * open characteristic into an otherwise encrypted service is the kind of
 * inconsistency that gets copied by the next characteristic.
 */
static ssize_t on_time_write(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(flags);

    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    int64_t epoch;

    if (len == sizeof(uint32_t)) {
        epoch = (int64_t)sys_get_le32(buf);
    } else if (len == sizeof(uint64_t)) {
        epoch = (int64_t)sys_get_le64(buf);
    } else {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    int rc = time_sync_set_epoch(epoch);

    if (rc == -EINVAL) {
        LOG_WRN("Time write rejected: epoch %lld out of range", epoch);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (rc) {
        LOG_ERR("Time write failed: %d", rc);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    LOG_INF("RTC set over BLE: epoch %lld", epoch);
    return len;
}

static ssize_t on_time_read(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset)
{
    int64_t epoch = time_sync_get_epoch();

    if (epoch < 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    uint8_t out[sizeof(uint64_t)];

    sys_put_le64((uint64_t)epoch, out);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, out, sizeof(out));
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
    BT_GATT_CHARACTERISTIC(BT_UUID_EW_TIME,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
        on_time_read, on_time_write, NULL),
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
