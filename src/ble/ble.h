#pragma once
#include <stdbool.h>

void ble_init(void);
bool ble_is_connected(void);  /* true if a phone is currently connected */

/* Called by identity module — restarts advertising with fresh payload */
void ble_update_adv(void);
