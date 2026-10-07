#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { const char *label; uint32_t size; } esp_partition_t;
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n);
