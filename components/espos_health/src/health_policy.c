/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The watchdog policy, pure C. See espos_health_policy.h; the device glue is
 * health_watchdog.c, the host test drives this directly with a fake port.
 */
#include <stdio.h>
#include <string.h>

#include "espos_health_policy.h"

/* Magic of a reset record; shared with the ports through the header's type
 * only, so it lives here where the record is written. */
#define RECORD_MAGIC 0x6865616Cu /* "heal" */

void espos_health_policy_init(espos_health_policy_t *p, const espos_health_policy_port_t *port, void *ctx,
                              const espos_health_policy_cfg_t *cfg)
{
    memset(p, 0, sizeof(*p));
    p->port = port;
    p->ctx = ctx;
    p->cfg = *cfg;
    if (p->cfg.strikes == 0) {
        p->cfg.strikes = 1;
    }
}

espos_health_state_t espos_health_policy_memory(const espos_health_policy_cfg_t *cfg, const espos_health_heap_t *h,
                                                char *message, size_t message_size, uint32_t *flags)
{
    uint32_t f = 0;
    espos_health_state_t st = ESPOS_HEALTH_NORMAL;
    const char *m = "";
    char buf[ESPOS_HEALTH_MSG_MAX];

    /* Alarms first, then warnings: internal RAM is judged on its own because on
     * a PSRAM board the total can read tens of megabytes while the internal
     * pool — the one the radio, DMA and the task stacks come from — is gone.
     *
     * The message states the THRESHOLD that was breached, not the live figure.
     * That is not cosmetic: espos_health_report() suppresses a report whose
     * state and message both match the last one, and a message carrying the
     * current byte count never matches, so every tick fanned out to every sink
     * for as long as the condition held. On a board whose idle free heap sits
     * below the warn floor -- an ESP32-C5 with ~166 KiB internal RAM idles at
     * 27-35 KB against the default 40 KB -- that is a SignalK notification
     * built, published and freed every 10 s for the life of the device. The
     * churn fragmented the heap until `largest_block` fell through its alarm
     * floor and the watchdog rebooted the board every ~9 minutes with 18 KB
     * still free (espOS #124).
     *
     * The live numbers are what a human wants, so they stay in the log line
     * espos_health_report() writes and in GET /api/v1/system/info -- both read
     * the heap directly. What a sink needs is the state, and the state is what
     * it now gets. */
    if (cfg->internal_alarm_kb && h->internal_free < cfg->internal_alarm_kb * 1024u) {
        st = ESPOS_HEALTH_ALARM;
        f = ESPOS_HEALTH_F_REBOOT_ON_ALARM;
        snprintf(buf, sizeof(buf), "internal RAM exhausted (below %u KB free)",
                 (unsigned)cfg->internal_alarm_kb);
        m = buf;
    } else if (cfg->largest_block_alarm_kb && h->largest_block < cfg->largest_block_alarm_kb * 1024u) {
        st = ESPOS_HEALTH_ALARM;
        f = ESPOS_HEALTH_F_REBOOT_ON_ALARM;
        /* A small largest block means fragmentation only while plenty is free.
         * Below the warn floor -- or below the block floor itself, which no
         * layout could satisfy -- the free RAM is simply spread over the chip's
         * separate regions, and "fragmented" sent a HALMET owner hunting a
         * fragmenting allocator when the heap was just running out. */
        const bool low = (cfg->internal_warn_kb && h->internal_free < cfg->internal_warn_kb * 1024u) ||
                         h->internal_free < cfg->largest_block_alarm_kb * 1024u;
        snprintf(buf, sizeof(buf), "internal RAM %s (largest block below %u KB)", low ? "low" : "fragmented",
                 (unsigned)cfg->largest_block_alarm_kb);
        m = buf;
    } else if (cfg->internal_warn_kb && h->internal_free < cfg->internal_warn_kb * 1024u) {
        st = ESPOS_HEALTH_WARN;
        snprintf(buf, sizeof(buf), "internal RAM low (below %u KB free)",
                 (unsigned)cfg->internal_warn_kb);
        m = buf;
    } else if (cfg->heap_warn_kb && h->total_free < cfg->heap_warn_kb * 1024u) {
        st = ESPOS_HEALTH_WARN;
        snprintf(buf, sizeof(buf), "heap low (below %u KB free)",
                 (unsigned)cfg->heap_warn_kb);
        m = buf;
    }
    if (message && message_size) {
        snprintf(message, message_size, "%s", m);
    }
    if (flags) {
        *flags = f;
    }
    return st;
}

