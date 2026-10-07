#pragma once
#include <stdio.h>
extern int g_host_log;
#define ESP_LOGE(tag, fmt, ...) do { if (g_host_log >= 1) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { if (g_host_log >= 2) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { if (g_host_log >= 3) fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { if (g_host_log >= 4) fprintf(stderr, "D %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGV(tag, fmt, ...) do { } while (0)
#define ESP_EARLY_LOGI ESP_LOGI
