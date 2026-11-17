/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_flow — the loop. See espos_flow.h for the contract; espos_sched.c is
 * the timer arithmetic, and everything typed is in the espos_flow/ headers.
 *
 * The loop is deliberately dull:
 *
 *     for (;;) {
 *         fire whatever timers are due
 *         drain the mailbox
 *         sleep until the earlier of (next deadline, something posted)
 *     }
 *
 * Sleeping is a blocking read on the mailbox queue with the timeout the
 * scheduler asks for, so a post wakes the loop immediately and a quiet
 * firmware costs nothing between timers. That is the whole design: one
 * queue serving as both work channel and wake source.
 *
 * The mutex protects the timer table only. The queue is FreeRTOS's and needs
 * none, which is what makes espos_flow_post_from_isr() possible at all — an
 * ISR can never take a mutex.
 */
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "espos_flow.h"
#include "espos_sched.h"

#include "flow_port.h"

/* Reporting is wanted (Kconfig) AND possible (the component is in the build,
 * which CMakeLists decides). Either one missing and espos_flow still builds;
 * the counters in espos_flow_stats() are then the only signal. */
#if defined(ESPOS_FLOW_HAVE_HEALTH) && CONFIG_ESPOS_FLOW_HEALTH
#define FLOW_REPORT_HEALTH 1
#include "espos_health.h"
#else
#define FLOW_REPORT_HEALTH 0
#endif

static const char *TAG = "espos_flow";

#define MAX_TIMERS    CONFIG_ESPOS_FLOW_MAX_TIMERS
#define MAILBOX_DEPTH CONFIG_ESPOS_FLOW_MAILBOX_DEPTH

typedef struct {
    espos_flow_cb_t cb;
    void *arg;
} msg_t;

static struct {
    espos_sched_t sched;
    espos_sched_timer_t slots[MAX_TIMERS];
    SemaphoreHandle_t lock;
    QueueHandle_t mailbox;
    TaskHandle_t task;
    TaskHandle_t owner;    /* the task allowed to emit(); the loop, or the one running run_until_idle() */
    volatile bool running; /* the loop task should keep going */
    volatile bool stopping;
    int64_t epoch_us; /* the port clock reading when the loop epoch was zeroed */
    /* Counters. Written on the loop task and by posters; read by _stats().
     * A torn read of a statistic is not worth a lock on the ISR path. */
    volatile uint32_t posts;
    volatile uint32_t dropped;
    volatile uint32_t timers_fired;
    volatile uint32_t queue_peak;
    volatile uint32_t edges_used;
    bool mailbox_warned; /* flowMailbox raised once, not once per drop */
} s;

/* Created before app_main so espos_flow_every() works from a constructor —
 * a C++ node that arms its own poll while the static initialisers run, which
 * is exactly what value-semantics node ownership is for. Same reason
 * espos_health does it: a lazily created mutex needs an unsynchronised read
 * to decide whether to create it, and that is the bug you find in a year. */
static void __attribute__((constructor)) flow_ctor(void)
{
    s.lock = xSemaphoreCreateMutex();
    espos_sched_init(&s.sched, s.slots, MAX_TIMERS);
    s.epoch_us = espos_flow_port_now_us();
}

static bool lock(void)
{
    if (!s.lock) return false;
    return xSemaphoreTake(s.lock, portMAX_DELAY) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s.lock);
}

uint32_t espos_flow_now_ms(void)
{
    /* Truncated to 32 bits on purpose: the whole scheduler is modular, and a
     * 64-bit division per loop pass on a C3 is not free. */
    return (uint32_t)((espos_flow_port_now_us() - s.epoch_us) / 1000);
}

bool espos_flow_on_loop_task(void)
{
    TaskHandle_t owner = s.owner;
    return owner != NULL && owner == xTaskGetCurrentTaskHandle();
}

/* ------------------------------------------------------------------ timers */

esp_err_t espos_flow_every(uint32_t period_ms, espos_flow_cb_t cb, void *arg, espos_flow_timer_t *out)
{
    if (!cb || period_ms == 0) return ESP_ERR_INVALID_ARG;
    espos_sched_handle_t h = ESPOS_SCHED_HANDLE_NONE;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = espos_sched_add(&s.sched, espos_flow_now_ms(), period_ms, period_ms, cb, arg, &h);
    unlock();
    if (out) *out = (espos_flow_timer_t)h;
    if (err == ESP_ERR_NO_MEM) {
        ESP_LOGE(TAG, "no timer slot for a %" PRIu32 " ms period (CONFIG_ESPOS_FLOW_MAX_TIMERS=%d)", period_ms,
                 MAX_TIMERS);
    }
    return err;
}

