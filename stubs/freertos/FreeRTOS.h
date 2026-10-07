#pragma once
/* Host shim: single-threaded FreeRTOS surface for the host build. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portTICK_PERIOD_MS 1
#define configASSERT(x) assert(x)
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m) ((void)(m))
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define IRAM_ATTR
#define DRAM_ATTR
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef struct host_queue *QueueHandle_t;
typedef struct { void *p; } StaticQueue_t;
typedef void *SemaphoreHandle_t;
typedef struct { void *p; } StaticSemaphore_t;
typedef struct { void *p; } StaticTask_t;
typedef uint8_t StackType_t;
extern TaskHandle_t g_host_cur_task;
