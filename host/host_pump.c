/* The ingest pump, driven synchronously: the device runs amy_ingest_task on
 * Core 0; here the main loop calls host_pump_drain() at fixed points. Includes
 * the real amy_helpers.c so the drain uses its private queue and urgent hook. */
#include "amy_helpers.c"   /* synth_core dir is on the include path */
void host_pump_drain(void)
{
    TaskHandle_t saved = g_host_cur_task;
    g_host_cur_task = s_pump_task;
    amy_event ev;
    for (;;) {
        if (s_urgent_drain != NULL && s_urgent_drain()) continue;
        if (s_ingest_queue && xQueueReceive(s_ingest_queue, &ev, 0) == pdTRUE) {
            amy_add_event(&ev);
            continue;
        }
        break;
    }
    g_host_cur_task = saved;
}
