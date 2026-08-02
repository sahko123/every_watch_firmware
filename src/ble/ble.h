#pragma once

void ble_init(void);

/* Called by identity module — restarts advertising with fresh payload */
void ble_update_adv(void);

/* Paint the pending notification into the notification layer and commit.
 * Called by the UI's notification page after it has blanked — the category
 * colour is only known here. Workqueue context. */
void ble_paint_notification(void);
