// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
/*
 * Liveness watchdog for the esp_hosted co-processor link (e.g. ESP32-P4
 * host + ESP32-C6 radio over SDIO).
 *
 * The failure this exists for: the transport dies while the host keeps
 * believing WiFi is fine. The symptom is a flood of
 *
 *     rpc_core: Timeout waiting for Resp for [0x126](Req_WifiStaGetApInfo)
 *
 * and nothing else — the whole RPC channel is gone, so the host cannot
 * even ask whether it is connected. esp_hosted declares
 * ESP_HOSTED_EVENT_TRANSPORT_FAILURE for faults it detects itself (a
 * dropped SDIO read, an all-ones PKT_LEN), but a silently wedged link
 * produces no event at all: there is nothing to detect, only an absence.
 *
 * The co-processor heartbeat supplies that missing signal. It arrives
 * over the same RPC channel that dies, so its absence IS the fault
 * detector. Miss enough of them and the link is gone regardless of what
 * the WiFi state machine believes.
 *
 * Recovery is a deliberate esp_restart(), not a transport re-init: the
 * esp_hosted_deinit()/init() pair asserts instead of failing when it cannot
 * re-allocate its SDIO pool (recover() below, and docs/wifi.md, "Why a
 * restart and not a transport re-init"). Detection is the valuable half: a
 * device that restarts 60 s after its link dies beats one that sits
 * unreachable until someone power-cycles it.
 */

#include "sdkconfig.h"

/* esp_hosted 3.x spells its own enable CONFIG_ESP_HOSTED; 2.x said
 * CONFIG_ESP_HOSTED_ENABLED. Getting that wrong compiles this whole file away
 * without a word -- the watchdog simply stops existing.
 *
 * The heartbeat is a second condition, not a nicety: it is the ONLY signal
 * this file has. 3.x made the feature opt-in and defaults it off, and the bare
 * symptom is unhelpful -- the build stops on "implicit declaration of function
 * 'esp_hosted_configure_heartbeat'", which never mentions the key. Gated and
 * warned about instead, with the stubs at the end of the file taking over,
 * because the one thing that must NOT happen is arming a 60 s timer for a
 * heartbeat that can never arrive: that is a device rebooting every minute on
 * healthy hardware. */
#if defined(CONFIG_ESP_HOSTED) && defined(CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT)

#include "espos_health.h"
#include "espos_httpd.h" /* espos_httpd_coproc_t, and the hook we define below */
#include "espos_wifi.h"
#include "espos_wifi_priv.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_event.h"
#include "esp_hosted_host_fw_ver.h" /* ESP_HOSTED_VERSION_*_1, the host side */
#include "esp_hosted_misc.h"

/* The co-processor's firmware version, chip id and host/CP compatibility
 * verdict, read straight out of the PRIV init-event TLVs esp_hosted already
 * parsed during transport bring-up. This is NOT the public compat API: that is
 * esp_hosted_get_coprocessor_fwversion(), which is an RPC to the other chip --
 * and a co-processor old or broken enough to be worth reporting is exactly the
 * one whose RPC does not answer, so the query that would tell you times out.
 * The TLV values are already in host RAM and cost nothing to read.
 *
 * esp_hosted does not list this directory in its idf_component_register()
 * INCLUDE_DIRS; it arrives transitively because eh_host_mcu_transport declares
 * its include dir PUBLIC and eh_host links it INTERFACE into the component.
 * That is wiring upstream could tighten, so the include is guarded and the
 * file falls back to the RPC -- see cp_fetch_once(). */
#if defined(__has_include)
#if __has_include("eh_host_mcu_transport_init_event.h")
#include "eh_host_mcu_transport_init_event.h"
#define ESPOS_HOSTED_CP_VER_FROM_TLV 1
#endif
#endif
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "espos_hostedwd";

/* The co-processor emits a heartbeat every HEARTBEAT_SEC. Allow several
 * to go missing before acting: a busy link, a scan, or a burst of traffic
 * can delay one, and re-initing the transport under a healthy link would
 * be its own outage. Three intervals is late enough to be certain and
 * still well inside the ~180 s an application-level watchdog would take. */
#define HEARTBEAT_SEC      20
#define MISSED_BEATS_LIMIT 3
#define TIMEOUT_US         ((int64_t)HEARTBEAT_SEC * MISSED_BEATS_LIMIT * 1000000)

