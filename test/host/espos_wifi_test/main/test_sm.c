/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * State machine tests. A fake port records driver calls and owns a manual
 * clock; "tick(ms)" advances time and fires the armed timer when due, so
 * every transition in docs/wifi.md is exercised deterministically.
 */
#include <string.h>
#include "unity.h"
#include "espos_wifi_sm.h"

/* ---------------------------------------------------------- fake port */

static struct {
    uint32_t now;
    bool timer_armed;
    uint32_t timer_due;
    int connects, disconnects, portal_starts, portal_stops, notifies;
    /* Timer arms, so a test can catch a deferral that re-arms in a tight
     * loop rather than waiting for the attempt (espOS #144). Cancels for the
     * same reason in the other direction: it is the only mark a policy run
     * leaves when it decides to do nothing, which is how "nothing reached the
     * port before EV_START" is checkable at all (espOS #158). */
    int arms, cancels;
    char last_ssid[33];
    bool last_has_bssid;
    uint32_t rnd;
    esp_err_t connect_result;
} F;

static esp_err_t f_connect(void *ctx, const espos_wifi_net_t *net)
{
    (void)ctx;
    F.connects++;
    strcpy(F.last_ssid, net->ssid);
    F.last_has_bssid = net->has_bssid;
    return F.connect_result;
}
static esp_err_t f_disconnect(void *ctx)
{
    (void)ctx;
    F.disconnects++;
    return ESP_OK;
}
static esp_err_t f_portal_start(void *ctx)
{
    (void)ctx;
    F.portal_starts++;
    return ESP_OK;
}
static esp_err_t f_portal_stop(void *ctx)
{
    (void)ctx;
    F.portal_stops++;
    return ESP_OK;
}
static void f_arm(void *ctx, uint32_t ms)
{
    (void)ctx;
    F.timer_armed = true;
    F.timer_due = F.now + ms;
    F.arms++;
}
static void f_cancel(void *ctx)
{
    (void)ctx;
    F.timer_armed = false;
    F.cancels++;
}
static uint32_t f_now(void *ctx)
{
    (void)ctx;
    return F.now;
}
static uint32_t f_random(void *ctx)
{
    (void)ctx;
    return F.rnd;
}
static void f_notify(void *ctx)
{
    (void)ctx;
    F.notifies++;
}

static const espos_wifi_port_t PORT = {
    .connect = f_connect,
    .disconnect = f_disconnect,
    .portal_start = f_portal_start,
    .portal_stop = f_portal_stop,
    .arm_timer = f_arm,
    .cancel_timer = f_cancel,
    .now_ms = f_now,
    .random = f_random,
    .status_changed = f_notify,
};

static espos_wifi_sm_t SM;

static espos_wifi_cfg_t cfg_with(const char *s0, const char *s1)
{
    espos_wifi_cfg_t c = { 0 };
    c.sta_enabled = true;
    if (s0) {
        strcpy(c.nets[c.net_count].ssid, s0);
        strcpy(c.nets[c.net_count].psk, "pw");
        c.net_count++;
    }
    if (s1) {
        strcpy(c.nets[c.net_count].ssid, s1);
        strcpy(c.nets[c.net_count].psk, "pw");
        c.net_count++;
    }
    c.backoff_max_ms = 60000;
    c.dhcp_timeout_ms = 15000;
    c.connect_timeout_ms = 20000;
    c.portal_enabled = true;
    c.portal_after_ms = 90000;
    return c;
}

static void reset(const espos_wifi_cfg_t *c)
{
    memset(&F, 0, sizeof(F));
    F.now = 1000;
    F.rnd = 0; /* deterministic: lower jitter bound (0.75·d) */
    espos_wifi_sm_init(&SM, &PORT, NULL, c);
}

/* Advance the clock, firing the timer once if it comes due. */
static void tick(uint32_t ms)
{
    uint32_t target = F.now + ms;
    if (F.timer_armed && (int32_t)(F.timer_due - target) <= 0) {
        F.now = F.timer_due;
        F.timer_armed = false;
        espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_TIMER, NULL);
    }
    F.now = target;
}

static void ev_disconnected(int reason) { espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_STA_DISCONNECTED, &reason); }
static void ev_connected(const char *ssid)
{
    espos_wifi_link_t l = { .channel = 6, .rssi = -60 };
    strcpy(l.ssid, ssid);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_STA_CONNECTED, &l);
}
static void ev_got_ip(void)
{
    espos_wifi_ip_t ip = { .ip = "10.0.0.5", .netmask = "255.255.255.0", .gateway = "10.0.0.1" };
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_GOT_IP, &ip);
}
#define ST() (espos_wifi_sm_status(&SM))

/* --------------------------------------------------------------- tests */

