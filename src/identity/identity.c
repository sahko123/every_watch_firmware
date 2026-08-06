#include "identity.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <hal/nrf_ficr.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(identity, LOG_LEVEL_INF);

/* NVS key IDs */
#define NVS_KEY_DEV_DIST    1
#define NVS_KEY_ENC_COUNT   2
#define NVS_KEY_ENC_HASHES  3

#define MAX_SEEN_HASHES     32
#define DEV_DIST_NONE       0xFF   /* not connected to the dev chain */

/* Batch NVS hash writes: flush every N encounters or when the table is full.
 * NVS on nRF52 flash has ~10 000 erase cycles; batching reduces wear at busy
 * events where many unique devices are encountered rapidly. */
#define NVS_WRITE_BATCH     4

static uint32_t own_hash;
static uint8_t  dev_dist   = DEV_DIST_NONE;
static uint16_t enc_count;
static uint32_t seen_hashes[MAX_SEEN_HASHES];
static uint8_t  seen_count;
static uint8_t  unsaved_encounters;

static struct nvs_fs nvs;
static bool nvs_ready;

/* Guards dev_dist/enc_count/seen_hashes/seen_count/unsaved_encounters.
 * identity_on_encounter() (BT RX thread) mutates them; nvs_flush_work_fn()
 * (system workqueue) reads them to write to flash. Deferring the flash
 * write off the BT RX thread (so it can't block Bluetooth connection
 * events on a sector garbage-collect) introduced real cross-thread
 * concurrency here that didn't exist when everything ran synchronously on
 * one thread — a flush mid-flight reading seen_hashes[] while a new
 * encounter's add_seen() mutates the same array (including its ring-buffer
 * memmove once full) would read/write torn data. */
static K_MUTEX_DEFINE(identity_mutex);

/* FNV-1a 32-bit over two 32-bit words */
static uint32_t fnv1a_2x32(uint32_t a, uint32_t b)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4; i++) {
        h ^= (a >> (i * 8)) & 0xFF;
        h *= 16777619u;
    }
    for (int i = 0; i < 4; i++) {
        h ^= (b >> (i * 8)) & 0xFF;
        h *= 16777619u;
    }
    return h;
}

static bool seen_before(uint32_t hash)
{
    for (int i = 0; i < seen_count; i++) {
        if (seen_hashes[i] == hash) {
            return true;
        }
    }
    return false;
}

/* Flushes seen_hashes[]/dev_dist/enc_count to flash. Runs on the system
 * workqueue, never directly on the caller of identity_on_encounter() — that
 * caller is the Bluetooth scan callback (BT RX thread), and nvs_write() can
 * block for a full sector garbage-collect/erase (tens of ms), not just the
 * write itself. Blocking that thread risks dropped connection events, same
 * reasoning as ble.c's notif_commit_work for the LED path. */
static void nvs_flush_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!nvs_ready) {
        return;
    }

    /* Snapshot under the lock, then do the actual (slow) flash I/O
     * unlocked — nvs_write() blocking for a sector GC must not also block
     * identity_on_encounter() on the BT RX thread from updating RAM state
     * for the next encounter. */
    uint32_t hashes_copy[MAX_SEEN_HASHES];
    uint8_t  count_copy;
    uint8_t  dist_copy;
    uint16_t enc_copy;

    k_mutex_lock(&identity_mutex, K_FOREVER);
    memcpy(hashes_copy, seen_hashes, sizeof(hashes_copy));
    count_copy = seen_count;
    dist_copy  = dev_dist;
    enc_copy   = enc_count;
    k_mutex_unlock(&identity_mutex);

    int rc;

    rc = nvs_write(&nvs, NVS_KEY_ENC_HASHES,
                    hashes_copy, count_copy * sizeof(uint32_t));
    if (rc < 0) {
        LOG_ERR("NVS write hashes failed: %d", rc);
    }
    rc = nvs_write(&nvs, NVS_KEY_DEV_DIST, &dist_copy, sizeof(dist_copy));
    if (rc < 0) {
        LOG_ERR("NVS write dev_dist failed: %d", rc);
    }
    rc = nvs_write(&nvs, NVS_KEY_ENC_COUNT, &enc_copy, sizeof(enc_copy));
    if (rc < 0) {
        LOG_ERR("NVS write enc_count failed: %d", rc);
    }
}
static K_WORK_DEFINE(nvs_flush_work, nvs_flush_work_fn);

/* In-RAM only — no flash I/O here. The actual write is batched; see
 * identity_on_encounter(), which folds this together with dev_dist/enc_count
 * into one nvs_flush_work dispatch every NVS_WRITE_BATCH encounters. Counter
 * writes used to fire unbatched on every single encounter (save_counters()
 * called unconditionally), which defeated the batching this comment always
 * claimed applied to all of it — folded together now so it actually does. */