esp_err_t espos_flow_after(uint32_t delay_ms, espos_flow_cb_t cb, void *arg, espos_flow_timer_t *out)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    espos_sched_handle_t h = ESPOS_SCHED_HANDLE_NONE;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = espos_sched_add(&s.sched, espos_flow_now_ms(), delay_ms, 0, cb, arg, &h);
    unlock();
    if (out) *out = (espos_flow_timer_t)h;
    if (err == ESP_ERR_NO_MEM) {
        ESP_LOGE(TAG, "no timer slot for a %" PRIu32 " ms delay (CONFIG_ESPOS_FLOW_MAX_TIMERS=%d)", delay_ms,
                 MAX_TIMERS);
    }
    return err;
}

esp_err_t espos_flow_cancel(espos_flow_timer_t h)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = espos_sched_cancel(&s.sched, (espos_sched_handle_t)h);
    unlock();
    return err;
}

/* ----------------------------------------------------------------- mailbox */

static void mailbox_full(void)
{
    s.dropped++;
    if (!s.mailbox_warned) {
        s.mailbox_warned = true;
#if FLOW_REPORT_HEALTH
        /* WARN, never fatal: a full mailbox is back-pressure, and restarting
         * the device would lose more work than the drop did. */
        espos_health_report("flowMailbox", ESPOS_HEALTH_WARN, "flow mailbox full: work dropped");
#endif
    }
}

esp_err_t espos_flow_post(espos_flow_cb_t cb, void *arg)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    if (!s.mailbox) return ESP_ERR_INVALID_STATE;

    msg_t m = { .cb = cb, .arg = arg };
    /* Zero ticks: posting never blocks, from any task. A producer faster than
     * the loop must be allowed to drop, not to stall — blocking here would
     * push the stall into whatever called it, which may be a driver task. */
    if (xQueueSend(s.mailbox, &m, 0) != pdTRUE) {
        mailbox_full();
        return ESP_ERR_NO_MEM;
    }
    s.posts++;
    uint32_t waiting = (uint32_t)uxQueueMessagesWaiting(s.mailbox);
    if (waiting > s.queue_peak) s.queue_peak = waiting;
    return ESP_OK;
}

esp_err_t espos_flow_post_from_isr(espos_flow_cb_t cb, void *arg, bool *hp_task_woken)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    if (!s.mailbox) return ESP_ERR_INVALID_STATE;

    msg_t m = { .cb = cb, .arg = arg };
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s.mailbox, &m, &woken) != pdTRUE) {
        /* No health report from an ISR — espos_health takes a mutex. The
         * counter is enough; the loop raises the condition when it next
         * notices dropped moved. */
        s.dropped++;
        if (hp_task_woken) *hp_task_woken = false;
        return ESP_ERR_NO_MEM;
    }
    s.posts++;
    if (hp_task_woken) *hp_task_woken = (woken == pdTRUE);
    return ESP_OK;
}

/* Run everything waiting in the mailbox. Returns how many ran. Bounded by the
 * depth of the queue when it starts, so a callback that posts more work does
 * not keep the loop out of its timers. */
static size_t drain_mailbox(void)
{
    size_t budget = MAILBOX_DEPTH;
    size_t n = 0;
    msg_t m;
    while (n < budget && xQueueReceive(s.mailbox, &m, 0) == pdTRUE) {
        /* A NULL callback is the wake-up espos_flow_stop() sends; it exists
         * only to break the loop out of its queue wait. */
        if (m.cb) m.cb(m.arg);
        n++;
    }
    return n;
}

/* One pass: timers, then posted work. Returns true if either did anything. */
static bool run_once(void)
{
    if (!lock()) return false;
    size_t fired = espos_sched_fire(&s.sched, espos_flow_now_ms());
    unlock();
    s.timers_fired += (uint32_t)fired;

    size_t drained = drain_mailbox();
    return fired > 0 || drained > 0;
}

/* ------------------------------------------------------------------- loop */

static void flow_task(void *ignored)
{
    (void)ignored;

    /* Claim ownership before the first run_once(). xTaskCreate() starts the
     * new task immediately and it can preempt the caller before the caller's
     * post-create `s.owner = s.task` line runs. A timer due at t=0 then emits
     * through PulseCounter/Linear and trips the task assertion even though it
     * is the flow task. */
    s.owner = xTaskGetCurrentTaskHandle();

    ESP_LOGI(TAG, "flow loop up (stack %d, prio %d, mailbox %d, timers %d)", CONFIG_ESPOS_FLOW_TASK_STACK,
             CONFIG_ESPOS_FLOW_TASK_PRIO, MAILBOX_DEPTH, MAX_TIMERS);

    while (s.running) {
        run_once();

        if (!lock()) break;
        uint32_t wait_ms = espos_sched_next_due(&s.sched, espos_flow_now_ms());
        unlock();

        /* Cap the sleep so `running` going false is noticed within a tick or
         * two even with no timer pending, and so a timer added by another
         * task while the loop sleeps waits at most this long. */
        if (wait_ms > 100) wait_ms = 100;

        if (wait_ms > 0) {
            /* Peek-and-run rather than receive-and-discard: whatever wakes us
             * is real work, and the next pass will drain the rest. */
            msg_t m;
            if (xQueueReceive(s.mailbox, &m, pdMS_TO_TICKS(wait_ms)) == pdTRUE && m.cb) {
                m.cb(m.arg);
            }
        }
    }

    ESP_LOGI(TAG, "flow loop down");
    s.task = NULL;
    s.stopping = false;
    vTaskDelete(NULL);
}

