/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_flow — one task, one clock, one mailbox: the runtime a data-flow graph
 * runs on.
 *
 * This is the C half. It owns a single FreeRTOS task (the *flow loop*) that
 * does exactly three things forever: fire whatever timers are due, drain
 * whatever other tasks have posted, and sleep until the next of those. On top
 * of it sits the typed graph in the espos_flow/ headers — producers, transforms and
 * sinks wired with connect_to(). Everything the graph does happens on this
 * task. Nothing else needs to exist for the C half to be useful: a firmware
 * that only wants "call this every 500 ms, and let me hand work to that task
 * from an ISR" can use espos_flow.h alone and never include a header.
 *
 * ── The threading contract ────────────────────────────────────────────────
 *
 * espOS today documents a different callback context per component: the
 * writer's task for a config change, the stream task for a SignalK update,
 * the esp_timer task for a health tick, the event loop task for ESPOS_EVENT
 * (docs/concepts.md, "Which task calls you back"). Every one of them is a
 * separate set of rules to remember and a separate chance to touch a variable
 * from two tasks at once.
 *
 * espos_flow replaces all of that with ONE rule:
 *
 *     Everything in a graph runs on the flow task. Every value that enters a
 *     graph from anywhere else enters through a Mailbox.
 *
 * So: a node's transform, a Poll's read function, a Sink's write, an emit()
 * anywhere — all on the flow task, one at a time, never concurrently. No node
 * needs a lock, because no node is ever re-entered. In exchange, nothing on
 * that task may block: a callback that sleeps stops every timer in the
 * firmware. Work that must block belongs on its own task, which posts its
 * result back with espos_flow_post() (or Mailbox<T>::post()).
 *
 * An ISR, a driver callback, another component's callback on its own task:
 * all of them use espos_flow_post_from_isr() / espos_flow_post(), which is
 * the only supported way in. CONFIG_ESPOS_FLOW_CHECK_TASK (on by default in
 * debug builds) turns a violation into an assert with the offending task's
 * name rather than a corruption that shows up a week later.
 *
 * ── Cost when unused ──────────────────────────────────────────────────────
 *
 * The loop task is started by espos_flow_start(), which nothing calls for you.
 * A firmware that does not REQUIRES espos_flow links nothing; one that does
 * but never calls start pays the code size and no RAM beyond the static
 * tables. The C++ graph starts the loop when a Graph is run, not when a node
 * is constructed.
 *
 * Threading of the API itself: espos_flow_post(), _post_from_isr(),
 * _now_ms() and _stats() are safe from any task. _every/_after/_cancel are
 * safe from any task as well (they take the loop's lock), though the natural
 * place to call them is the flow task itself. _start/_stop are for the
 * application's start-up task, not for a callback.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A timer's identity. 0 is never a live timer, so a struct member zeroed at
 * start-up reads as "nothing scheduled". */
typedef uint32_t espos_flow_timer_t;

#define ESPOS_FLOW_TIMER_NONE ((espos_flow_timer_t)0)

/* Runs on the flow task. Must not block. */
typedef void (*espos_flow_cb_t)(void *arg);

/**
 * Start the flow loop: creates the task (CONFIG_ESPOS_FLOW_TASK_STACK,
 * CONFIG_ESPOS_FLOW_TASK_PRIO) and its mailbox. Idempotent — a second call
 * with the loop already running returns ESP_OK, so a library and its
 * application may both call it without arranging who goes first.
 *
 * Timers may be added before this: they are held and start counting from the
 * moment the loop starts, which is what lets a device struct arm its poll in
 * its own constructor, long before app_main() decides to run the graph.
 *
 * @return ESP_ERR_NO_MEM when the task or the queue cannot be created.
 */
esp_err_t espos_flow_start(void);

/**
 * Stop the loop and delete its task. Anything still in the mailbox is dropped
 * (counted in `dropped`); timers stay armed, so a later espos_flow_start() or
 * espos_flow_run_until_idle() fires whatever is due. To keep work that was
 * already posted, post a marker and wait for it to run before stopping: the
 * mailbox is first in, first out. Mostly for tests and for a device going
 * into deep sleep; a normal firmware starts the loop and leaves it running.
 *
 * Must not be called from the flow task itself (ESP_ERR_INVALID_STATE): a
 * task cannot wait for its own exit.
 */
esp_err_t espos_flow_stop(void);

/** True between a successful start and a stop. */
bool espos_flow_is_running(void);

/**
 * Milliseconds since the loop's epoch — a monotonic counter that never jumps
 * when the wall clock is learned. This is NOT espos_time: espos_time answers
 * "what time is it" and reads 0 until something syncs it; this answers "how
 * long since" and is always usable. A node that needs to stamp a value for
 * SignalK uses espos_time; a node that needs to know whether two inputs are
 * within 200 ms of each other uses this.
 *
 * uint32_t, so it wraps every 49.7 days. Every comparison in espos_flow is
 * modular (see espos_sched.h) and yours must be too: subtract, never compare.
 */
uint32_t espos_flow_now_ms(void);

/**
 * Call `cb(arg)` every `period_ms`, starting one period from now.
 *
 * The deadline is advanced from the deadline just met, so the period does not
 * drift with the callback's own duration; periods missed because the loop was
 * busy are skipped, not fired back to back.
 *
 * @return ESP_ERR_NO_MEM when CONFIG_ESPOS_FLOW_MAX_TIMERS is exhausted,
 *         ESP_ERR_INVALID_ARG for a NULL callback, a period of 0 or a period
 *         over 24.8 days.
 */
esp_err_t espos_flow_every(uint32_t period_ms, espos_flow_cb_t cb, void *arg, espos_flow_timer_t *out);

/**
 * Call `cb(arg)` once, `delay_ms` from now. `delay_ms` 0 runs it on the loop's
 * next pass, which is the cheapest way to move work off the current task onto
 * the flow task with a delay of "as soon as convenient".
 *
 * The handle is spent once the callback has run; cancelling it afterwards is
 * ESP_ERR_NOT_FOUND, never a cancellation of some unrelated later timer.
 */
esp_err_t espos_flow_after(uint32_t delay_ms, espos_flow_cb_t cb, void *arg, espos_flow_timer_t *out);

/**
 * Cancel a timer. Safe from inside the callback of the timer being cancelled.
 * ESP_ERR_NOT_FOUND when the handle is stale (already fired one-shot,
 * already cancelled, never existed) — which is the answer, not a failure.
 */
esp_err_t espos_flow_cancel(espos_flow_timer_t h);

/**
 * Run `cb(arg)` on the flow task, from any task. This is the seam every
 * external value crosses: a driver callback, a task that just finished a
 * blocking read, another component's subscription callback.
 *
 * Ordering is FIFO: two posts from the same task run in that order. Posts
 * from different tasks interleave however the scheduler decided, which is the
 * only nondeterminism in the whole model.
 *
 * Never blocks. When the mailbox is full the post is dropped, the drop is
 * counted, and health condition "flowMailbox" is raised WARN once — a full
 * mailbox means the loop is behind or a producer is too fast, and neither is
 * fixed by blocking the producer (that would just push the stall upstream,
 * possibly into an ISR).
 *
 * @return ESP_ERR_NO_MEM when the mailbox is full, ESP_ERR_INVALID_STATE when
 *         the loop is not running, ESP_ERR_INVALID_ARG for a NULL callback.
 */
esp_err_t espos_flow_post(espos_flow_cb_t cb, void *arg);

/**
 * espos_flow_post() from an interrupt. Same contract, ISR-safe primitives.
 *
 * `hp_task_woken` may be NULL; when it is not, it is set to true if the post
 * woke a task of higher priority than the interrupted one, and the ISR should
 * then yield (portYIELD_FROM_ISR). The callback still runs on the flow task —
 * an ISR posts the work, it does not do it.
 */
esp_err_t espos_flow_post_from_isr(espos_flow_cb_t cb, void *arg, bool *hp_task_woken);

/**
 * Run timers and mailbox work until neither has anything left to do, or until
 * `timeout_ms` has passed; return true if the loop actually reached idle.
 *
 * Two uses. A test drives a graph deterministically: post, run until idle,
 * assert. And a device about to enter deep sleep drains what is pending
 * before the clock stops, so a queued publish is not lost to the nap.
 *
 * Callable only when the loop is NOT running (ESP_ERR_INVALID_STATE
 * otherwise, reported as false): it does the loop's job, so the two would
 * fight over the same tables. A firmware that wants both starts the loop
 * after start-up and stops it before sleeping.
 */
bool espos_flow_run_until_idle(uint32_t timeout_ms);

/* ------------------------------------------------------------- inspection */

typedef struct {
    uint32_t posts;        /* accepted espos_flow_post/_from_isr calls */
    uint32_t dropped;      /* posts refused because the mailbox was full */
    uint32_t timers_fired; /* timer callbacks run since start */
    uint32_t timers_live;  /* timers currently scheduled */
    uint32_t queue_peak;   /* deepest the mailbox has been */
    uint32_t edges_used;   /* graph edges taken from the static pool */
} espos_flow_stats_t;

/**
 * Snapshot the counters. Safe from any task. Cheap enough to put behind a
 * REST endpoint or publish as a SignalK path; `dropped` growing is the one
 * number that means something is wrong.
 */
void espos_flow_stats(espos_flow_stats_t *out);

/**
 * True when called on the flow task. The C++ layer's emit() checks this under
 * CONFIG_ESPOS_FLOW_CHECK_TASK; an application writing its own node in C can
 * use it for the same assertion.
 */
bool espos_flow_on_loop_task(void);

/**
 * Adopt the calling task as the loop for as long as the loop is NOT running,
 * so emit() is permitted from it. Returns ESP_ERR_INVALID_STATE once
 * espos_flow_start() has created the real loop — the check exists precisely to
 * stop a second task emitting alongside it, and this may not be used to
 * subvert that.
 *
 * Two legitimate uses, both single-threaded by construction:
 *
 *   * Wiring that emits during start-up — a Constant priming a chain, an
 *     initial reading pushed before the loop exists.
 *   * A host test that drives the graph by hand between
 *     espos_flow_run_until_idle() calls.
 *
 * espos_flow_run_until_idle() does this for the duration of the call by
 * itself; this is the same borrow held open across several statements.
 * espos_flow_release_loop() gives it back.
 */
esp_err_t espos_flow_adopt_loop(void);

/** Undo espos_flow_adopt_loop(). Harmless when nothing was adopted. */
void espos_flow_release_loop(void);

/* espos_flow_start() revokes an outstanding borrow before it creates the loop;
 * after it returns, only the flow task is the loop. */

/* Reported by the graph when the edge pool (CONFIG_ESPOS_FLOW_MAX_EDGES) runs
 * out. Not an espos_flow_* function because it is the C++ layer that counts
 * edges; declared here so both halves agree on the number. */
void espos_flow_note_edges_used(uint32_t n);

#ifdef __cplusplus
}
#endif
