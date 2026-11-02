/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_health — the device's own view of what is wrong with it.
 *
 * A component that notices a condition it cannot fix — a wake service that
 * went away, internal RAM running out, a bus that stopped answering — raises
 * it here with a short stable key. Something else decides what to do with it:
 * espos_sk turns conditions into SignalK notifications, and any application
 * may add a sink of its own (a red LED, a line on a display, a relay).
 *
 * This exists so that "report a problem" is not a reason to depend on the
 * SignalK stack. espos_voice used to call espos_sk_notify() directly for its
 * one notification, which made a voice satellite unbuildable without SignalK
 * and pointed the dependency graph the wrong way — an optional component
 * depending on another optional component for a core concern. Reporting is
 * the core concern; SignalK is one sink.
 *
 * Conditions are level-triggered and idempotent: report the same state and
 * message twice and sinks are called once, so a caller may re-report on every
 * poll of whatever it is watching. ESPOS_HEALTH_NORMAL clears a condition and
 * is delivered like any other change — it is what retires an alert a previous
 * boot raised.
 *
 * The policy half (docs/health.md) is the device watchdog: a 10 s tick that
 * raises the built-in conditions (lowMemory, taskStalled), counts strikes for
 * conditions flagged ESPOS_HEALTH_F_REBOOT_ON_ALARM and restarts after N of
 * them, leaving a record the next boot can read. Losing WiFi is not a reason
 * to restart (espos_core's netDown); never getting it back is, once per
 * outage (espos_wifi's wifiStalled).
 *
 * Thread-safe. Sinks run on the reporting task with no lock held, so a sink
 * may report conditions of its own; it must not block for long.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPOS_HEALTH_NORMAL = 0,  /* condition cleared */
    ESPOS_HEALTH_WARN = 1,
    ESPOS_HEALTH_ALARM = 2,
} espos_health_state_t;

/* A key is an identifier, not a sentence ("lowMemory", "wakeService"): it is
 * what a rule or a dashboard keys on, and it ends up in a SignalK path. The
 * message is the human-readable half and may change without re-notifying. */
#define ESPOS_HEALTH_KEY_MAX 24
#define ESPOS_HEALTH_MSG_MAX 96

/* Condition flags (espos_health_report_ex). */

/* A fatal condition: held in ALARM for CONFIG_ESPOS_HEALTH_POLICY_STRIKES
 * consecutive policy ticks, it restarts the device. WARN with this flag is
 * still only a warning. Reserve it for what a restart actually fixes — a
 * wedged co-processor link, memory that will not come back — never for the
 * network being away. */
#define ESPOS_HEALTH_F_REBOOT_ON_ALARM (1u << 0)

/**
 * Raise (or with ESPOS_HEALTH_NORMAL, clear) the condition `key`.
 *
 * Sinks are called only when the state or the message actually changed.
 * `message` may be NULL or "".
 *
 * @return ESP_OK also when nothing changed; ESP_ERR_INVALID_ARG for an empty
 *         key; ESP_ERR_INVALID_SIZE when key/message exceed the maxima above
 *         (rejected rather than truncated — a clipped key would never match on
 *         the next call, so every report would consume another slot);
 *         ESP_ERR_NO_MEM when CONFIG_ESPOS_HEALTH_MAX_CONDITIONS is exhausted.
 */
esp_err_t espos_health_report(const char *key, espos_health_state_t state, const char *message);

/**
 * espos_health_report() with flags (ESPOS_HEALTH_F_*). The flags of a
 * condition are those of its latest report; a report that changes only the
 * flags is recorded but not fanned out — sinks see states and messages.
 * Unknown flag bits are ESP_ERR_INVALID_ARG.
 */
esp_err_t espos_health_report_ex(const char *key, espos_health_state_t state, const char *message, uint32_t flags);

/** Human-readable state, for logs and sinks: "normal", "warn", "alarm". */
const char *espos_health_state_str(espos_health_state_t state);

/* ------------------------------------------------------------------ sinks */

typedef void (*espos_health_sink_t)(const char *key, espos_health_state_t state,
                                    const char *message, void *arg);

/**
 * Register a sink. Every condition recorded so far is replayed into it before
 * this returns, so a sink that comes up late (espos_sk connects long after the
 * first report) still learns the current state instead of waiting for the next
 * change.
 *
 * @return ESP_ERR_NO_MEM when CONFIG_ESPOS_HEALTH_MAX_SINKS is exhausted,
 *         ESP_ERR_INVALID_STATE if (sink, arg) is already registered.
 */
esp_err_t espos_health_add_sink(espos_health_sink_t sink, void *arg);

/**
 * Tell one sink the current state of every recorded condition again, NORMAL
 * ones included, without registering anything.
 *
 * Sinks hear about a condition only when it changes, so a sink whose far end
 * can lose what it was told uses this to restore it: espos_sk calls it on
 * every stream connect, because a clear dropped from its offline buffer, or a
 * SignalK server restart, would otherwise leave the server with a stale
 * alert, or none, until the condition next changed. Runs the sink on the
 * caller's task with no espos_health lock held. A condition that changes
 * while it is being replayed is sent again, so the sink hears its newest
 * state last.
 *
 * @return ESP_ERR_TIMEOUT if the table stayed locked and the replay stopped
 *         part way, or a condition kept changing while it was replayed and
 *         its newest state may not have been the last one sent: replay again
 *         later. ESP_ERR_INVALID_ARG for a NULL sink.
 */
esp_err_t espos_health_replay(espos_health_sink_t sink, void *arg);

/**
 * Remove a sink. A call already in flight on another task may still complete
 * after this returns.
 */
esp_err_t espos_health_remove_sink(espos_health_sink_t sink, void *arg);

/* -------------------------------------------------------------- inspection */

typedef struct {
    char key[ESPOS_HEALTH_KEY_MAX];
    espos_health_state_t state;
    char message[ESPOS_HEALTH_MSG_MAX];
    uint32_t flags; /* ESPOS_HEALTH_F_* of the latest report */
} espos_health_condition_t;

/**
 * Copy the current conditions into `out` (at most `max`).
 * @param out  receives the conditions, oldest first
 * @param max  capacity of `out` in entries
 * @param out may be NULL to query the count only.
 * @return how many conditions exist, which may exceed `max`.
 */
size_t espos_health_snapshot(espos_health_condition_t *out, size_t max);

/** Worst state currently recorded — what a single status LED wants to know. */
espos_health_state_t espos_health_worst(void);

/**
 * The condition, if any, the policy would restart for: an ALARM raised with
 * ESPOS_HEALTH_F_REBOOT_ON_ALARM. What a display wants to show while the
 * strikes are still counting. `out` may be NULL.
 */
bool espos_health_fatal_alarm(espos_health_condition_t *out);

/** Forget every condition and sink (tests). */
void espos_health_reset(void);

/* ------------------------------------------------------- synthetic conditions */

/**
 * Keys a synthetic condition may use must start with this. A real condition
 * cannot be impersonated, and a reader can tell at a glance that an ALARM is a
 * drill -- which matters when the fan-out reaches a chartplotter.
 */
#define ESPOS_HEALTH_TEST_PREFIX "test."

/** Longest ttl_ms accepted: a drill nobody clears must end on its own. */
#define ESPOS_HEALTH_TEST_TTL_MAX_MS (300u * 1000u)

/**
 * Raise or clear a synthetic condition, for exercising the sinks a real fault
 * would reach (buzzer, LED, SignalK notifications) without causing the fault.
 *
 * Reported through the same path as anything real, so a sink cannot tell the
 * difference -- that is the point -- with two deliberate restrictions:
 *
 *  - flags are always 0, so this can never arm the reboot path no matter what a
 *    real condition of the same name would carry. Structural, not a promise.
 *  - `key` must start with ESPOS_HEALTH_TEST_PREFIX, else ESP_ERR_INVALID_ARG.
 *
 * At most ONE synthetic condition is active at a time: raising a second clears
 * the first. A drill on a live boat should have a bounded blast radius, and a
 * test script cannot leak fake alarms into the table by looping.
 *
 * @param state  ESPOS_HEALTH_NORMAL clears it (and disarms the ttl); WARN or
 *               ALARM raises it.
 * @param ttl_ms backstop for a test session that goes away, NOT the normal way
 *               to clear -- call again with NORMAL for that. Required when
 *               raising: 1..ESPOS_HEALTH_TEST_TTL_MAX_MS. Ignored when clearing.
 * @return ESP_ERR_INVALID_ARG for a bad key, state or ttl; ESP_ERR_NO_MEM when
 *         the condition table is full (CONFIG_ESPOS_HEALTH_MAX_CONDITIONS --
 *         note a key keeps its slot for the life of the boot, so reuse one key).
 */
esp_err_t espos_health_report_test(const char *key, espos_health_state_t state, const char *message,
                                   uint32_t ttl_ms);

/**
 * Clear the synthetic condition if its ttl has elapsed; otherwise do nothing.
 * Cheap and idempotent.
 *
 * The policy tick calls this, so on a device with espos_health_policy_start()
 * running the backstop fires within a tick. Nothing else drives it, so a caller
 * that reads the conditions should call this first rather than assume a tick has
 * happened -- GET /api/v1/health does.
 *
 * @return true if a synthetic condition was cleared by this call.
 */
bool espos_health_test_expire(void);

/* ------------------------------------------------------------------ policy */

/**
 * Arm the device watchdog: a periodic tick (CONFIG_ESPOS_HEALTH_POLICY_TICK_S,
 * 10 s) that reports lowMemory and taskStalled, counts consecutive ticks on
 * which a fatal ALARM is held and, at CONFIG_ESPOS_HEALTH_POLICY_STRIKES of
 * them, writes the reset record and restarts. Idempotent. espos_start() calls
 * this when espos_start_opts_t.health_watchdog is set (the default); call it
 * yourself only when bringing espOS up by hand.
 *
 * Runs on the esp_timer task: ticks are short, and the sinks a report reaches
 * from it are held to the same rule as everywhere else (do not block).
 */
esp_err_t espos_health_policy_start(void);

/**
 * Watch the calling task: subscribe it to the IDF task watchdog (a task that
 * stops calling espos_health_kick() for CONFIG_ESP_TASK_WDT_TIMEOUT_S panics,
 * which core-dumps, restarts and — before an OTA image is confirmed — rolls
 * back) and register it with the policy, which raises `taskStalled` as a
 * fatal ALARM once the task has been silent for `timeout_ms`. Idempotent per
 * task; a second call updates name and timeout. `name` is clipped to 15
 * characters. A watched task must call espos_health_unwatch_task() before it
 * exits. ESP_ERR_NO_MEM when ESPOS_HEALTH_WATCHED_MAX tasks are watched.
 */
esp_err_t espos_health_watch_task(const char *name, uint32_t timeout_ms);

/** Stop watching the calling task (both the task watchdog and the policy). */
esp_err_t espos_health_unwatch_task(void);

/**
 * The calling task is alive: feed the task watchdog and stamp the policy's
 * registry. Cheap and lock-free; call it from the loop you want watched. A
 * task that is not watched may call it harmlessly.
 */
void espos_health_kick(void);

/* ------------------------------------------------------------ reset record */

/* Written right before the policy restarts the device; survives the restart in
 * RTC (or plain no-init) memory. `magic` is set by the writer and cleared by
 * the first reader, so a record is seen by exactly one boot. */
typedef struct {
    uint32_t magic;
    char key[ESPOS_HEALTH_KEY_MAX]; /* the condition that struck out */
    char message[64];               /* its message, clipped */
    uint32_t min_free_heap;         /* low-water marks at the time, bytes */
    uint32_t min_internal;
    uint32_t largest_block;         /* largest free internal block at the time */
    uint32_t uptime_s;              /* how long that boot had run */
    int64_t unix_ms;                /* wall clock, 0 when it was never set */
} espos_health_reset_record_t;

/**
 * The record the previous boot left when the policy restarted the device.
 * Valid for the whole of this boot (every caller gets it; /system/info shows
 * it as `last_reset`) and gone after the next restart, whatever its cause:
 * the first call of a boot clears the stored copy. False when the last reset
 * was not the policy's — power-on, a panic, an OTA reboot, a user's reboot.
 */
bool espos_health_last_reset(espos_health_reset_record_t *out);

#ifdef __cplusplus
}
#endif
