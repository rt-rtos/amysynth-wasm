/* Host shims for the host build: FreeRTOS queues/tasks, ESP ROM/partition. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

TaskHandle_t g_host_cur_task = (TaskHandle_t)0x2;   /* "main/UI" task */
int g_host_log = 2;

static int s_next_task = 0x100;
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                   void *arg, UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    (void)fn; (void)stack; (void)arg; (void)prio; (void)core;
    /* hostsim calls synth_ui_slice() on the main thread, which must then be
     * the task synth_ui_init() registers as the layers applier. */
    if (out) *out = strcmp(name, "seq_ui") == 0 ? g_host_cur_task
                                                : (TaskHandle_t)(intptr_t)(s_next_task++);
    return pdPASS;
}
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *out)
{ return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, 0); }

/* Unbounded FIFO: the device pump drains concurrently; here the main loop
 * drains at fixed points, so capacity must never be the limit. */
struct host_queue { size_t item, cap, head, count; uint8_t *buf; };
QueueHandle_t xQueueCreate(UBaseType_t depth, UBaseType_t item)
{
    struct host_queue *q = calloc(1, sizeof *q);
    q->item = item; q->cap = depth < 1024 ? 1024 : depth;
    q->buf = malloc(q->cap * item);
    return q;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t depth, UBaseType_t item, uint8_t *st, StaticQueue_t *s)
{ (void)st; (void)s; return xQueueCreate(depth, item); }
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t)
{
    (void)t;
    if (q->count == q->cap) {
        size_t ncap = q->cap * 2;
        uint8_t *nb = malloc(ncap * q->item);
        for (size_t i = 0; i < q->count; i++)
            memcpy(nb + i * q->item, q->buf + ((q->head + i) % q->cap) * q->item, q->item);
        free(q->buf); q->buf = nb; q->cap = ncap; q->head = 0;
    }
    memcpy(q->buf + ((q->head + q->count) % q->cap) * q->item, item, q->item);
    q->count++;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t)
{
    (void)t;
    if (q->count == 0) return pdFALSE;
    memcpy(item, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->cap; q->count--;
    return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { return (UBaseType_t)q->count; }

/* 'drums' partition = components/amy/drums.bin */
static uint8_t *s_drums; static size_t s_drums_len;
esp_partition_t g_host_drums_part = { "drums", 4u << 20 };
int host_drums_load(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); s_drums_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    s_drums = malloc(s_drums_len);
    size_t n = fread(s_drums, 1, s_drums_len, f); fclose(f);
    return n == s_drums_len;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n)
{
    (void)p;
    if (!s_drums || off + n > s_drums_len) { memset(dst, 0xFF, n); return off < s_drums_len ? ESP_OK : ESP_FAIL; }
    memcpy(dst, s_drums + off, n);
    return ESP_OK;
}

/* Same table-driven reflected CRC-32 as IDF's esp_rom/linux/esp_rom_crc.c. */
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    static uint32_t t[256]; static int init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        init = 1;
    }
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) crc = t[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void delay_ms(uint32_t ms) { (void)ms; }