TEST_CASE("backoff: exponential, capped, jittered ±25%, floor 250 ms", "[wifi_sm]")
{
    TEST_ASSERT_EQUAL_UINT32(750, espos_wifi_backoff_ms(0, 60000, 0));      /* 1 s · 0.75 */
    TEST_ASSERT_EQUAL_UINT32(1250, espos_wifi_backoff_ms(0, 60000, 500));   /* 1 s · 1.25 */
    TEST_ASSERT_EQUAL_UINT32(1500, espos_wifi_backoff_ms(1, 60000, 0));     /* 2 s · 0.75 */
    TEST_ASSERT_EQUAL_UINT32(6000, espos_wifi_backoff_ms(3, 60000, 0));     /* 8 s · 0.75 */
    TEST_ASSERT_EQUAL_UINT32(45000, espos_wifi_backoff_ms(20, 60000, 0));   /* capped at 60 s · 0.75 */
    TEST_ASSERT_EQUAL_UINT32(75000, espos_wifi_backoff_ms(20, 60000, 30000)); /* 60 s · 1.25 */
    for (uint32_t r = 0; r < 12; r++) {
        for (uint32_t j = 0; j < 5000; j += 137) {
            uint32_t d = espos_wifi_backoff_ms(r, 60000, j);
            uint64_t base = 1000ull << r;
            if (base > 60000) base = 60000;
            TEST_ASSERT_TRUE(d >= base * 3 / 4 && d <= base * 5 / 4 + 1);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(250, espos_wifi_backoff_ms(0, 100, 0)); /* floor */
}

TEST_CASE("happy path: start → connecting → obtaining_ip → connected, portal never up", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_DISABLED, ST()->state);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL(1, F.connects);
    TEST_ASSERT_EQUAL_STRING("Boat", F.last_ssid);
    TEST_ASSERT_TRUE(F.timer_armed);                       /* connect timeout */
    TEST_ASSERT_EQUAL_UINT32(F.now + 20000, F.timer_due);
    TEST_ASSERT_EQUAL(1, ST()->attempt);
    ev_connected("Boat");
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_OBTAINING_IP, ST()->state);
    TEST_ASSERT_EQUAL_UINT32(F.now + 15000, F.timer_due);  /* dhcp timeout */
    TEST_ASSERT_EQUAL_STRING("Boat", ST()->link.ssid);
    tick(500);
    ev_got_ip();
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTED, ST()->state);
    TEST_ASSERT_FALSE(F.timer_armed);
    TEST_ASSERT_EQUAL_STRING("10.0.0.5", ST()->ip.ip);
    TEST_ASSERT_EQUAL(1, ST()->connect_count);
    TEST_ASSERT_EQUAL(0, ST()->attempt);
    TEST_ASSERT_EQUAL(0, ST()->round);
    TEST_ASSERT_EQUAL(0, ST()->reason);
    TEST_ASSERT_EQUAL(0, F.portal_starts);
    TEST_ASSERT_FALSE(ST()->portal_active);
    TEST_ASSERT_TRUE(F.notifies >= 3);
}

TEST_CASE("NO_AP_FOUND: backoff with countdown, then retry", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_disconnected(201);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);
    TEST_ASSERT_EQUAL(201, ST()->reason);
    TEST_ASSERT_EQUAL_STRING("network not in range", espos_wifi_reason_str(ST()->reason));
    TEST_ASSERT_EQUAL_UINT32(750, espos_wifi_sm_backoff_remaining_ms(&SM));
    TEST_ASSERT_EQUAL(1, ST()->round);
    tick(300);
    TEST_ASSERT_EQUAL_UINT32(450, espos_wifi_sm_backoff_remaining_ms(&SM));
    TEST_ASSERT_EQUAL(1, F.connects);
    tick(500);                                              /* timer fires at 750 */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL(2, F.connects);
    TEST_ASSERT_EQUAL_UINT32(0, espos_wifi_sm_backoff_remaining_ms(&SM));
    /* second failure: round 1 → 1.5 s */
    ev_disconnected(201);
    TEST_ASSERT_EQUAL_UINT32(1500, espos_wifi_sm_backoff_remaining_ms(&SM));
    TEST_ASSERT_EQUAL(2, ST()->round);
    /* success resets the round counter */
    tick(1500);
    ev_connected("Boat");
    ev_got_ip();
    TEST_ASSERT_EQUAL(0, ST()->round);
    TEST_ASSERT_EQUAL(0, ST()->reason);
}

TEST_CASE("AUTH_FAIL and AUTH_EXPIRE map to 'wrong password'", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_disconnected(202);
    TEST_ASSERT_EQUAL_STRING("wrong password", espos_wifi_reason_str(ST()->reason));
    tick(1000);
    ev_disconnected(2);
    TEST_ASSERT_EQUAL_STRING("wrong password (auth expired)", espos_wifi_reason_str(2));
    TEST_ASSERT_EQUAL_STRING("auth timed out, weak signal?", espos_wifi_reason_str(204));
    TEST_ASSERT_EQUAL_STRING("unknown reason", espos_wifi_reason_str(9999));
}