static esp_timer_handle_t s_timer;
static uint32_t s_last_beat;
static bool s_seen_beat;
/* Set by a co-processor update (coproc_update.c) for as long as it keeps the
 * link busy. Written on that task and read on the event loop, on either core:
 * see arm_timer() for the race between the two. */
static volatile bool s_held;

static void arm_timer(void)
{
    if (!s_timer || s_held) {
        return;
    }
    if (esp_timer_is_active(s_timer)) {
        esp_timer_restart(s_timer, TIMEOUT_US);
    } else {
        esp_timer_start_once(s_timer, TIMEOUT_US);
    }
    /* A hold taken between the check above and the arm stopped a timer this
     * then started again, and later beats return early without touching it,
     * so it would fire mid-update. Checked again after arming; the hold sets
     * the flag before it stops the timer, so one side always sees the other. */
    if (s_held) {
        esp_timer_stop(s_timer);
    }
}

/* Runs on the esp_timer task.
 *
 * Deliberately NOT esp_hosted_deinit()/init(): that pair asserts rather
 * than returning an error when it cannot re-allocate. A failed re-init
 * panics on whatever task called it —
 *
 *     assert failed: sdio_mempool_create sdio_drv.c:258 (buf_mp_g)
 *
 * observed on a panel doing exactly this, taking down the esp_timer
 * task. The transport is already broken at this point, so the recovery
 * must not have a failure mode of its own; a deliberate restart is the
 * one path that always works.
 *
 * Detection is the valuable half. A device that reboots 60 s after the
 * link dies is strictly better than one that sits unreachable until
 * someone power-cycles it, which is what happened before this existed. */
static void recover(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "no co-processor heartbeat for %d s — the radio link is gone, restarting",
             HEARTBEAT_SEC * MISSED_BEATS_LIMIT);
    /* esp_restart() is safe from the timer task and always succeeds;
     * that is the whole point of choosing it over a re-init that can
     * assert. The log line above is the only breadcrumb explaining the
     * restart, so emit it before going down. */
    esp_restart();
}

/* ------------------------------------------------- co-processor identity */

/* Read once and remembered.
 *
 * With ESPOS_HOSTED_CP_VER_FROM_TLV this is three memory reads of values
 * esp_hosted parsed during transport bring-up, so it can run on the event loop
 * and needs no retry: by the time a heartbeat arrives the handshake is long
 * done, and a zero version means the co-processor did not advertise one rather
 * than that the answer has not come yet. Without it, the fallback is the RPC,
 * which blocks -- so that path keeps the bounded worker task.
 *
 * A zero version is reported as such, not hidden: a co-processor whose firmware
 * predates the ESP_PRIV_FIRMWARE_VERSION TLV says nothing about itself, which
 * is why esp_hosted's own boot log reads "Co-proc [0.0.0]" on a link that is
 * working perfectly. Absent and 0.0.0 are different facts and the status
 * document keeps them apart. */

static espos_httpd_coproc_t s_cp;
static volatile bool s_cp_known;
/* s_cp is written on whichever task runs the fetch and read on the httpd task,
 * so the flag and the fields it describes are published as ONE step rather
 * than leaving a reader to see a half-written version string beside a stale
 * flag. A spinlock, not a mutex: the critical section is one struct copy and
 * neither side may block the other.
 *
 * cp_fetch_once()'s own `if (s_cp_known) return;` reads the flag WITHOUT the
 * lock, deliberately. It only ever goes false->true, and the cost of reading a
 * stale false is one redundant fetch that publishes the same values over the
 * same ones -- so the lock would buy nothing. volatile so the compiler cannot
 * hoist the read out of a loop. Every read that goes on to touch s_cp's
 * FIELDS takes the lock. */
static portMUX_TYPE s_cp_mux = portMUX_INITIALIZER_UNLOCKED;

static void cp_version_str(char *out, size_t n, uint32_t maj, uint32_t min, uint32_t patch)
{
    snprintf(out, n, "%" PRIu32 ".%" PRIu32 ".%" PRIu32, maj, min, patch);
}

/* Publish, log and report the condition. Shared by both fetch paths so they
 * cannot disagree about what "stale" means. */
