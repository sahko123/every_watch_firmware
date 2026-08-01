#pragma once

void light_init(void);

/* Trigger an immediate ambient-light sample. ISR-safe. Must be called while
 * the display is still off, before whatever's about to turn it on — the
 * sensor reading is meaningless once the LEDs themselves are lit. */
void light_sample_now(void);
