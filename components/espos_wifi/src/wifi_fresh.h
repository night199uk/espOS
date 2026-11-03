// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* wifi.fresh_assoc: drop the first association of each boot once, so an access
 * point that still holds the station from before the restart starts a fresh one
 * (espOS #289). Zero-initialised is armed. */
typedef struct {
    atomic_uint_fast8_t phase; /* the event task decides, the state machine's port cancels */
} espos_wifi_fresh_t;

/* The first association of this boot is still to come: worth reading the setting. */
bool espos_wifi_fresh_armed(const espos_wifi_fresh_t *f);

/* Dropped, and the replacement association has not come up yet. Anything the
 * dropped association still produces (an address, say) is not to be reported. */
bool espos_wifi_fresh_pending(const espos_wifi_fresh_t *f);

/* An association came up. True: the caller must not report it, and disconnects
 * instead. Decides once per boot; with `enabled` false it only disarms. */
bool espos_wifi_fresh_on_connected(espos_wifi_fresh_t *f, bool enabled);

/* A disconnect arrived. True: it is the echo of the drop (`leave` = reason
 * ASSOC_LEAVE), so the caller swallows it and reconnects at once; the drop
 * stays pending until the replacement association. Anything else while the
 * drop is pending, or after the echo, ends it, and the disconnect goes on as
 * usual. */
bool espos_wifi_fresh_on_disconnected(espos_wifi_fresh_t *f, bool leave);

/* End a pending drop without its echo, e.g. when the disconnect could not be
 * issued: the association is then reported as usual. */
void espos_wifi_fresh_cancel(espos_wifi_fresh_t *f);

#ifdef __cplusplus
}
#endif