static void cp_publish(espos_httpd_coproc_t *cp)
{
    portENTER_CRITICAL(&s_cp_mux);
    s_cp = *cp;
    s_cp_known = true;
    portEXIT_CRITICAL(&s_cp_mux);

    if (cp->stale) {
        /* esp_hosted says this once at boot and never again, into a ring that
         * rotates. Reported as a condition as well, so the device keeps
         * answering for it: this is a standing precondition for the very fault
         * this file watches for, and the one thing an operator cannot discover
         * after the fact. */
        ESP_LOGW(TAG, "co-processor firmware %s is older than this build's esp_hosted %s — "
                      "RPC timeouts are expected until it is updated (docs/hardware.md)",
                 cp->version, cp->host_version);
        char msg[96];
        /* The two versions, which do not change for the life of the boot. A
         * message carrying anything live would defeat espos_health's duplicate
         * suppression and fan out on every tick -- the churn that fragmented a
         * board into a reboot loop (#124). */
        snprintf(msg, sizeof(msg), "co-processor %s, host expects %s", cp->version, cp->host_version);
        (void)espos_health_report("coprocessorStale", ESPOS_HEALTH_WARN, msg);
    } else {
        ESP_LOGI(TAG, "co-processor %s firmware %s", cp->target[0] ? cp->target : "radio", cp->version);
        /* Reported either way: a condition that only ever appears is one an
         * operator cannot tell from a device that never checked. */
        (void)espos_health_report("coprocessorStale", ESPOS_HEALTH_NORMAL, "");
    }
}

static void cp_fill_host_version(espos_httpd_coproc_t *cp)
{
    cp_version_str(cp->host_version, sizeof(cp->host_version), ESP_HOSTED_VERSION_MAJOR_1,
                   ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1);
}

#if defined(ESPOS_HOSTED_CP_VER_FROM_TLV)

/* The chip ids esp_hosted can report. Its own table is static inside
 * eh_host_mcu_transport_init_event.c, so this one exists rather than being
 * borrowed; an id it does not cover is printed as a number instead of being
 * dropped, because "a co-processor we do not have a name for" is still a fact
 * worth showing. Values from eh_common_caps.h. */
static void cp_target_name(char *out, size_t n, uint8_t chip_id)
{
    switch (chip_id) {
    case 0x00: snprintf(out, n, "esp32"); return;
    case 0x02: snprintf(out, n, "esp32s2"); return;
    case 0x05: snprintf(out, n, "esp32c3"); return;
    case 0x09: snprintf(out, n, "esp32s3"); return;
    case 0x0C: snprintf(out, n, "esp32c2"); return;
    case 0x0D: snprintf(out, n, "esp32c6"); return;
    case 0x10: snprintf(out, n, "esp32h2"); return;
    case 0x14: snprintf(out, n, "esp32c61"); return;
    case 0x17: snprintf(out, n, "esp32c5"); return;
    case 0x1C: snprintf(out, n, "esp32h4"); return;
    case 0xFF: out[0] = '\0'; return; /* UNRECOGNIZED: say nothing, not "0xff" */
    default: snprintf(out, n, "0x%02x", (unsigned)chip_id); return;
    }
}

static void cp_fetch_once(void)
{
    if (s_cp_known) {
        return;
    }
    espos_httpd_coproc_t cp;
    memset(&cp, 0, sizeof(cp));

    const uint32_t cp_ver = eh_host_mcu_transport_get_fw_version();
    cp_version_str(cp.version, sizeof(cp.version), (cp_ver >> 16) & 0xFF, (cp_ver >> 8) & 0xFF,
                   cp_ver & 0xFF);
    cp_fill_host_version(&cp);
    cp_target_name(cp.target, sizeof(cp.target), eh_host_mcu_transport_get_chip_id());

    /* esp_hosted's own verdict, so this flag cannot disagree with the warning
     * it logs: 0 = compatible (including a patch-level difference), +1 = the
     * co-processor is behind, -1 = the host is. Only +1 is "stale" here; a host
     * older than its co-processor is a different problem and not this flag's.
     * Called exactly once because it logs at W/E level, and a per-request call
     * would fan that out into the log ring. */
    cp.stale = (eh_host_mcu_transport_verify_fw_compat(cp_ver) > 0);

    cp_publish(&cp);
}

#else /* no TLV accessor: fall back to the RPC, which blocks */

/* Long enough that a co-processor which never answers costs one query every
 * few minutes rather than one per heartbeat, short enough that a slow slave is
 * picked up while somebody is still looking at the device. */
