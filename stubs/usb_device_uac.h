#pragma once
/* Host shim: no UAC device on the host. */
#include <stdint.h>
#include "esp_err.h"
static inline esp_err_t uac_device_get_pull_stats(uint32_t c[4], int64_t *t_us)
{ (void)c; (void)t_us; return ESP_FAIL; }
