/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos — one call brings espOS up.
 *
 *     #include "espos.h"
 *     void app_main(void) { ESP_ERROR_CHECK(espos_start(NULL)); ... }
 *
 * espos_start() runs the component starts in the one order that works
 * (docs/concepts.md): the log ring first so the boot log is kept for
 * /api/v1/logs; the config store next because everything else reads it;
 * the HTTP server before the network seam (espos_net) so /net/status and
 * the provisioning portal have a page to serve the moment there is a link
 * or an access point; espos_net before WiFi because it owns the hostname
 * the station's DHCP request carries and the mDNS responder; WiFi before
 * SignalK because discovery is mDNS; then OTA and BLE, which need all of
 * the above. Each espos_*_start() checks its own prerequisites and fails
 * with ESP_ERR_INVALID_STATE when called out of order — espos_start() is
 * how an application never sees that error.
 *
 * Optional components (espos_sk, espos_ota, espos_ble) are started only
 * when the project builds them. That is decided at configure time from the
 * component list, not by the application. espos_wifi is built wherever the
 * chip has a radio (or, ESP32-P4, a co-processor) and left out on the
 * 802.15.4-only H-series; CONFIG_ESPOS_WIFI (default y) is the switch for a
 * firmware that links it but does not want the station started.
 *
 * Threading: call once, from app_main() or any task. The calls block until
 * every component has started its own tasks, then return; before_network
 * runs on the caller's task. Thread-safe against a second caller.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* The name the device presents: the SignalK access-request description
     * and the hostname prefix. NULL = the project name (PROJECT_NAME). */
    const char *app_name;
    /* Runs after espos_init() — log and config are up — and before the
     * network stack: bring a display up here so it can show the portal
     * SSID. A non-OK return aborts espos_start() with that error. */
    esp_err_t (*before_network)(void *arg);
    void *arg; /* handed to before_network */
    /* Restart on health conditions the device cannot recover from. Default
     * true; also gated by the espOS core Kconfig option. The policy sink
     * is armed in espos_init() via espos_health_policy_start(); see docs/health.md. */
    bool health_watchdog;
    /* The physical board, for a human reading /api/v1/system/info: "Waveshare
     * ESP32-P4-WIFI6-Touch-LCD-7B". NOTHING can discover this -- IDF knows the
     * chip, not what it is soldered to; the MAC's OUI is Espressif's, not the
     * board vendor's; and the USER_DATA efuse a vendor could burn an id into is
     * blank on every board we have seen. The firmware is the only thing that
     * knows, and it usually already does: a Kconfig `choice` selecting the
     * board has a prompt string that is exactly this. NULL = omit the field
     * rather than guess.
     *
     * APPENDED, not inserted next to app_name where it reads better: a caller
     * using positional initialisers would otherwise have every field after the
     * insertion point silently shift by one. Nothing in this repo does that,
     * but the whole point of a public ABI is the callers that are not in it. */
    const char *board;
} espos_start_opts_t;

#define ESPOS_START_OPTS_DEFAULT { .app_name = NULL, .before_network = NULL, .arg = NULL, .health_watchdog = true, .board = NULL }

/**
 * Bring everything up: log → config → [before_network] → httpd → net →
 * [wifi] → [sk] → [ota] → [ble], the bracketed ones only if built (and,
 * for wifi, enabled). NULL = defaults.
 * Idempotent: a second call returns ESP_OK and does nothing. On failure
 * the components started so far stay up and the error names the stage.
 */
esp_err_t espos_start(const espos_start_opts_t *opts);

/**
 * First half of espos_start(): the log ring and the config store, then
 * ESPOS_EVENT_CONFIG_READY. Idempotent; an application that initialised
 * the store itself beforehand is fine. For code that must run between
 * config and the network and cannot be expressed as before_network.
 */
esp_err_t espos_init(void);

/** Second half: httpd → net → [wifi] → [sk] → [ota] → [ble]. Requires the
 * config store to be up (espos_init()); ESP_ERR_INVALID_STATE otherwise. Idempotent. */
esp_err_t espos_start_network(void);

/** espOS's own version ("0.7.0"): the checkout's version.txt at build time,
 * or the version the component manager installed for a registry copy. Not the
 * application's version — that is PROJECT_VER, on the boot banner. */
const char *espos_version(void);

/** espos_start_opts_t.app_name if one was given, else PROJECT_NAME. Valid
 * before espos_start() (the project name) and stable afterwards. */
const char *espos_app_name(void);

/** espos_start_opts_t.board if one was given, else NULL. A label only: espOS
 * neither validates nor interprets it. */
const char *espos_board(void);

/**
 * The public C ABI: every header under the components' include directories, taken
 * together (docs/development.md, "Public API rules"). Bumped by any change
 * to one of them that is not purely additive — a function, type, macro or
 * enum value removed or renamed; a signature changed; a struct's members
 * changed in any way (appending one changes sizeof, which a caller compiled
 * against the old header has baked in); an enum or macro value changed; a
 * callback's threading contract changed; a new exception to the include
 * rule. Not bumped by a new function, macro or header, or an enum value
 * appended before its _MAX when no public struct is sized by that _MAX.
 * A binding generated from the headers records the value it was built
 * against and compares it with espos_abi_version() at run time.
 */
#define ESPOS_ABI_VERSION 8
/* History, so that a binding pinned to an older value can tell what moved:
 *   8  espos_sk_delta_cfg_t gained no_drop (appended, sizeof) and
 *      espos_sk_delta_flush() returns esp_err_t instead of void, for ring
 *      backpressure -- espOS #238.
 *   7  portal_force moved from espos_wifi_sm_t into espos_wifi_sm_status_t,
 *      so that GET /api/v1/wifi/status can report it; both sizeof values
 *      changed. espos_wifi_portal_force_str() is new, which on its own would
 *      not have needed a bump.
 *   6  espos_wifi_cfg_t gained portal_online and espos_wifi_sm_t gained
 *      other_net_up + portal_force (both appended, sizeof) -- espOS #158.
 *      The portal policy changed with them: see docs/wifi.md.
 *   5  espos_httpd_auth_policy_t gained recovery_until_s and recovery
 *      (appended, sizeof) -- espOS #154. The portal exemption narrowed with
 *      it, which is a behaviour change rather than an ABI one.
 *   4  espos_health_policy_cfg_t gained internal_trough_warn_kb (appended, so
 *      sizeof changed, which is what a caller bakes in) -- espOS #129.
 *   3  espos_wifi_sm_t gained connect_in_flight (appended, sizeof) -- #146.
 *   2  espos_start_opts_t gained board (appended, sizeof) -- #113.
 *   1  the first value, introduced with the rule itself -- #36.
 */

/** ESPOS_ABI_VERSION of the espos_core actually linked, for code compiled
 * against another copy of the headers. Callable at any time, any task. */
int espos_abi_version(void);

#ifdef __cplusplus
}
#endif
