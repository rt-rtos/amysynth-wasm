#pragma once
/* Host shim: time is audio time, advanced by hostsim's step loop. */
#include <stdint.h>
int64_t esp_timer_get_time(void);
