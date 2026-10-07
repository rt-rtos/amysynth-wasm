#pragma once
#include "freertos/FreeRTOS.h"
QueueHandle_t xQueueCreate(UBaseType_t depth, UBaseType_t item);
QueueHandle_t xQueueCreateStatic(UBaseType_t depth, UBaseType_t item, uint8_t *storage, StaticQueue_t *q);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