#define CP_RETRY_QUIET_MS (5 * 60 * 1000)

/* Both written and read ONLY on the default event loop, which is one task, so
 * they need no synchronisation at all -- and that is a property of the design,
 * not an assumption: the worker does not clear the gate.
 *
 * An earlier shape had the worker clear it on giving up. That left two tasks
 * writing one flag, and a worker clearing it just as the loop was testing it
 * could let a second worker start beside the first. Letting the retry window
 * do the job instead removes the second writer: the worker makes at most
 * 5 attempts 2 s apart, ~10 s, so by the time CP_RETRY_QUIET_MS has elapsed it
 * is long gone and the loop can safely treat the gate as free. */
static bool s_cp_task_started;
static uint32_t s_cp_last_try_ms;

static void cp_fetch_once(void)
{
    if (s_cp_known) {
        return;
    }
    esp_hosted_coprocessor_fwver_t ver = { 0 };
    if (esp_hosted_get_coprocessor_fwversion(&ver) != ESP_OK) {
        return; /* try again on the next heartbeat */
    }
    espos_httpd_coproc_t cp;
    memset(&cp, 0, sizeof(cp));
    cp_version_str(cp.version, sizeof(cp.version), ver.major1, ver.minor1, ver.patch1);
    cp_fill_host_version(&cp);

    uint32_t chip_id = 0;
    char target[16] = { 0 };
    if (esp_hosted_get_cp_info(&chip_id, target, sizeof(target)) == ESP_OK) {
        snprintf(cp.target, sizeof(cp.target), "%s", target);
    }

    /* Major and minor only, and numerically.
     *
     * The patch level is deliberately excluded because that is what esp_hosted
     * itself does before deciding whether to warn. This flag exists to answer
     * "is my co-processor the thing esp_hosted is complaining about", so it has
     * to agree with esp_hosted or it answers a different question. Numerically,
     * because a string compare makes 2.9.0 newer than 2.12.0 -- the same trap
     * the registry's own version listing sets. */
    const uint32_t host[2] = { ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1 };
    const uint32_t co[2] = { ver.major1, ver.minor1 };
    for (int i = 0; i < 2; i++) {
        if (host[i] != co[i]) {
            cp.stale = host[i] > co[i];
            break;
        }
    }

    cp_publish(&cp);
}

#endif /* ESPOS_HOSTED_CP_VER_FROM_TLV */

/* Asked on the httpd task, from GET /api/v1/system/info. Reads only. */
bool espos_httpd_coprocessor_hook(espos_httpd_coproc_t *out)
{
    if (!out) {
        return false;
    }
    bool known;
    portENTER_CRITICAL(&s_cp_mux);
    known = s_cp_known;
    if (known) {
        *out = s_cp;
    }
    portEXIT_CRITICAL(&s_cp_mux);
    return known;
}

/* Kick the fetch from a heartbeat. A heartbeat is proof the link is answering,
 * which start-up is not; on the TLV path it is simply the first moment worth
 * bothering, since the values have been in RAM since the handshake. */
#if defined(ESPOS_HOSTED_CP_VER_FROM_TLV)

/* Three memory reads. Runs on the caller's task -- the default event loop --
 * because there is nothing here to block on. */
static void cp_fetch_kick(void)
{
    cp_fetch_once();
}

#else /* RPC path */

/* The query is an RPC to the other chip and therefore blocks, so it runs on a
 * task of its own: on_hosted_event() is the default event loop, the task that
 * dispatches the WiFi and IP events this component feeds its state machine
 * from, and blocking it for an RPC timeout on a wedged link is the deadlock
 * this file exists to notice rather than cause. The espos_timer task is no
 * better -- it is shared by every timer in the firmware.
 *
 * Bounded on purpose. A link that never answers would otherwise retry on every
 * heartbeat for the life of the boot; after this gives up the field is simply
 * absent, which is what it means. */
static void cp_fetch_task(void *arg)
{
    (void)arg;
    bool known = false;
    for (int i = 0; i < 5 && !known; i++) {
        if (i) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        cp_fetch_once();
        portENTER_CRITICAL(&s_cp_mux);
        known = s_cp_known;
        portEXIT_CRITICAL(&s_cp_mux);
    }
    if (!known) {
        /* Only logged. The gate is NOT cleared here -- see s_cp_task_started.
         * A later heartbeat retries once CP_RETRY_QUIET_MS has passed, so a
         * slave whose RPC server is merely slow to come up does not leave the
         * field absent for the life of the boot. */
        ESP_LOGW(TAG, "co-processor did not report its firmware version; retrying later");
    }
    vTaskDelete(NULL);
}

