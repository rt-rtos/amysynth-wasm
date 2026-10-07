#pragma once
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *out);
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return g_host_cur_task; }
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline TickType_t xTaskGetTickCount(void) { return 0; }
static inline uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t) { (void)c; (void)t; return 0; }
static inline BaseType_t xTaskNotifyGive(TaskHandle_t h) { (void)h; return pdPASS; }
static inline void vTaskDelete(TaskHandle_t h) { (void)h; }
static inline int xPortGetCoreID(void) { return 0; }