TEST_CASE("DHCP timeout is a distinct failure and moves to the next network", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", "Marina");
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("Boat");
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_OBTAINING_IP, ST()->state);
    tick(15000);                                            /* dhcp timer fires */
    TEST_ASSERT_EQUAL(1, F.disconnects);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_DHCP_TIMEOUT, ST()->reason);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL_STRING("Marina", F.last_ssid);         /* next in priority */
    TEST_ASSERT_EQUAL(1, ST()->net_index);
    TEST_ASSERT_EQUAL(2, ST()->attempt);
}

TEST_CASE("multi-SSID: rotate through the list, back off after a full round", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("A", "B");
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL_STRING("A", F.last_ssid);
    ev_disconnected(201);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state); /* no backoff between networks */
    TEST_ASSERT_EQUAL_STRING("B", F.last_ssid);
    ev_disconnected(202);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);   /* round complete */
    TEST_ASSERT_EQUAL(0, ST()->net_index);                   /* next round starts at A */
    TEST_ASSERT_EQUAL(202, ST()->reason);
    tick(750);
    TEST_ASSERT_EQUAL_STRING("A", F.last_ssid);
    TEST_ASSERT_EQUAL(3, F.connects);
}

TEST_CASE("drop while connected: retry the same network first", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("A", "B");
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("A");
    ev_got_ip();
    tick(5000);
    ev_disconnected(200);                                    /* beacon timeout */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL_STRING("A", F.last_ssid);              /* same one again */
    TEST_ASSERT_EQUAL(1, ST()->disconnect_count);
    TEST_ASSERT_EQUAL(200, ST()->reason);
    ev_disconnected(201);                                    /* now it is really gone */
    TEST_ASSERT_EQUAL_STRING("B", F.last_ssid);
}

TEST_CASE("connect timeout is a safety net when the driver stays silent", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("A", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    tick(20000);
    TEST_ASSERT_EQUAL(1, F.disconnects);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_CONNECT_TIMEOUT, ST()->reason);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);
}

TEST_CASE("driver refusing connect() counts as a failed attempt, no recursion blow-up", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("A", "B");
    reset(&c);
    F.connect_result = ESP_FAIL;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);
    TEST_ASSERT_EQUAL(2, F.connects);
}

TEST_CASE("unconfigured: portal immediately, no connect attempts", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_UNCONFIGURED, ST()->state);
    TEST_ASSERT_EQUAL(0, F.connects);
    TEST_ASSERT_EQUAL(1, F.portal_starts);
    TEST_ASSERT_TRUE(ST()->portal_active);
    /* configure via the portal → connect, portal down on success */
    espos_wifi_cfg_t c2 = cfg_with("Boat", NULL);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c2);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_TRUE(ST()->portal_active);                   /* stays up while trying */
    ev_connected("Boat");
    ev_got_ip();
    TEST_ASSERT_FALSE(ST()->portal_active);
    TEST_ASSERT_EQUAL(1, F.portal_stops);
}

TEST_CASE("portal comes up after portal_after without a connection, alongside retries", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.portal_after_ms = 5000;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_disconnected(201);                                    /* → backoff 750 ms */
    TEST_ASSERT_FALSE(ST()->portal_active);
    /* walk the clock; the timer alternates between portal deadline and state timeouts */
    for (int i = 0; i < 40 && !ST()->portal_active; i++) {
        tick(250);
        if (ST()->state == ESPOS_WIFI_ST_CONNECTING) {
            ev_disconnected(201);
        }
    }
    TEST_ASSERT_TRUE(ST()->portal_active);
    TEST_ASSERT_TRUE(F.now - 1000 >= 5000);
    TEST_ASSERT_TRUE(F.now - 1000 <= 5500);
    TEST_ASSERT_TRUE(F.connects >= 2);                       /* station kept retrying */
    /* connection eventually succeeds → portal down */
    tick(60000);
    if (ST()->state == ESPOS_WIFI_ST_BACKOFF) {
        tick(60000);
    }
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    ev_connected("Boat");
    ev_got_ip();
    TEST_ASSERT_FALSE(ST()->portal_active);
}

TEST_CASE("the portal is not raised while an association is in flight", "[wifi_sm]")
{
    /* The deadline elapsing DURING an attempt is the case that matters: raising
     * the portal then switches the radio to AP+STA mid-association, which on real
     * hardware preceded a ~9 minute stall where a portal settled beforehand
     * connected first try (espOS #144). */
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.portal_after_ms = 5000;
    c.connect_timeout_ms = 20000;   /* longer than the portal deadline, on purpose */
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);

    /* Sit in CONNECTING past the portal deadline without answering. */
    tick(9000);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_FALSE_MESSAGE(ST()->portal_active,
                              "portal must not come up mid-association");
    TEST_ASSERT_EQUAL_MESSAGE(0, F.portal_starts,
                              "not even started and stopped again");
}

