/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_power policy — may a duty-cycling device go back to sleep now?
 *
 * Pure C over plain inputs, so every rule is a host test. The rules are few,
 * and each one exists because a device that is asleep almost all the time is
 * almost never reachable, so a mistake here does not show as a bug report but
 * as a device nobody can talk to any more:
 *
 *   - Never while the running image is unconfirmed. Every wake from deep sleep
 *     is a boot, and the bootloader marks an image that is still
 *     PENDING_VERIFY at boot as aborted. A device that slept before its update
 *     confirmed itself would roll that update back on the first wake.
 *   - Stay awake for a window after any boot that is not the cycle's own timer
 *     wake — power-on, an update's reboot, a crash. That is the way back in:
 *     the web UI and OTA are reachable, and cutting the power always reopens
 *     it.
 *   - A wake has a deadline. A network that never comes up, or a server that
 *     does not answer, must not keep a battery device awake until it is flat.
 *     An application hold does not beat the deadline for the same reason.
 *
 * Threading: none. Nothing here allocates, blocks or calls out.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPOS_POWER_STAY = 0,
    ESPOS_POWER_SLEEP = 1,
} espos_power_decision_t;

/* Why the policy decided what it did; also what GET /api/v1/power reports. */
typedef enum {
    ESPOS_POWER_WHY_OFF = 0,         /* STAY: power.mode is off */
    ESPOS_POWER_WHY_UNCONFIRMED = 1, /* STAY: the running image has not confirmed itself */
    ESPOS_POWER_WHY_WINDOW = 2,      /* STAY: awake window after a boot that was not a timer wake */
    ESPOS_POWER_WHY_NETWORK = 3,     /* STAY: waiting for the network */
    ESPOS_POWER_WHY_STREAM = 4,      /* STAY: waiting for the SignalK stream */
    ESPOS_POWER_WHY_PUBLISHING = 5,  /* STAY: ready, giving the application time to publish */
    ESPOS_POWER_WHY_HOLD = 6,        /* STAY: the application holds the device awake */
    ESPOS_POWER_WHY_DONE = 7,        /* SLEEP: everything this wake was for is done */
    ESPOS_POWER_WHY_DEADLINE = 8,    /* SLEEP: the wake ran out of time */
    ESPOS_POWER_WHY_OTA = 9,         /* STAY: an update is being checked, downloaded or installed */
    ESPOS_POWER_WHY_MAX = 10,
} espos_power_why_t;

typedef struct {
    uint32_t window_ms;    /* power.window_s: awake after a boot that was not a timer wake */
    uint32_t awake_max_ms; /* power.awake_max_s: a wake's time budget once the cycle runs */
    uint32_t publish_ms;   /* power.publish_ms: time connected before sleeping */
} espos_power_policy_cfg_t;

typedef struct {
    bool enabled;           /* power.mode is cycle */
    bool timer_wake;        /* this boot is the cycle's own wake from deep sleep */
    bool image_unconfirmed; /* the running OTA image is still pending verification */
    bool ota_busy;          /* an update is being checked, downloaded or installed, or the radio
                             * co-processor's firmware is being replaced */
    bool net_up;
    uint32_t net_up_for_ms; /* how long the network has been up; ignored when it is not */
    bool have_stream;       /* SignalK is built and streaming is enabled: wait for it */
    bool stream_connected;
    uint32_t stream_up_for_ms; /* how long the stream has been connected; ignored when it is not */
    uint32_t uptime_ms;        /* since this boot */
    uint32_t holds;            /* application holds (espos_power_hold) */
} espos_power_policy_in_t;

/**
 * STAY or SLEEP, and why in `*why` (may be NULL). NULL cfg or in is STAY/OFF:
 * a caller that cannot say what it knows does not get to put the device to
 * sleep.
 */
espos_power_decision_t espos_power_policy_decide(const espos_power_policy_cfg_t *cfg, const espos_power_policy_in_t *in,
                                                 espos_power_why_t *why);

/**
 * The uptime at which this boot gives up and sleeps whatever is still
 * outstanding: awake_max_ms for a timer wake, window_ms + awake_max_ms after
 * any other boot. Saturates instead of wrapping.
 */
uint32_t espos_power_policy_deadline_ms(const espos_power_policy_cfg_t *cfg, bool timer_wake);

/** "off", "unconfirmed", "window", …; "?" for an out-of-range value. */
const char *espos_power_why_str(espos_power_why_t why);

#ifdef __cplusplus
}
#endif