static void add_seen(uint32_t hash)
{
    if (seen_count < MAX_SEEN_HASHES) {
        seen_hashes[seen_count++] = hash;
    } else {
        /* Ring buffer: evict oldest entry */
        memmove(seen_hashes, seen_hashes + 1,
                (MAX_SEEN_HASHES - 1) * sizeof(uint32_t));
        seen_hashes[MAX_SEEN_HASHES - 1] = hash;
    }
}

void identity_init(void)
{
    /* Unique hash from FICR device address (burned in at factory) */
    uint32_t addr0 = NRF_FICR->DEVICEADDR[0];
    uint32_t addr1 = NRF_FICR->DEVICEADDR[1] & 0x0000FFFF;
    own_hash = fnv1a_2x32(addr0, addr1);

    /* Mount NVS on identity_partition (separate from storage_partition used by
     * Zephyr Settings / BT bonds — two nvs_fs on the same partition corrupt ATEs) */
    nvs.flash_device = FIXED_PARTITION_DEVICE(identity_partition);
    nvs.offset       = FIXED_PARTITION_OFFSET(identity_partition);
    nvs.sector_size  = 0x1000;  /* 4 KB — nRF52833 flash erase unit */
    nvs.sector_count = 2;

    int rc = nvs_mount(&nvs);
    if (rc) {
        LOG_WRN("NVS mount failed (%d) — starting fresh", rc);
        return;
    }
    nvs_ready = true;

    /* Load persisted state */
    uint8_t saved_dist;
    if (nvs_read(&nvs, NVS_KEY_DEV_DIST, &saved_dist, 1) > 0) {
        dev_dist = saved_dist;
    }

    /*
     * -ENOENT here is not a failure: it is what NVS returns for a key that has
     * never been written, which is the normal state of a watch that has not yet
     * had a BLE encounter (or has just been mass-erased over SWD). This warned
     * unconditionally and so cried wolf on every boot of a fresh device, while
     * the dev_dist read three lines up treats the identical situation as
     * unremarkable — the inconsistency was the bug, not the read.
     *
     * A genuine read error is still worth hearing about, so only that case
     * warns now.
     */
    rc = nvs_read(&nvs, NVS_KEY_ENC_COUNT, &enc_count, sizeof(enc_count));
    if (rc < 0) {
        enc_count = 0;
        if (rc != -ENOENT) {
            LOG_WRN("enc_count read failed (%d) — starting from 0", rc);
        }
    }

    ssize_t n = nvs_read(&nvs, NVS_KEY_ENC_HASHES,
                         seen_hashes, sizeof(seen_hashes));
    if (n > 0) {
        uint32_t loaded = (uint32_t)(n / sizeof(uint32_t));
        seen_count = (uint8_t)MIN(loaded, (uint32_t)MAX_SEEN_HASHES);
    }

    LOG_INF("Identity: hash=0x%08X dist=%u encounters=%u",
            own_hash, dev_dist, enc_count);
}

void identity_on_encounter(uint32_t their_hash, uint8_t their_dev_dist)
{
    if (their_hash == own_hash) {
        return;  /* ignore own reflection */
    }

    /* Whole read-modify-write as one critical section, not just the
     * individual field updates: seen_before()'s check and add_seen()'s
     * mutation must happen atomically together, or two rapid calls for
     * the same hash could both pass the check before either adds it. */
    k_mutex_lock(&identity_mutex, K_FOREVER);

    if (seen_before(their_hash)) {
        k_mutex_unlock(&identity_mutex);
        return;
    }

    add_seen(their_hash);
    enc_count++;

    /* Propagate the dev chain — adopt the shorter path */
    if (their_dev_dist != DEV_DIST_NONE) {
        uint8_t candidate = their_dev_dist + 1;
        if (candidate < dev_dist) {
            dev_dist = candidate;
            LOG_INF("Dev chain updated: distance=%u via 0x%08X",
                    dev_dist, their_hash);
        }
    }

    bool flush = false;

    unsaved_encounters++;
    if (nvs_ready && unsaved_encounters >= NVS_WRITE_BATCH) {
        unsaved_encounters = 0;
        flush = true;
    }

    k_mutex_unlock(&identity_mutex);

    /* Outside the lock: k_work_submit() is fine to call unlocked, and
     * nvs_flush_work_fn() takes its own snapshot under the same lock. */
    if (flush) {
        k_work_submit(&nvs_flush_work);
    }

    LOG_INF("Encounter #%u: hash=0x%08X their_dist=%u",
            enc_count, their_hash, their_dev_dist);
}

uint32_t identity_hash(void) { return own_hash; }  /* set once at boot, never mutated after */

uint8_t identity_dev_distance(void)
{
    k_mutex_lock(&identity_mutex, K_FOREVER);
    uint8_t d = dev_dist;
    k_mutex_unlock(&identity_mutex);
    return d;
}

uint16_t identity_encounter_count(void)
{
    k_mutex_lock(&identity_mutex, K_FOREVER);
    uint16_t c = enc_count;
    k_mutex_unlock(&identity_mutex);
    return c;
}

bool identity_is_dev(void) { return identity_dev_distance() == 0; }