TEST_CASE("with several networks the portal is not deferred across the whole round", "[wifi_sm]")
{
    /* attempt_failed() moves straight to the next network without passing through
     * backoff, so that gap is the only chance to honour the deadline in a round.
     * Miss it and the deferral is bounded by the ROUND, not by one attempt. */
    espos_wifi_cfg_t c = cfg_with("BoatA", "BoatB");
    c.portal_after_ms = 5000;
    c.connect_timeout_ms = 20000;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    tick(9000);                             /* past the deadline, attempt 1 in flight */
    TEST_ASSERT_FALSE(ST()->portal_active);

    ev_disconnected(201);                   /* attempt 1 fails -> straight to BoatB */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL(1, ST()->net_index);
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active,
                             "the gap between networks must honour the deadline");
}

TEST_CASE("a deferred portal comes up as soon as the attempt resolves", "[wifi_sm]")
{
    /* Deferral must be bounded by the ATTEMPT, not by success: a device that never
     * connects still has to get its portal. */
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.portal_after_ms = 5000;
    c.connect_timeout_ms = 20000;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    tick(9000);
    TEST_ASSERT_FALSE(ST()->portal_active);

    /* The attempt fails; the deadline is long past, so the portal is due now. */
    ev_disconnected(201);
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active,
                             "portal should come up the moment the attempt ends");
}

TEST_CASE("deferring does not spin the timer", "[wifi_sm]")
{
    /* A past-due deadline left pending makes arm() fold it to a 1 ms timer, which
     * fires, defers, re-arms -- a busy loop for the whole attempt. Counting timer
     * arms over a long stay in CONNECTING is what catches that. */
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.portal_after_ms = 1000;
    c.connect_timeout_ms = 30000;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    const int max_rearms = 10;
    int arms_before = F.arms;
    /* Successive expiries, not one long tick: a single tick() fires the timer at
     * most once, so a 1 ms re-arm loop would sail past it. Walk the clock in small
     * steps and stop as soon as the arm count says the loop is back, so a
     * regression fails fast rather than running to the loop bound. */
    for (int i = 0; i < 200 && (F.arms - arms_before) <= max_rearms; i++) {
        tick(100);
        if (ST()->state != ESPOS_WIFI_ST_CONNECTING) break;
    }
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_FALSE(ST()->portal_active);
    TEST_ASSERT_TRUE_MESSAGE(F.arms - arms_before <= max_rearms,
                             "timer re-armed repeatedly: the deferral is spinning");
}

TEST_CASE("a portal deferred through the attempt comes up once DHCP starts", "[wifi_sm]")
{
    /* The deadline elapsed while associating, so it is due the moment the
     * association finishes -- not dhcp_timeout_ms later. A device that associates
     * but cannot get a lease is one an operator needs to reach. */
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.portal_after_ms = 3000;
    c.connect_timeout_ms = 20000;
    c.dhcp_timeout_ms = 30000;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    tick(6000);                                   /* past the deadline, still associating */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_FALSE(ST()->portal_active);        /* deferred */

    ev_connected("Boat");                          /* association done, DHCP begins */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_OBTAINING_IP, ST()->state);
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active,
                             "a portal deferred during the attempt is due once it ends");

    ev_got_ip();                                   /* and goes away on success */
    TEST_ASSERT_FALSE(ST()->portal_active);
}

TEST_CASE("obtaining_ip does not defer the portal", "[wifi_sm]")
{
    /* The association is done by then, so a mode switch cannot disturb it -- and a
     * device stuck on DHCP is exactly one an operator needs the portal to reach. */
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.portal_after_ms = 3000;
    c.dhcp_timeout_ms = 30000;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("Boat");
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_OBTAINING_IP, ST()->state);
    tick(6000);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_OBTAINING_IP, ST()->state);
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active,
                             "portal should still be raised while waiting for DHCP");
}

TEST_CASE("portal disabled: never started", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    c.portal_enabled = false;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_UNCONFIGURED, ST()->state);
    TEST_ASSERT_EQUAL(0, F.portal_starts);
}

TEST_CASE("sta disabled: idle with the portal up; enabling starts connecting", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.sta_enabled = false;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_DISABLED, ST()->state);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_DISABLED, ST()->reason);
    TEST_ASSERT_EQUAL(0, F.connects);
    TEST_ASSERT_TRUE(ST()->portal_active);
    espos_wifi_cfg_t c2 = c;
    c2.sta_enabled = true;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c2);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    /* and disabling while connected disconnects */
    ev_connected("Boat");
    ev_got_ip();
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_DISABLED, ST()->state);
    TEST_ASSERT_EQUAL(1, F.disconnects);
    TEST_ASSERT_EQUAL(1, ST()->disconnect_count);
}

