#pragma once
#include <stdint.h>
#include <stdbool.h>

void     identity_init(void);
uint32_t identity_hash(void);
uint8_t  identity_dev_distance(void);
uint16_t identity_encounter_count(void);
bool     identity_is_dev(void);

/* Called by ble.c when a valid nearby watch advertisement is parsed. */
void     identity_on_encounter(uint32_t their_hash, uint8_t their_dev_dist);
