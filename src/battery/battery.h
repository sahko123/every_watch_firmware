#pragma once
#include <stdint.h>
#include <stdbool.h>

void     battery_init(void);
uint8_t  battery_percent(void);    /* 0-100 */
uint32_t battery_voltage_mv(void); /* millivolts */
bool     battery_charging(void);   /* avg current > 0 */
bool     battery_is_low(void);     /* true when SoC < LOW_BATTERY_THRESHOLD */
