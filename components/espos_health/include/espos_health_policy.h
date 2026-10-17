/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_health policy — the device watchdog as a pure C state machine.
 *
 * One tick (every CONFIG_ESPOS_HEALTH_POLICY_TICK_S on a device) reads the
 * heap, raises or clears the built-in conditions — lowMemory from the heap,
 * taskStalled from the registry of watched tasks — then asks whether any
 * condition flagged ESPOS_HEALTH_F_REBOOT_ON_ALARM is in ALARM and counts
 * strikes: N consecutive such ticks write a reset record and restart. WARN
 * never restarts, an ALARM without the flag never restarts, and one clean tick
 * resets the count, so a condition that comes and goes never adds up.
 *
 * Nothing here touches the platform. Clock, heap, the condition table, the
 * record store and the restart are injected through espos_health_policy_port_t,
 * so the machine runs unchanged on the host under test — the same shape as
 * espos_wifi_sm and espos_sk_token_sm. espos_health_policy_start()
 * (espos_health.h) owns the one instance a device runs.
 *
 * Threading: the caller serialises init/tick/watch/unwatch (the singleton uses
 * its own lock); port callbacks run on the caller's task with whatever it
 * holds. espos_health_policy_kick() is deliberately lock-free — it only stamps
 * a slot — so it may run on any task at any rate.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "espos_health.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPOS_HEALTH_WATCHED_MAX   8
#define ESPOS_HEALTH_TASK_NAME_MAX 16

/* What the policy reads on every tick. Bytes throughout. */
typedef struct {
    uint32_t total_free;    /* free in the default heap */
    uint32_t total_min;     /* low-water mark of total_free since boot */
    uint32_t internal_free; /* free internal byte-addressable RAM — the scarce pool */
    uint32_t internal_min;  /* low-water mark of internal_free since boot */
    uint32_t largest_block; /* largest allocatable internal 8-bit block */
} espos_health_heap_t;

typedef struct {
    uint32_t strikes;                /* consecutive fatal ticks before a restart (0 counts as 1) */
    uint32_t heap_warn_kb;           /* lowMemory WARN below this much total heap; 0 = off */
    uint32_t internal_warn_kb;       /* lowMemory WARN below this much internal RAM; 0 = off */
    uint32_t internal_alarm_kb;      /* lowMemory fatal ALARM below this much internal RAM; 0 = off */
    uint32_t largest_block_alarm_kb; /* lowMemory fatal ALARM below this largest internal block; 0 = off */
    /* memoryTrough WARN when the internal-RAM low-water mark since boot is below
     * this much; 0 = off. Reporting only -- never fatal, by construction.
     *
     * The mark is heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
     * which SUMS each matching region's own minimum. Those minima need not be simultaneous, so
     * the sum is a LOWER bound on the worst simultaneous total -- no higher than it,
     * and usually below it. It says the device never had LESS free than this, not
     * that it ever had this little. Pessimistic, which is safe for a warning.
     *
     * Appended, not inserted: a mid-struct member shifts every later field for a
     * caller built against the old header. Appending changes only sizeof, which is
     * still an ABI break -- hence the ESPOS_ABI_VERSION bump -- but the narrow kind. */
    uint32_t internal_trough_warn_kb;
} espos_health_policy_cfg_t;

/* Everything the machine needs from the outside world. */
typedef struct {
    uint32_t (*now_ms)(void *ctx);   /* monotonic, wraps; differences only */
    uint32_t (*uptime_s)(void *ctx); /* for the record */
    int64_t (*unix_ms)(void *ctx);   /* wall clock; 0 when unknown */
    void (*heap)(void *ctx, espos_health_heap_t *out);
    /* Raise/clear a built-in condition: espos_health_report_ex() on a device. */
    esp_err_t (*report)(void *ctx, const char *key, espos_health_state_t state, const char *message, uint32_t flags);
    /* Is a condition flagged ESPOS_HEALTH_F_REBOOT_ON_ALARM held in ALARM?
     * espos_health_fatal_alarm() on a device. */
    bool (*fatal_alarm)(void *ctx, espos_health_condition_t *out);
    void (*store_record)(void *ctx, const espos_health_reset_record_t *rec);
    void (*restart)(void *ctx); /* does not return on a device */
} espos_health_policy_port_t;

typedef struct {
    void *task; /* opaque task identity (TaskHandle_t on a device); NULL = free slot */
    char name[ESPOS_HEALTH_TASK_NAME_MAX];
    uint32_t timeout_ms;
    uint32_t last_kick_ms;
} espos_health_watched_t;

typedef struct espos_health_policy {
    const espos_health_policy_port_t *port;
    void *ctx;
    espos_health_policy_cfg_t cfg;
    uint32_t strikes;               /* consecutive ticks a fatal ALARM was held */
    uint32_t ticks;
    bool restarting;                /* restart requested; further ticks do nothing */
    espos_health_condition_t fatal; /* the condition the last strike was for */
    espos_health_watched_t watched[ESPOS_HEALTH_WATCHED_MAX];
    bool ever_watched;              /* taskStalled exists only once a task registered */
} espos_health_policy_t;

void espos_health_policy_init(espos_health_policy_t *p, const espos_health_policy_port_t *port, void *ctx,
                              const espos_health_policy_cfg_t *cfg);

/**
 * One tick. Reports lowMemory (and taskStalled once a task is watched), then
 * counts. Returns the strike count after this tick — 0 when nothing fatal is
 * held — so the caller can log "strike 2/3: <key>". When the count reaches
 * cfg.strikes the record is stored, `restarting` is set and port->restart()
 * is called; a port whose restart returns (the host) sees no further action.
 */
uint32_t espos_health_policy_tick(espos_health_policy_t *p);

/** Register `task` (a second call for the same task updates it). ESP_ERR_NO_MEM when full. */
esp_err_t espos_health_policy_watch(espos_health_policy_t *p, void *task, const char *name, uint32_t timeout_ms);
esp_err_t espos_health_policy_unwatch(espos_health_policy_t *p, void *task);
/** Stamp `task` alive. Lock-free; unknown tasks are ignored. */
void espos_health_policy_kick(espos_health_policy_t *p, void *task);

/**
 * The lowMemory rule on its own: state, message and flags for a heap reading.
 * Exposed for tests and for a consumer that wants the same thresholds.
 */
/* Did this device come close to exhaustion at some point since boot?
 *
 * A different question from espos_health_policy_memory(), which asks about NOW.
 * Answered from the low-water marks the port already samples, so it costs a
 * comparison rather than a measurement. Returns WARN or NORMAL; never ALARM --
 * see the note on the implementation for why this must not be fatal. */
espos_health_state_t espos_health_policy_trough(const espos_health_policy_cfg_t *cfg,
                                                const espos_health_heap_t *h, char *message,
                                                size_t message_size);

espos_health_state_t espos_health_policy_memory(const espos_health_policy_cfg_t *cfg, const espos_health_heap_t *heap,
                                                char *message, size_t message_size, uint32_t *flags);

#ifdef __cplusplus
}
#endif
