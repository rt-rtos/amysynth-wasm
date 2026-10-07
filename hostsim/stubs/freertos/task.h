#pragma once
/* Host shim for hostsim: the task surface of host/stubs, with the tick count
 * taken from audio time (1 tick = 1 ms, portTICK_PERIOD_MS 1). */
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *out);
TickType_t xTaskGetTickCount(void);
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return g_host_cur_task; }
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline void vTaskDelayUntil(TickType_t *prev, TickType_t inc) { *prev += inc; }
static inline uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t) { (void)c; (void)t; return 0; }
static inline BaseType_t xTaskNotifyGive(TaskHandle_t h) { (void)h; return pdPASS; }
static inline void vTaskDelete(TaskHandle_t h) { (void)h; }
static inline int xPortGetCoreID(void) { return 0; }
