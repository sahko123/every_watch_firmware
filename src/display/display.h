#pragma once

#include <stdbool.h>

#define DISPLAY_TIMEOUT_S 10

void display_init(void);
void display_on(void);
void display_off(void);
bool display_is_on(void);
void display_reset_timeout(void);
