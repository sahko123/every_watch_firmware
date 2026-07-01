#include "identity.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_ficr.h>
#include <string.h>

LOG_MODULE_REGISTER(identity, LOG_LEVEL_INF);

/* NVS key IDs */
#define NVS_KEY_DEV_DIST    1
#define NVS_KEY_ENC_COUNT   2
#define NVS_KEY_ENC_HASHES  3

#define MAX_SEEN_HASHES     32
#define DEV_DIST_NONE       0xFF   /* not connected to the dev chain */
#define RSSI_CLOSE_DBM      (-70)  /* minimum RSSI to count as an encounter */

static uint32_t own_hash;
static uint8_t  dev_dist   = DEV_DIST_NONE;
static uint16_t enc_count;
static uint32_t seen_hashes[MAX_SEEN_HASHES];
static uint8_t  seen_count;

static struct nvs_fs nvs;
static bool nvs_ready;

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
    if (nvs_ready) {
        nvs_write(&nvs, NVS_KEY_ENC_HASHES,
                  seen_hashes, seen_count * sizeof(uint32_t));
    }
}

static void save_counters(void)
{
    if (!nvs_ready) {
        return;
    }
    nvs_write(&nvs, NVS_KEY_DEV_DIST,  &dev_dist,  sizeof(dev_dist));
    nvs_write(&nvs, NVS_KEY_ENC_COUNT, &enc_count, sizeof(enc_count));
}

void identity_init(void)
{
    /* Unique hash from FICR device address (burned in at factory) */
    uint32_t addr0 = NRF_FICR->DEVICEADDR[0];
    uint32_t addr1 = NRF_FICR->DEVICEADDR[1] & 0x0000FFFF;
    own_hash = fnv1a_2x32(addr0, addr1);

    /* Mount NVS on the storage_partition */
    nvs.flash_device = FIXED_PARTITION_DEVICE(storage_partition);
    nvs.offset       = FIXED_PARTITION_OFFSET(storage_partition);
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

    nvs_read(&nvs, NVS_KEY_ENC_COUNT, &enc_count, sizeof(enc_count));

    ssize_t n = nvs_read(&nvs, NVS_KEY_ENC_HASHES,
                         seen_hashes, sizeof(seen_hashes));
    if (n > 0) {
        seen_count = (uint8_t)(n / sizeof(uint32_t));
    }

    LOG_INF("Identity: hash=0x%08X dist=%u encounters=%u",
            own_hash, dev_dist, enc_count);
}

void identity_on_encounter(uint32_t their_hash, uint8_t their_dev_dist)
{
    if (their_hash == own_hash) {
        return;  /* ignore own reflection */
    }
    if (seen_before(their_hash)) {
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

    save_counters();
    LOG_INF("Encounter #%u: hash=0x%08X their_dist=%u",
            enc_count, their_hash, their_dev_dist);
}

uint32_t identity_hash(void)          { return own_hash; }
uint8_t  identity_dev_distance(void)  { return dev_dist; }
uint16_t identity_encounter_count(void) { return enc_count; }
bool     identity_is_dev(void)        { return dev_dist == 0; }