/* Starts at most one worker, and at most one per CP_RETRY_QUIET_MS after that.
 * The elapsed-time test is what frees the gate, so the worker never writes it
 * and this stays a single-task function. */
static void cp_fetch_kick(void)
{
    /* Done means done. Without this the quiet window would spawn a 4 KB task
     * every five minutes for the life of the boot, each one returning at
     * cp_fetch_once()'s first line. Unlocked for the same reason that read is:
     * the flag only ever goes false->true. */
    if (s_cp_known) {
        return;
    }
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_cp_task_started && now_ms - s_cp_last_try_ms < CP_RETRY_QUIET_MS) {
        return;
    }
    s_cp_last_try_ms = now_ms;
    s_cp_task_started = true;
    if (xTaskCreate(cp_fetch_task, "cpver", 4096, NULL, 3, NULL) != pdPASS) {
        /* The gate stays SET. s_cp_last_try_ms is already stamped, so the
         * elapsed-time test above frees it after CP_RETRY_QUIET_MS -- clearing
         * it here instead would short-circuit that test and retry on every
         * heartbeat, which on a board too short of RAM for a 4 KB stack means
         * for ever, every 20 s. */
        ESP_LOGW(TAG, "could not start the co-processor version query; retrying later");
    }
}

#endif /* ESPOS_HOSTED_CP_VER_FROM_TLV */

static void on_hosted_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case ESP_HOSTED_EVENT_CP_HEARTBEAT: {
        const esp_hosted_event_heartbeat_t *e = (const esp_hosted_event_heartbeat_t *)data;
        /* A gap in the sequence means beats were lost but the link
         * recovered on its own — worth seeing in the log ring when
         * diagnosing a flaky slot, not worth acting on. */
        if (s_seen_beat && e->heartbeat != s_last_beat + 1) {
            ESP_LOGW(TAG, "heartbeat gap: expected %" PRIu32 ", got %" PRIu32,
                     s_last_beat + 1, e->heartbeat);
        }
        s_last_beat = e->heartbeat;
        s_seen_beat = true;
        cp_fetch_kick();
        arm_timer();
        break;
    }
    case ESP_HOSTED_EVENT_TRANSPORT_FAILURE:
        /* esp_hosted found the fault itself. With
         * CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE=y -- the default
         * espOS keeps -- it restarts the system and we never reach here. With
         * it disabled, act now rather than waiting out the heartbeat
         * timeout. */
        ESP_LOGE(TAG, "transport failure reported — restarting");
        esp_restart();
        break;
    case ESP_HOSTED_EVENT_CP_INIT:
        /* Before the first beat this is the boot handshake, not a restart:
         * the host resets the co-processor during transport bring-up and
         * the init event can reach the loop only after this handler is
         * registered. Restarting on it boot-looped a P4 panel, and on the
         * first boot of an OTA image a restart before the confirm is a
         * rollback. A co-processor that really resets this early sends no
         * beat at all, and the timer armed at start catches that. */
        if (!s_seen_beat) {
            ESP_LOGI(TAG, "co-processor init event before the first heartbeat — boot handshake, ignored");
            break;
        }
        /* The co-processor restarted underneath us, so its heartbeat
         * config went with it and no beat will ever arrive again.
         *
         * Re-enabling it here would mean an RPC from the event loop
         * task — blocking, on a link that has just proved unreliable,
         * which is exactly the mistake that made a wedged transport
         * panic the UI task. Restart instead: a co-processor that
         * reset under a running host is not a state worth nursing, and
         * the heartbeat is re-enabled cleanly on the next boot. The
         * timer is left running, so if this event ever fires spuriously
         * on a healthy link the beats keep it disarmed. */
        ESP_LOGE(TAG, "co-processor restarted underneath us — restarting");
        esp_restart();
        break;
    default:
        break;
    }
}

void espos_wifi_hosted_watchdog_hold(bool hold)
{
    s_held = hold;
    if (!s_timer) {
        return;
    }
    if (hold) {
        esp_timer_stop(s_timer); /* ESP_ERR_INVALID_STATE when idle: harmless */
    } else {
        arm_timer();
    }
}