TEST_CASE("config change: same network keeps the connection, changed psk reconnects", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("Boat");
    ev_got_ip();
    /* add a second network below: nothing happens to the live link */
    espos_wifi_cfg_t c2 = cfg_with("Boat", "Marina");
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c2);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTED, ST()->state);
    TEST_ASSERT_EQUAL(0, F.disconnects);
    /* reorder: Boat becomes index 1 — still connected, index follows */
    espos_wifi_cfg_t c3 = cfg_with("Marina", "Boat");
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c3);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTED, ST()->state);
    TEST_ASSERT_EQUAL(1, ST()->net_index);
    /* identical config again: no-op */
    int n = F.notifies;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c3);
    TEST_ASSERT_EQUAL(n, F.notifies);
    /* password change of the live network → reconnect from the top */
    espos_wifi_cfg_t c4 = c3;
    strcpy(c4.nets[1].psk, "newpw");
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c4);
    TEST_ASSERT_EQUAL(1, F.disconnects);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL_STRING("Marina", F.last_ssid);         /* priority order */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_CONFIG_CHANGE, ST()->reason);
    TEST_ASSERT_EQUAL(1, ST()->disconnect_count);
}

TEST_CASE("BSSID pin is passed to the driver", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.nets[0].has_bssid = true;
    memcpy(c.nets[0].bssid, (uint8_t[]) { 1, 2, 3, 4, 5, 6 }, 6);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_TRUE(F.last_has_bssid);
}

TEST_CASE("lost IP while connected: wait for DHCP again, then fail over", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("Boat");
    ev_got_ip();
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_LOST_IP, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_OBTAINING_IP, ST()->state);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_LOST_IP, ST()->reason);
    TEST_ASSERT_EQUAL(1, ST()->disconnect_count);
    ev_got_ip();                                             /* DHCP recovered */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTED, ST()->state);
    TEST_ASSERT_EQUAL(2, ST()->connect_count);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_LOST_IP, NULL);
    tick(15000);                                             /* no DHCP this time */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_DHCP_TIMEOUT, ST()->reason);
    TEST_ASSERT_TRUE(ST()->state == ESPOS_WIFI_ST_CONNECTING || ST()->state == ESPOS_WIFI_ST_BACKOFF);
}

TEST_CASE("stale driver events in the wrong state are ignored", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_disconnected(201);                                    /* → backoff */
    int n = F.notifies;
    ev_connected("Boat");                                    /* late CONNECTED: ignored */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);
    ev_disconnected(201);                                    /* duplicate: ignored */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);
    TEST_ASSERT_EQUAL(1, ST()->round);
    TEST_ASSERT_EQUAL(n, F.notifies);
}

TEST_CASE("stop: disconnects, portal down, timer cancelled; start again works", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("Boat");
    ev_got_ip();
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_STOP, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_DISABLED, ST()->state);
    TEST_ASSERT_EQUAL(1, F.disconnects);
    TEST_ASSERT_FALSE(F.timer_armed);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_STOP, NULL);      /* idempotent */
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL(2, F.connects);
}

TEST_CASE("portal start failure is retried, not believed", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_TRUE(ST()->portal_active);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_PORTAL_FAILED, NULL);
    TEST_ASSERT_FALSE(ST()->portal_active);
    TEST_ASSERT_TRUE(F.timer_armed);
    TEST_ASSERT_EQUAL_UINT32(F.now + 10000, F.timer_due);
    tick(10000);
    TEST_ASSERT_TRUE(ST()->portal_active);
    TEST_ASSERT_EQUAL(2, F.portal_starts);
}

TEST_CASE("timing/portal knob changes do not restart an attempt", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(1, F.connects);
    espos_wifi_cfg_t c2 = c;
    c2.backoff_max_ms = 30000;
    c2.portal_after_ms = 5000;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c2);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL(1, F.connects);                        /* no restart */
    TEST_ASSERT_EQUAL_UINT32(30000, SM.cfg.backoff_max_ms);  /* but taken */
    /* a stale lease (GOT_IP without a preceding association) is ignored */
    ev_got_ip();
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    ev_connected("Boat");
    ev_got_ip();
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTED, ST()->state);
}

TEST_CASE("a timer fire that is early for the currently armed deadline is ignored", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_disconnected(201);                                    /* BACKOFF, armed for 750 ms */
    /* a fire queued from a previous arm arrives now (before the deadline) */
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_TIMER, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_BACKOFF, ST()->state);
    TEST_ASSERT_EQUAL(1, F.connects);
    tick(750);                                               /* the real one */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    TEST_ASSERT_EQUAL(2, F.connects);
    /* after GOT_IP the timer is cancelled: a late fire does nothing */
    ev_connected("Boat");
    ev_got_ip();
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_TIMER, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTED, ST()->state);
}

TEST_CASE("portal deadline firing does not extend the DHCP/connect timeout", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.portal_after_ms = 3000;
    c.dhcp_timeout_ms = 10000;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("Boat");                                    /* dhcp timer: due at +10 s */
    uint32_t dhcp_due = F.now + 10000;
    TEST_ASSERT_TRUE(F.timer_due < dhcp_due);                /* portal deadline (+3 s) pre-empts */
    tick(3000);                                              /* portal fires */
    TEST_ASSERT_TRUE(ST()->portal_active);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_OBTAINING_IP, ST()->state);
    TEST_ASSERT_EQUAL_UINT32(dhcp_due, F.timer_due);         /* original deadline kept */
    tick(7000);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_REASON_DHCP_TIMEOUT, ST()->reason);
}