/* "This device got close to nothing at some point", which is not what
 * espos_health_policy_memory() answers.
 *
 * WHY IT IS WORTH A SEPARATE CONDITION. The live checks read the instantaneous
 * free size on a tick, so a trough BETWEEN two ticks is invisible to them --
 * and a transient trough is exactly what precedes an allocation failure.
 * Measured on a Waveshare ESP32-C5 running the BLE gateway: 27604 B free after
 * 27 hours of uptime, with a low-water mark of 4924 B -- and 148 B on a later
 * run. No tick came near either: each read a free size well above the fatal floor.
 * The mark sums per-region floors, so it does not prove the TOTAL got that low --
 * but since the parts sum to it, every region had been within 148 B of exhaustion
 * at its own worst moment, and an allocation fails on a region (espOS #129). The port
 * already samples these marks -- they were written into the reset record and
 * read only after a restart, so the measurement existed and nothing decided on
 * it.
 *
 * WHY IT IS NEVER FATAL. A low-water mark does not recover: once it has dipped,
 * it reads that way for the life of the boot. A fatal condition on a value that
 * cannot improve would reboot the device, observe the same mark on the next
 * boot's first dip, and reboot again. It is a diagnostic -- it tells an operator
 * where to look -- and the live ALARMs remain what actually restarts anything.
 *
 * Deliberately internal-RAM only. The total is the less interesting pool on any
 * board with PSRAM, and a second sticky condition on it would mostly repeat
 * this one. */
espos_health_state_t espos_health_policy_trough(const espos_health_policy_cfg_t *cfg,
                                                const espos_health_heap_t *h, char *message,
                                                size_t message_size)
{
    espos_health_state_t st = ESPOS_HEALTH_NORMAL;
    const char *m = "";
    char buf[ESPOS_HEALTH_MSG_MAX];

    if (cfg->internal_trough_warn_kb && h->internal_min < cfg->internal_trough_warn_kb * 1024u) {
        st = ESPOS_HEALTH_WARN;
        /* The THRESHOLD, not the mark -- espos_health_report() suppresses a
         * repeat only when state AND message both match, and a message carrying
         * the live figure would fan out to every sink on every tick. The mark
         * itself is in GET /api/v1/system/info as `min_internal_free`, and in
         * the watchdog's own heap log line. Same reasoning as the live checks
         * above; see espOS #124 for what it cost to learn.
         *
         * "low-water mark", not "fell below": heap_caps_get_minimum_free_size()
         * SUMS each matching region's own minimum, and those minima need not have
         * happened together, so the total may never have held this value at any
         * one instant. Summing them gives a LOWER bound on the worst simultaneous
         * total -- no HIGHER than it, never a value the device is known to have
         * reached. Two 100 KB regions that each dip to 10 KB at different times
         * report 20 KB while the total never left 110 KB. Pessimistic is the safe
         * direction for a warning, but the message must not claim a moment that
         * may not have existed. */
        snprintf(buf, sizeof(buf), "internal RAM low-water mark below %u KB since boot",
                 (unsigned)cfg->internal_trough_warn_kb);
        m = buf;
    }
    if (message && message_size) {
        snprintf(message, message_size, "%s", m);
    }
    return st;
}