esp_err_t espos_flow_start(void)
{
    if (s.running) return ESP_OK;
    if (!s.lock) return ESP_ERR_INVALID_STATE;

    if (!s.mailbox) {
        s.mailbox = xQueueCreate(MAILBOX_DEPTH, sizeof(msg_t));
        if (!s.mailbox) return ESP_ERR_NO_MEM;
    }

    /* An espos_flow_adopt_loop() borrow is only valid until the real loop
     * exists. Revoke it before xTaskCreate() so the adopter cannot emit
     * alongside the new task between its creation and its first pass; the
     * flow task claims ownership itself before run_once(). */
    TaskHandle_t borrowed_owner = s.owner;
    s.owner = NULL;

    s.running = true;
    if (xTaskCreate(flow_task, "espos_flow", CONFIG_ESPOS_FLOW_TASK_STACK, NULL, CONFIG_ESPOS_FLOW_TASK_PRIO,
                    &s.task) != pdPASS) {
        s.running = false;
        s.owner = borrowed_owner; /* start failed: the borrow is still valid */
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t espos_flow_stop(void)
{
    if (!s.running) return ESP_OK;
    if (s.task && s.task == xTaskGetCurrentTaskHandle()) return ESP_ERR_INVALID_STATE;

    s.stopping = true;
    s.running = false;
    /* Wake the loop out of its queue wait so it notices within a pass rather
     * than at the end of its 100 ms cap. */
    msg_t nop = { .cb = NULL, .arg = NULL };
    (void)xQueueSend(s.mailbox, &nop, 0);

    for (int i = 0; i < 200 && s.task; i++) vTaskDelay(pdMS_TO_TICKS(5));

    s.owner = NULL;
    /* Drop what is left rather than run it: stop means stop. */
    if (s.mailbox) {
        msg_t m;
        while (xQueueReceive(s.mailbox, &m, 0) == pdTRUE) s.dropped++;
    }
    return s.task ? ESP_ERR_TIMEOUT : ESP_OK;
}

bool espos_flow_is_running(void)
{
    return s.running;
}

esp_err_t espos_flow_adopt_loop(void)
{
    /* Refused once the real loop exists: the whole point of the task check is
     * that exactly one task emits, and a borrow taken alongside a running
     * loop would be two. */
    if (s.running) return ESP_ERR_INVALID_STATE;
    s.owner = xTaskGetCurrentTaskHandle();
    return ESP_OK;
}

void espos_flow_release_loop(void)
{
    if (!s.running) s.owner = NULL;
}

bool espos_flow_run_until_idle(uint32_t timeout_ms)
{
    if (s.running) return false;
    if (!s.mailbox) {
        s.mailbox = xQueueCreate(MAILBOX_DEPTH, sizeof(msg_t));
        if (!s.mailbox) return false;
    }

    /* Borrow ownership so emit() checks pass for the duration: the caller IS
     * the loop while this runs. */
    TaskHandle_t prev = s.owner;
    s.owner = xTaskGetCurrentTaskHandle();

    uint32_t started = espos_flow_now_ms();
    bool idle = false;
    for (;;) {
        if (!run_once()) {
            /* Nothing ran. Idle means nothing is due either — a timer with a
             * future deadline is pending, not busy. */
            if (!lock()) break;
            uint32_t next = espos_sched_next_due(&s.sched, espos_flow_now_ms());
            unlock();
            if (next != 0) {
                idle = true;
                break;
            }
        }
        if (espos_flow_now_ms() - started >= timeout_ms) break;
    }

    s.owner = prev;
    return idle;
}

/* -------------------------------------------------------------- inspection */

void espos_flow_stats(espos_flow_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->posts = s.posts;
    out->dropped = s.dropped;
    out->timers_fired = s.timers_fired;
    out->queue_peak = s.queue_peak;
    out->edges_used = s.edges_used;
    if (lock()) {
        out->timers_live = (uint32_t)espos_sched_count(&s.sched);
        unlock();
    }
}

void espos_flow_note_edges_used(uint32_t n)
{
    s.edges_used = n;
}