TEST_CASE("portal reconfig bounces a running portal only", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(1, F.portal_starts);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_PORTAL_RECONFIG, NULL);
    TEST_ASSERT_EQUAL(1, F.portal_stops);
    TEST_ASSERT_EQUAL(2, F.portal_starts);
    TEST_ASSERT_TRUE(ST()->portal_active);
    espos_wifi_cfg_t c2 = cfg_with("Boat", NULL);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c2);
    ev_connected("Boat");
    ev_got_ip();
    TEST_ASSERT_FALSE(ST()->portal_active);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_PORTAL_RECONFIG, NULL); /* nothing to bounce */
    TEST_ASSERT_EQUAL(2, F.portal_starts);
}

TEST_CASE("portal client count is tracked", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    int two = 2;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_PORTAL_CLIENT, &two);
    TEST_ASSERT_EQUAL(2, ST()->portal_clients);
}

TEST_CASE("a client count that arrives after the portal stopped is ignored", "[wifi_sm]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_TRUE(ST()->portal_active);
    espos_wifi_cfg_t c2 = cfg_with("Boat", NULL);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &c2);
    ev_connected("Boat");
    ev_got_ip();
    TEST_ASSERT_FALSE(ST()->portal_active);
    int one = 1;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_PORTAL_CLIENT, &one);
    TEST_ASSERT_EQUAL(0, ST()->portal_clients);
}

TEST_CASE("state names", "[wifi_sm]")
{
    TEST_ASSERT_EQUAL_STRING("connected", espos_wifi_state_str(ESPOS_WIFI_ST_CONNECTED));
    TEST_ASSERT_EQUAL_STRING("backoff", espos_wifi_state_str(ESPOS_WIFI_ST_BACKOFF));
    TEST_ASSERT_EQUAL_STRING("unconfigured", espos_wifi_state_str(ESPOS_WIFI_ST_UNCONFIGURED));
    TEST_ASSERT_EQUAL_STRING("obtaining_ip", espos_wifi_state_str(ESPOS_WIFI_ST_OBTAINING_IP));
}

/* ------------------------------------------- another transport (espOS #158) */

static void ev_other_net(bool up)
{
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_OTHER_NET, &up);
}

static void ev_portal_force(espos_wifi_portal_force_t f)
{
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_PORTAL_FORCE, &f);
}

TEST_CASE("Ethernet up: an unconfigured device raises no portal", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    /* The reported case: no station network, so this machine never reaches
     * CONNECTED and nothing ever took the portal down -- an open access point
     * for as long as the device was powered, on a device reachable over
     * Ethernet the whole time. */
    ev_other_net(true);
    /* Seeded before EV_START, where it must be recorded and nothing else: the
     * driver is not started yet, so a policy run here would reach a port that
     * cannot serve it. EV_START applies it. */
    TEST_ASSERT_EQUAL_MESSAGE(0, F.notifies, "a pre-start event drove the machine");
    TEST_ASSERT_EQUAL_MESSAGE(0, F.cancels, "a pre-start event reached the timer");
    TEST_ASSERT_EQUAL_MESSAGE(0, F.portal_starts + F.portal_stops, "a pre-start event reached the radio");

    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_UNCONFIGURED, ST()->state);
    TEST_ASSERT_FALSE_MESSAGE(ST()->portal_active, "an open access point nobody needs");
    TEST_ASSERT_EQUAL(0, F.portal_starts);
}

TEST_CASE("Ethernet arriving takes a running portal down", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_TRUE(ST()->portal_active);
    /* Ethernet usually comes up AFTER the portal -- a PHY negotiates while the
     * portal is already serving -- so declining to raise one is not enough. */
    ev_other_net(true);
    TEST_ASSERT_FALSE(ST()->portal_active);
    TEST_ASSERT_EQUAL(1, F.portal_stops);
}

TEST_CASE("Ethernet going away puts the portal back at once", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    ev_other_net(true);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_FALSE(ST()->portal_active);
    /* Unplugged: the device has no way in left, so the portal is owed
     * immediately -- not after another portal_after_s, because the clock it is
     * measured from has not moved while we sat here disconnected. */
    ev_other_net(false);
    TEST_ASSERT_TRUE(ST()->portal_active);
    TEST_ASSERT_EQUAL(1, F.portal_starts);
}

TEST_CASE("Ethernet up does not suppress the portal when portal_online is set", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    c.portal_online = true;
    reset(&c);
    ev_other_net(true);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active, "wifi.portal_online was ignored");
}

TEST_CASE("Ethernet up still defers to the configured deadline, then suppresses", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.portal_after_ms = 5000;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_other_net(true);
    /* A configured device that cannot reach its network: the deadline passes
     * and the portal still does not come up, because Ethernet is carrying it. */
    ev_disconnected(201);
    F.now += 6000;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_TIMER, NULL);
    TEST_ASSERT_FALSE(ST()->portal_active);
}