static void check_tasks(espos_health_policy_t *p, uint32_t now)
{
    const espos_health_watched_t *stalled = NULL;
    uint32_t worst_age = 0;
    for (size_t i = 0; i < ESPOS_HEALTH_WATCHED_MAX; i++) {
        const espos_health_watched_t *w = &p->watched[i];
        if (!w->task) {
            continue;
        }
        /* `now` was sampled once at the top of the tick; a kick from the
         * watched task can land between that sample and this read, stamping
         * a time a few ms in the future. Unsigned subtraction would turn
         * that into ~2^32 ms of silence and a spurious strike (seen on the
         * P4 panel at display frame rate). A newer stamp means alive. */
        int32_t delta = (int32_t)(now - w->last_kick_ms);
        uint32_t age = delta < 0 ? 0 : (uint32_t)delta;
        if (age > w->timeout_ms && age >= worst_age) {
            stalled = w;
            worst_age = age;
        }
    }
    if (stalled) {
        char m[ESPOS_HEALTH_MSG_MAX];
        snprintf(m, sizeof(m), "%s silent for %u s (limit %u s)", stalled->name, (unsigned)(worst_age / 1000),
                 (unsigned)(stalled->timeout_ms / 1000));
        p->port->report(p->ctx, "taskStalled", ESPOS_HEALTH_ALARM, m, ESPOS_HEALTH_F_REBOOT_ON_ALARM);
    } else {
        p->port->report(p->ctx, "taskStalled", ESPOS_HEALTH_NORMAL, "", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
    }
}

uint32_t espos_health_policy_tick(espos_health_policy_t *p)
{
    if (p->restarting) {
        return p->strikes;
    }
    p->ticks++;
    uint32_t now = p->port->now_ms(p->ctx);

    espos_health_heap_t h = { 0 };
    p->port->heap(p->ctx, &h);
    char msg[ESPOS_HEALTH_MSG_MAX];
    uint32_t flags = 0;
    espos_health_state_t st = espos_health_policy_memory(&p->cfg, &h, msg, sizeof(msg), &flags);
    p->port->report(p->ctx, "lowMemory", st, msg, flags);

    /* Its own key, not folded into lowMemory: that check is an if/else chain
     * returning one condition, so a trough would be masked whenever a live
     * threshold fired -- which is precisely when it is most worth knowing that
     * the device has been here before. Flags are 0: never fatal. */
    char trough_msg[ESPOS_HEALTH_MSG_MAX];
    espos_health_state_t tst = espos_health_policy_trough(&p->cfg, &h, trough_msg, sizeof(trough_msg));
    p->port->report(p->ctx, "memoryTrough", tst, trough_msg, 0);

    /* Not before the first watch: a firmware that watches nothing should not
     * spend a condition slot (and a NORMAL notification) on taskStalled. */
    if (p->ever_watched) {
        check_tasks(p, now);
    }

    /* The table decides, not the reports above: a consumer's own fatal
     * condition (a silent N2K bus) counts exactly like lowMemory does. */
    espos_health_condition_t c;
    if (!p->port->fatal_alarm(p->ctx, &c)) {
        p->strikes = 0;
        return 0;
    }
    p->fatal = c;
    p->strikes++;
    if (p->strikes < p->cfg.strikes) {
        return p->strikes;
    }

    espos_health_reset_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = RECORD_MAGIC;
    snprintf(rec.key, sizeof(rec.key), "%s", c.key);
    /* Clipped on purpose: the record keeps 63 of the message's 95 characters. */
    size_t n = strnlen(c.message, sizeof(rec.message) - 1);
    memcpy(rec.message, c.message, n);
    rec.message[n] = '\0';
    rec.min_free_heap = h.total_min;
    rec.min_internal = h.internal_min;
    rec.largest_block = h.largest_block;
    rec.uptime_s = p->port->uptime_s(p->ctx);
    rec.unix_ms = p->port->unix_ms(p->ctx);
    p->restarting = true;
    p->port->store_record(p->ctx, &rec);
    p->port->restart(p->ctx);
    return p->strikes;
}

static espos_health_watched_t *find(espos_health_policy_t *p, void *task)
{
    for (size_t i = 0; i < ESPOS_HEALTH_WATCHED_MAX; i++) {
        if (p->watched[i].task == task) {
            return &p->watched[i];
        }
    }
    return NULL;
}

esp_err_t espos_health_policy_watch(espos_health_policy_t *p, void *task, const char *name, uint32_t timeout_ms)
{
    if (!task || !name || !name[0] || !p->port) {
        return ESP_ERR_INVALID_ARG;
    }
    espos_health_watched_t *w = find(p, task);
    if (!w) {
        w = find(p, NULL);
        if (!w) {
            return ESP_ERR_NO_MEM;
        }
    }
    /* Fill before publishing the identity: kick() scans without a lock and
     * must never see a half-written slot. Re-watching keeps the old stamp
     * out of the picture too — the task is alive, it is calling us. */
    snprintf(w->name, sizeof(w->name), "%s", name);
    w->timeout_ms = timeout_ms;
    w->last_kick_ms = p->port->now_ms(p->ctx);
    w->task = task;
    p->ever_watched = true;
    return ESP_OK;
}

esp_err_t espos_health_policy_unwatch(espos_health_policy_t *p, void *task)
{
    espos_health_watched_t *w = task ? find(p, task) : NULL;
    if (!w) {
        return ESP_ERR_NOT_FOUND;
    }
    w->task = NULL; /* a tombstone, not a compaction: kick() may be scanning */
    return ESP_OK;
}

void espos_health_policy_kick(espos_health_policy_t *p, void *task)
{
    if (!p->port || !task) {
        return;
    }
    espos_health_watched_t *w = find(p, task);
    if (w) {
        w->last_kick_ms = p->port->now_ms(p->ctx);
    }
}
