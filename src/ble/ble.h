#pragma once

void ble_init(void);

/* Called by identity module — restarts advertising with fresh payload */
void ble_update_adv(void);