TEST_CASE("a repeated OTHER_NET does not bounce the portal", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_other_net(true);
    TEST_ASSERT_EQUAL(1, F.portal_stops);
    /* NETWORK_UP fires again whenever the default route moves, and the port
     * re-reads rather than trusting the id, so the same answer arrives often. */
    ev_other_net(true);
    ev_other_net(true);
    TEST_ASSERT_EQUAL(1, F.portal_stops);
    TEST_ASSERT_EQUAL(1, F.portal_starts);
}

TEST_CASE("portal_open(false) holds it down with no network at all", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_TRUE(ST()->portal_active);
    ev_portal_force(ESPOS_WIFI_PORTAL_DOWN);
    TEST_ASSERT_FALSE(ST()->portal_active);
    /* And it stays down: this is an application decision, not a hint. */
    ev_other_net(false);
    TEST_ASSERT_FALSE(ST()->portal_active);
    ev_portal_force(ESPOS_WIFI_PORTAL_AUTO);
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active, "auto did not hand the decision back");
}

TEST_CASE("the standing portal request is reported, not just obeyed", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    /* The request decides whether an access point exists. A reader who cannot
     * see it cannot tell "the policy declined" from "an application said no",
     * which are the same `portal_active: false` and need different answers. */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_PORTAL_AUTO, ST()->portal_force);

    ev_portal_force(ESPOS_WIFI_PORTAL_DOWN);
    TEST_ASSERT_FALSE(ST()->portal_active);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_PORTAL_DOWN, ST()->portal_force);

    ev_portal_force(ESPOS_WIFI_PORTAL_UP);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_PORTAL_UP, ST()->portal_force);

    ev_portal_force(ESPOS_WIFI_PORTAL_AUTO);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_PORTAL_AUTO, ST()->portal_force);
}

TEST_CASE("an out-of-range force request is ignored, and reported as such", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_portal_force(ESPOS_WIFI_PORTAL_DOWN);

    espos_wifi_portal_force_t junk = (espos_wifi_portal_force_t)99;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_PORTAL_FORCE, &junk);
    /* Refused rather than stored: a value the string helper cannot name would
     * be reported as "auto" while behaving like neither. */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_PORTAL_DOWN, ST()->portal_force);
}

TEST_CASE("force names round-trip through the string helper", "[wifi_sm]")
{
    TEST_ASSERT_EQUAL_STRING("auto", espos_wifi_portal_force_str(ESPOS_WIFI_PORTAL_AUTO));
    TEST_ASSERT_EQUAL_STRING("up", espos_wifi_portal_force_str(ESPOS_WIFI_PORTAL_UP));
    TEST_ASSERT_EQUAL_STRING("down", espos_wifi_portal_force_str(ESPOS_WIFI_PORTAL_DOWN));
    /* Out of range reads as the default rather than as a dangling pointer. */
    TEST_ASSERT_EQUAL_STRING("auto", espos_wifi_portal_force_str((espos_wifi_portal_force_t)99));
    TEST_ASSERT_EQUAL_STRING("auto", espos_wifi_portal_force_str(ESPOS_WIFI_PORTAL_FORCE_MAX));
}

TEST_CASE("portal_open(true) holds it up through a connection", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_portal_force(ESPOS_WIFI_PORTAL_UP);
    /* Deferred, not dropped: the attempt in flight comes first (espOS #144). */
    TEST_ASSERT_FALSE(ST()->portal_active);
    ev_connected("Boat");
    /* The association is done, so the request is honoured -- and connecting,
     * which normally takes the portal down, does not: an application that
     * asked for it up is commissioning something and wants AP+STA. */
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active, "the deferred request was never applied");
    ev_got_ip();
    TEST_ASSERT_TRUE(ST()->portal_active);
    ev_other_net(true);
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active, "suppression overrode an explicit request");
}

TEST_CASE("neither override outranks portal_enabled = false", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with(NULL, NULL);
    c.portal_enabled = false;
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_portal_force(ESPOS_WIFI_PORTAL_UP);
    /* An operator who turned the access point off in the configuration keeps
     * it off; the override is for deciding within what they allowed. */
    TEST_ASSERT_FALSE_MESSAGE(ST()->portal_active, "an app overrode the operator");
    TEST_ASSERT_EQUAL(0, F.portal_starts);
}