esp_err_t espos_wifi_hosted_watchdog_start(void)
{
    if (s_timer) {
        return ESP_OK;
    }
    const esp_timer_create_args_t args = {
        .callback = recover,
        .name = "hostedwd",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID,
                                     on_hosted_event, NULL);
    if (err != ESP_OK) {
        /* No stop needed: the timer was only just created and no handler
         * is registered yet, so nothing can have armed it. */
        esp_timer_delete(s_timer);
        s_timer = NULL;
        return err;
    }
    /* Blocking RPC, but this runs once from espos_wifi_start() on the
     * app task at boot, where the link is known good and blocking is
     * expected. Never call it from the event loop or a UI task. */
    err = esp_hosted_configure_heartbeat(true, HEARTBEAT_SEC);
    if (err != ESP_OK) {
        /* Not fatal: WiFi works, we just cannot see it wedge. Unwind
         * fully so a later retry actually retries — leaving s_timer set
         * would make the next call return ESP_OK with detection off,
         * which is worse than failing. */
        ESP_LOGW(TAG, "co-processor heartbeat unavailable (%s) — "
                      "wedge detection is OFF",
                 esp_err_to_name(err));
        /* Unregister BEFORE touching the timer: the handler is already
         * live, and a heartbeat arriving mid-teardown would call
         * arm_timer() on a handle we are about to delete.
         *
         * Stop before delete: esp_timer_delete() returns
         * ESP_ERR_INVALID_STATE for an armed timer, so a delete that
         * silently failed would leak a timer that still fires recover()
         * — restarting a device whose detection we just reported OFF.
         * esp_timer_stop() on an idle timer is a harmless no-op. */
        esp_event_handler_unregister(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, on_hosted_event);
        esp_timer_stop(s_timer);
        esp_err_t derr = esp_timer_delete(s_timer);
        if (derr != ESP_OK) {
            /* Should not happen now, but leaving s_timer set is the
             * safer failure: start() then returns early instead of
             * creating a second timer over a leaked one. */
            ESP_LOGE(TAG, "esp_timer_delete: %s", esp_err_to_name(derr));
            return err;
        }
        s_timer = NULL;
        return err;
    }
    arm_timer();
    ESP_LOGI(TAG, "watching co-processor heartbeat (%d s, act after %d missed)",
             HEARTBEAT_SEC, MISSED_BEATS_LIMIT);
    return ESP_OK;
}

uint32_t espos_wifi_hosted_recoveries(void)
{
    /* Always 0 on a hosted build: recovery is a restart, so a RAM
     * counter cannot survive to report it. Kept so the API is uniform;
     * the restart itself is visible as reset_reason plus the
     * "radio link is gone" line in the log ring. */
    return 0;
}

#endif /* CONFIG_ESP_HOSTED && CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT */

#if !defined(CONFIG_ESP_HOSTED) || !defined(CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT)

#if defined(CONFIG_ESP_HOSTED) && !defined(CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT)
/* A co-processor with no heartbeat is a configuration mistake, not a mode:
 * esp_hosted raises an event for a transport fault it detects itself but has no
 * "is the link up" query, so a silently wedged SDIO link becomes undetectable
 * and the device sits unreachable until somebody power-cycles it. Loud rather
 * than fatal, so a consumer who means it can still build; silent on a
 * native-radio build, which has nothing to configure. */
#warning "espos_wifi: CONFIG_ESP_HOSTED is on but CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT is not -- co-processor wedge detection is COMPILED OUT. Set CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT=y (docs/wifi.md)."
#endif

/* Native-radio and simulator builds: the API exists so callers never
 * need an #ifdef, but there is no co-processor to watch. */
#include "espos_wifi.h"
#include "espos_wifi_priv.h"

esp_err_t espos_wifi_hosted_watchdog_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void espos_wifi_hosted_watchdog_hold(bool hold) { (void)hold; }
uint32_t espos_wifi_hosted_recoveries(void) { return 0; }
/* No co-processor on this target, so no override: espos_httpd's weak stub
 * answers false and the document carries no "coprocessor" object. Deliberately
 * NOT defined here -- a strong definition returning false would be
 * indistinguishable from the stub and would hide a missing WHOLE_ARCHIVE. */

#endif