TEST_CASE("Ethernet going away pulls the timer in to the portal deadline", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.portal_after_ms = 1000; /* sooner than the 15 s DHCP timeout below */
    reset(&c);
    ev_other_net(true);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_connected("Boat");
    /* OBTAINING_IP on purpose: the association is done, so the espOS #144
     * deferral does not apply and the policy will schedule rather than skip.
     * The armed timer is the 15 s DHCP timeout. */
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_OBTAINING_IP, ST()->state);
    const uint32_t deadline = 1000 + c.portal_after_ms; /* reset() starts the clock at 1000 */
    TEST_ASSERT_TRUE(F.timer_due > deadline);

    /* Unplugged: the portal is owed in under a second while the armed timer is
     * fifteen away. arm() folds the portal deadline in when it is CALLED, not
     * retroactively, so without a re-arm here the portal waits out the DHCP
     * timeout -- it does arrive, which is why "does it arrive" cannot catch
     * this, but it arrives fourteen seconds late. */
    ev_other_net(false);
    TEST_ASSERT_FALSE_MESSAGE(ST()->portal_active, "raised early, so this tests nothing");
    TEST_ASSERT_TRUE_MESSAGE(F.timer_armed, "nothing is armed, so nothing will raise it");
    TEST_ASSERT_TRUE_MESSAGE(F.timer_due <= deadline, "the timer still points past the portal deadline");

    F.now = F.timer_due;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_TIMER, NULL);
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active, "the deadline fired and nothing came up");
    TEST_ASSERT_TRUE_MESSAGE(F.now <= deadline, "the portal was late");
    /* The DHCP timeout it displaced is still owed, not cancelled. */
    TEST_ASSERT_TRUE_MESSAGE(F.timer_armed, "the DHCP timeout was lost with the re-arm");
}

TEST_CASE("Ethernet going away inside the deadline arms the timer for it", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    c.portal_after_ms = 30000;
    reset(&c);
    ev_other_net(true);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_disconnected(201); /* retrying; the portal is owed 30 s from here */
    F.now += 1000;

    /* Unplugged well before the deadline, so the policy schedules rather than
     * raises. Nothing acts on portal_due_ms unless the timer is armed for it:
     * arm() folds the deadline in when it is CALLED, not retroactively, so
     * without a re-arm here the portal arrives whenever some unrelated
     * timeout happens to fire -- or never. */
    ev_other_net(false);
    TEST_ASSERT_FALSE(ST()->portal_active);
    TEST_ASSERT_TRUE_MESSAGE(F.timer_armed, "nothing is armed, so nothing will raise it");

    /* Which timer fires first is not the point -- a retry deadline is sooner
     * than the portal's and legitimately wins -- so drive the clock through
     * them and assert only the guarantee: the portal is owed and arrives. A
     * bounded loop, because the failure being guarded against is a deadline
     * nothing is armed for, which here looks like running out of timers. */
    for (int i = 0; i < 40 && !ST()->portal_active; i++) {
        TEST_ASSERT_TRUE_MESSAGE(F.timer_armed, "the portal deadline was dropped");
        F.now = F.timer_due;
        espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_TIMER, NULL);
    }
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active, "the deadline passed and nothing came up");
}

TEST_CASE("a forced portal still waits out an association", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);

    /* An application asking for the portal does not get to undo espOS #144:
     * switching to APSTA inside an attempt cost one board a ~9 minute stall.
     * The request is sticky, so it is honoured the moment the attempt ends. */
    ev_portal_force(ESPOS_WIFI_PORTAL_UP);
    TEST_ASSERT_FALSE_MESSAGE(ST()->portal_active, "APSTA switch inside an association");
    int arms_before = F.arms;

    ev_disconnected(201);
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active, "the request was dropped, not deferred");
    /* And it did not spin re-arming a past-due timer while it waited. */
    TEST_ASSERT_TRUE(F.arms - arms_before <= 2);
}

TEST_CASE("disabling the station mid-attempt does not strand a forced portal", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_CONNECTING, ST()->state);
    ev_portal_force(ESPOS_WIFI_PORTAL_UP); /* deferred: an attempt is in flight */
    TEST_ASSERT_FALSE(ST()->portal_active);

    /* The attempt is now abandoned rather than resolved, so nothing will ever
     * report it finished. A connect_in_flight left set here defers the portal
     * for the life of the boot. */
    espos_wifi_cfg_t off = c;
    off.sta_enabled = false;
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &off);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_DISABLED, ST()->state);
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active, "the deferral outlived the attempt");
}

TEST_CASE("removing every network mid-attempt does not strand a forced portal", "[wifi_sm][othernet]")
{
    espos_wifi_cfg_t c = cfg_with("Boat", NULL);
    reset(&c);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_START, NULL);
    ev_portal_force(ESPOS_WIFI_PORTAL_UP);
    TEST_ASSERT_FALSE(ST()->portal_active);

    /* Same gap by the other route: UNCONFIGURED is reached without any attempt
     * resolving. */
    espos_wifi_cfg_t none = cfg_with(NULL, NULL);
    espos_wifi_sm_event(&SM, ESPOS_WIFI_EV_CONFIG, &none);
    TEST_ASSERT_EQUAL(ESPOS_WIFI_ST_UNCONFIGURED, ST()->state);
    TEST_ASSERT_TRUE_MESSAGE(ST()->portal_active, "the deferral outlived the attempt");
}
