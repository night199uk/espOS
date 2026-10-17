/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_health policy: the device watchdog, driven with a fake port.
 *
 * The properties that matter: a fatal ALARM restarts after exactly N strikes
 * and not before; a warning or an unflagged ALARM never does, however long it
 * lasts; one clean tick resets the count, so a condition that comes and goes
 * never adds up to a restart; and the record left behind names what struck
 * out. The fake port either scripts the answers or routes them into the real
 * condition table, so the path a consumer's own fatal condition takes —
 * espos_health_report_ex() → espos_health_fatal_alarm() → strikes — is the
 * one exercised here.
 */
#include <string.h>

#include "espos_health.h"
#include "espos_health_policy.h"
#include "unity.h"

/* ------------------------------------------------------------ fake port */

#define MAX_REPORTS 64

typedef struct {
    char key[ESPOS_HEALTH_KEY_MAX];
    espos_health_state_t state;
    char message[ESPOS_HEALTH_MSG_MAX];
    uint32_t flags;
} report_t;

static struct {
    uint32_t now_ms;
    uint32_t uptime_s;
    int64_t unix_ms;
    espos_health_heap_t heap;
    /* scripted answer, unless routed through the real table */
    bool fatal;
    espos_health_condition_t fatal_cond;
    bool use_table;
    int restarts, stores;
    espos_health_reset_record_t stored;
    report_t rep[MAX_REPORTS];
    size_t rep_n;
} F;

static uint32_t f_now_ms(void *ctx)
{
    (void)ctx;
    return F.now_ms;
}
static uint32_t f_uptime_s(void *ctx)
{
    (void)ctx;
    return F.uptime_s;
}
static int64_t f_unix_ms(void *ctx)
{
    (void)ctx;
    return F.unix_ms;
}
static void f_heap(void *ctx, espos_health_heap_t *out)
{
    (void)ctx;
    *out = F.heap;
}

static esp_err_t f_report(void *ctx, const char *key, espos_health_state_t st, const char *msg, uint32_t flags)
{
    (void)ctx;
    if (F.rep_n < MAX_REPORTS) {
        report_t *r = &F.rep[F.rep_n++];
        snprintf(r->key, sizeof(r->key), "%s", key);
        r->state = st;
        snprintf(r->message, sizeof(r->message), "%s", msg ? msg : "");
        r->flags = flags;
    }
    return F.use_table ? espos_health_report_ex(key, st, msg, flags) : ESP_OK;
}

static bool f_fatal_alarm(void *ctx, espos_health_condition_t *out)
{
    (void)ctx;
    if (F.use_table) {
        return espos_health_fatal_alarm(out);
    }
    if (F.fatal && out) {
        *out = F.fatal_cond;
    }
    return F.fatal;
}

static void f_store(void *ctx, const espos_health_reset_record_t *rec)
{
    (void)ctx;
    F.stores++;
    F.stored = *rec;
}
static void f_restart(void *ctx)
{
    (void)ctx;
    F.restarts++;
}

static const espos_health_policy_port_t PORT = {
    .now_ms = f_now_ms,
    .uptime_s = f_uptime_s,
    .unix_ms = f_unix_ms,
    .heap = f_heap,
    .report = f_report,
    .fatal_alarm = f_fatal_alarm,
    .store_record = f_store,
    .restart = f_restart,
};

static espos_health_policy_t P;

/* The defaults a device runs with (Kconfig): 3 strikes, 40/20 KB warn, 12/8 KB alarm. */
static const espos_health_policy_cfg_t CFG = {
    .strikes = 3,
    .heap_warn_kb = 40,
    .internal_warn_kb = 20,
    .internal_alarm_kb = 12,
    .largest_block_alarm_kb = 8,
    .internal_trough_warn_kb = 10,
};

/* A comfortable heap: nothing to warn about. */
static const espos_health_heap_t HEALTHY = {
    .total_free = 200 * 1024,
    .total_min = 150 * 1024,
    .internal_free = 90 * 1024,
    .internal_min = 60 * 1024,
    .largest_block = 40 * 1024,
};

static void fresh(bool use_table)
{
    espos_health_reset();
    memset(&F, 0, sizeof(F));
    F.now_ms = 5000;
    F.uptime_s = 5;
    F.unix_ms = 1767225600000LL; /* 2026-01-01T00:00:00Z */
    F.heap = HEALTHY;
    F.use_table = use_table;
    espos_health_policy_init(&P, &PORT, NULL, &CFG);
}

/* One policy tick, the clock advanced by the device's tick period first. */
static uint32_t tick(void)
{
    F.now_ms += 10000;
    F.uptime_s += 10;
    return espos_health_policy_tick(&P);
}

static void script_fatal(bool on, const char *key, const char *msg)
{
    F.fatal = on;
    memset(&F.fatal_cond, 0, sizeof(F.fatal_cond));
    snprintf(F.fatal_cond.key, sizeof(F.fatal_cond.key), "%s", key);
    snprintf(F.fatal_cond.message, sizeof(F.fatal_cond.message), "%s", msg);
    F.fatal_cond.state = ESPOS_HEALTH_ALARM;
    F.fatal_cond.flags = ESPOS_HEALTH_F_REBOOT_ON_ALARM;
}

static const report_t *last_report(const char *key)
{
    for (size_t i = F.rep_n; i > 0; i--) {
        if (strcmp(F.rep[i - 1].key, key) == 0) return &F.rep[i - 1];
    }
    return NULL;
}

/* ---------------------------------------------------------- the strikes */

TEST_CASE("a fatal ALARM restarts after exactly N strikes", "[health][policy]")
{
    fresh(false);
    script_fatal(true, "n2kBus", "no frames for 30 s");

    TEST_ASSERT_EQUAL_UINT32(1, tick());
    TEST_ASSERT_EQUAL_INT(0, F.restarts);
    TEST_ASSERT_EQUAL_UINT32(2, tick());
    TEST_ASSERT_EQUAL_INT(0, F.restarts);
    TEST_ASSERT_EQUAL_UINT32(3, tick());
    TEST_ASSERT_EQUAL_INT(1, F.restarts);
    TEST_ASSERT_EQUAL_INT(1, F.stores);
    TEST_ASSERT_TRUE(P.restarting);
}

/* On a device restart() never returns; the host's does, and the policy must
 * not then keep restarting on every tick or write a second record. */
TEST_CASE("a restart is requested once", "[health][policy]")
{
    fresh(false);
    script_fatal(true, "n2kBus", "");
    for (int i = 0; i < 10; i++) tick();
    TEST_ASSERT_EQUAL_INT(1, F.restarts);
    TEST_ASSERT_EQUAL_INT(1, F.stores);
}

TEST_CASE("recovery resets the strike count", "[health][policy]")
{
    fresh(false);
    script_fatal(true, "n2kBus", "");
    tick();
    tick();
    script_fatal(false, "", "");
    TEST_ASSERT_EQUAL_UINT32(0, tick()); /* one clean tick */
    script_fatal(true, "n2kBus", "");
    TEST_ASSERT_EQUAL_UINT32(1, tick());
    TEST_ASSERT_EQUAL_UINT32(2, tick());
    TEST_ASSERT_EQUAL_INT(0, F.restarts); /* 2 + 2 is not 3 in a row */
    TEST_ASSERT_EQUAL_UINT32(3, tick());
    TEST_ASSERT_EQUAL_INT(1, F.restarts);
}

TEST_CASE("strikes of 0 means the first fatal tick restarts", "[health][policy]")
{
    fresh(false);
    espos_health_policy_cfg_t c = CFG;
    c.strikes = 0;
    espos_health_policy_init(&P, &PORT, NULL, &c);
    script_fatal(true, "x", "");
    tick();
    TEST_ASSERT_EQUAL_INT(1, F.restarts);
}

/* The table is the judge, exactly as on a device: a consumer marks its own
 * condition fatal with espos_health_report_ex() and the policy finds it. */
TEST_CASE("a consumer's fatal condition in the real table strikes out", "[health][policy]")
{
    fresh(true);
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_ex("n2kBus", ESPOS_HEALTH_ALARM, "bus silent", ESPOS_HEALTH_F_REBOOT_ON_ALARM));
    tick();
    tick();
    TEST_ASSERT_EQUAL_INT(0, F.restarts);
    tick();
    TEST_ASSERT_EQUAL_INT(1, F.restarts);
    TEST_ASSERT_EQUAL_STRING("n2kBus", F.stored.key);
    TEST_ASSERT_EQUAL_STRING("bus silent", F.stored.message);
}

TEST_CASE("WARN never restarts, flagged or not", "[health][policy]")
{
    fresh(true);
    espos_health_report_ex("netDown", ESPOS_HEALTH_WARN, "station link lost", 0);
    espos_health_report_ex("n2kBus", ESPOS_HEALTH_WARN, "quiet", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
    for (int i = 0; i < 20; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, tick());
    }
    TEST_ASSERT_EQUAL_INT(0, F.restarts);
    TEST_ASSERT_EQUAL_INT(0, F.stores);
}

TEST_CASE("an ALARM without the fatal flag never restarts", "[health][policy]")
{
    fresh(true);
    espos_health_report("wakeService", ESPOS_HEALTH_ALARM, "unreachable");
    for (int i = 0; i < 20; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, tick());
    }
    TEST_ASSERT_EQUAL_INT(0, F.restarts);
}

TEST_CASE("clearing the fatal condition mid-count starts over", "[health][policy]")
{
    fresh(true);
    espos_health_report_ex("n2kBus", ESPOS_HEALTH_ALARM, "", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
    tick();
    tick();
    espos_health_report_ex("n2kBus", ESPOS_HEALTH_NORMAL, "", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
    TEST_ASSERT_EQUAL_UINT32(0, tick());
    TEST_ASSERT_EQUAL_INT(0, F.restarts);
}

/* ------------------------------------------------------------ the record */

TEST_CASE("the reset record names the condition and the heap at the time", "[health][policy]")
{
    fresh(false);
    F.heap.total_min = 33 * 1024;
    F.heap.internal_min = 14 * 1024;
    F.heap.largest_block = 9 * 1024;
    script_fatal(true, "skLinkStalled", "stream down for over 300 s while WiFi reports connected");
    tick();
    tick();
    tick();
    TEST_ASSERT_EQUAL_INT(1, F.stores);
    TEST_ASSERT_NOT_EQUAL(0, F.stored.magic);
    TEST_ASSERT_EQUAL_STRING("skLinkStalled", F.stored.key);
    TEST_ASSERT_EQUAL_STRING("stream down for over 300 s while WiFi reports connected", F.stored.message);
    TEST_ASSERT_EQUAL_UINT32(33 * 1024, F.stored.min_free_heap);
    TEST_ASSERT_EQUAL_UINT32(14 * 1024, F.stored.min_internal);
    TEST_ASSERT_EQUAL_UINT32(9 * 1024, F.stored.largest_block);
    TEST_ASSERT_EQUAL_UINT32(35, F.stored.uptime_s); /* 5 s at start + three 10 s ticks */
    TEST_ASSERT_EQUAL_INT64(1767225600000LL, F.stored.unix_ms);
}

/* A consumer's message may be the full 95 characters; the record keeps 63. */
TEST_CASE("the record clips a long message instead of overflowing", "[health][policy]")
{
    fresh(false);
    char m[ESPOS_HEALTH_MSG_MAX];
    memset(m, 'x', sizeof(m) - 1);
    m[sizeof(m) - 1] = '\0';
    script_fatal(true, "k", m);
    tick();
    tick();
    tick();
    TEST_ASSERT_EQUAL_UINT32(sizeof(F.stored.message) - 1, (uint32_t)strlen(F.stored.message));
}

/* ------------------------------------------------------------ lowMemory */

TEST_CASE("lowMemory thresholds: warn on total or internal, fatal alarm on internal or block", "[health][policy]")
{
    char msg[ESPOS_HEALTH_MSG_MAX];
    uint32_t flags;
    espos_health_heap_t h = HEALTHY;

    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, espos_health_policy_memory(&CFG, &h, msg, sizeof(msg), &flags));
    TEST_ASSERT_EQUAL_STRING("", msg);
    TEST_ASSERT_EQUAL_UINT32(0, flags);

    h = HEALTHY;
    h.total_free = 39 * 1024;
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, espos_health_policy_memory(&CFG, &h, msg, sizeof(msg), &flags));
    TEST_ASSERT_EQUAL_UINT32(0, flags);
    TEST_ASSERT_NOT_NULL(strstr(msg, "heap low"));

    h = HEALTHY;
    h.internal_free = 19 * 1024;
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, espos_health_policy_memory(&CFG, &h, msg, sizeof(msg), &flags));
    TEST_ASSERT_EQUAL_UINT32(0, flags);
    TEST_ASSERT_NOT_NULL(strstr(msg, "internal RAM low"));

    h = HEALTHY;
    h.internal_free = 11 * 1024;
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, espos_health_policy_memory(&CFG, &h, msg, sizeof(msg), &flags));
    TEST_ASSERT_EQUAL_UINT32(ESPOS_HEALTH_F_REBOOT_ON_ALARM, flags);
    TEST_ASSERT_NOT_NULL(strstr(msg, "exhausted"));

    h = HEALTHY;
    h.largest_block = 7 * 1024;
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, espos_health_policy_memory(&CFG, &h, msg, sizeof(msg), &flags));
    TEST_ASSERT_EQUAL_UINT32(ESPOS_HEALTH_F_REBOOT_ON_ALARM, flags);
    TEST_ASSERT_NOT_NULL(strstr(msg, "fragmented"));

    /* The HALMET's figures: free RAM below the warn floor, spread over regions.
     * Still fatal, but named for running out, not for fragmentation. */
    h = HEALTHY;
    h.internal_free = 18 * 1024;
    h.largest_block = 7 * 1024;
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, espos_health_policy_memory(&CFG, &h, msg, sizeof(msg), &flags));
    TEST_ASSERT_EQUAL_UINT32(ESPOS_HEALTH_F_REBOOT_ON_ALARM, flags);
    TEST_ASSERT_NULL(strstr(msg, "fragmented"));
    TEST_ASSERT_NOT_NULL(strstr(msg, "internal RAM low"));

    /* The cockpit's measured trough while esp-sr starts must not alarm. */
    h = HEALTHY;
    h.internal_free = 24 * 1024;
    h.largest_block = 23 * 1024;
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, espos_health_policy_memory(&CFG, &h, msg, sizeof(msg), &flags));

    /* 0 disables a threshold. */
    espos_health_policy_cfg_t off = { 0 };
    h.internal_free = 1;
    h.largest_block = 1;
    h.total_free = 1;
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, espos_health_policy_memory(&off, &h, msg, sizeof(msg), &flags));
}

TEST_CASE("the tick reports lowMemory every time, letting the table dedup", "[health][policy]")
{
    fresh(false);
    tick();
    const report_t *r = last_report("lowMemory");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, r->state);

    F.heap.internal_free = 18 * 1024;
    tick();
    r = last_report("lowMemory");
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, r->state);
    TEST_ASSERT_EQUAL_UINT32(0, r->flags);
    TEST_ASSERT_EQUAL_INT(0, F.restarts);
}

/* The built-in condition takes the same route as a consumer's: through the
 * table, three strikes, a record that says lowMemory. */
TEST_CASE("internal RAM exhaustion restarts through the real table after N ticks", "[health][policy]")
{
    fresh(true);
    F.heap.internal_free = 10 * 1024;
    TEST_ASSERT_EQUAL_UINT32(1, tick());
    TEST_ASSERT_EQUAL_UINT32(2, tick());
    TEST_ASSERT_EQUAL_INT(0, F.restarts);
    TEST_ASSERT_EQUAL_UINT32(3, tick());
    TEST_ASSERT_EQUAL_INT(1, F.restarts);
    TEST_ASSERT_EQUAL_STRING("lowMemory", F.stored.key);

    espos_health_condition_t c;
    TEST_ASSERT_TRUE(espos_health_fatal_alarm(&c));
    TEST_ASSERT_EQUAL_STRING("lowMemory", c.key);
}

TEST_CASE("a memory warning alone never restarts", "[health][policy]")
{
    fresh(true);
    F.heap.internal_free = 15 * 1024; /* below warn, above alarm */
    F.heap.total_free = 30 * 1024;
    for (int i = 0; i < 12; i++) TEST_ASSERT_EQUAL_UINT32(0, tick());
    TEST_ASSERT_EQUAL_INT(0, F.restarts);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, espos_health_worst());
}

/* ------------------------------------------------------- watched tasks */

static int task_a, task_b;

TEST_CASE("a task that keeps kicking is never stalled", "[health][policy]")
{
    fresh(false);
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_policy_watch(&P, &task_a, "ui", 15000));
    for (int i = 0; i < 5; i++) {
        F.now_ms += 5000;
        espos_health_policy_kick(&P, &task_a);
        F.now_ms += 5000;
        espos_health_policy_tick(&P);
        const report_t *r = last_report("taskStalled");
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, r->state);
    }
}

TEST_CASE("a kick stamped after the tick sampled its clock is alive, not a 49-day stall", "[health][policy]")
{
    /* The device tick reads the clock once, then scans the registry; a kick
     * from a frame-rate UI task lands in between with a newer stamp. Seen on
     * the P4 panel as "ui silent for 4294967 s" and strike 2/3. */
    fresh(true);
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_policy_watch(&P, &task_a, "ui", 15000));
    F.now_ms += 5005;
    espos_health_policy_kick(&P, &task_a); /* stamp = t + 5005 */
    F.now_ms -= 5;                         /* the tick's sample: t + 5000 */
    TEST_ASSERT_EQUAL_UINT32(0, espos_health_policy_tick(&P));
    const report_t *r = last_report("taskStalled");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, r->state);
    TEST_ASSERT_EQUAL_INT(0, F.restarts);
}

TEST_CASE("a task that stops kicking is a fatal ALARM naming it", "[health][policy]")
{
    fresh(true);
    espos_health_policy_watch(&P, &task_a, "ui", 15000);
    espos_health_policy_watch(&P, &task_b, "n2k", 60000);
    tick(); /* 10 s: within both limits */
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, last_report("taskStalled")->state);
    espos_health_policy_kick(&P, &task_b);
    TEST_ASSERT_EQUAL_UINT32(1, tick()); /* ui at 20 s > 15 s */
    const report_t *r = last_report("taskStalled");
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, r->state);
    TEST_ASSERT_EQUAL_UINT32(ESPOS_HEALTH_F_REBOOT_ON_ALARM, r->flags);
    TEST_ASSERT_NOT_NULL(strstr(r->message, "ui silent for 20 s"));
    tick();
    TEST_ASSERT_EQUAL_INT(0, F.restarts);
    tick();
    TEST_ASSERT_EQUAL_INT(1, F.restarts);
    TEST_ASSERT_EQUAL_STRING("taskStalled", F.stored.key);
}

TEST_CASE("kicking again clears a stall before it strikes out", "[health][policy]")
{
    fresh(true);
    espos_health_policy_watch(&P, &task_a, "ui", 15000);
    tick();
    TEST_ASSERT_EQUAL_UINT32(1, tick());
    espos_health_policy_kick(&P, &task_a);
    TEST_ASSERT_EQUAL_UINT32(0, tick());
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, last_report("taskStalled")->state);
    TEST_ASSERT_EQUAL_INT(0, F.restarts);
}

TEST_CASE("an unwatched task is forgotten; the registry fills and refuses", "[health][policy]")
{
    fresh(false);
    espos_health_policy_watch(&P, &task_a, "ui", 15000);
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_policy_unwatch(&P, &task_a));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_health_policy_unwatch(&P, &task_a));
    for (int i = 0; i < 10; i++) tick();
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, last_report("taskStalled")->state);

    int tasks[ESPOS_HEALTH_WATCHED_MAX + 1];
    for (int i = 0; i < ESPOS_HEALTH_WATCHED_MAX; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, espos_health_policy_watch(&P, &tasks[i], "t", 1000));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, espos_health_policy_watch(&P, &tasks[ESPOS_HEALTH_WATCHED_MAX], "t", 1000));
    /* Re-watching a known task is an update, not a slot. */
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_policy_watch(&P, &tasks[0], "renamed", 2000));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_health_policy_watch(&P, NULL, "t", 1000));
}

TEST_CASE("taskStalled is not reported before anything is watched", "[health][policy]")
{
    fresh(false);
    tick();
    TEST_ASSERT_NULL(last_report("taskStalled"));
}

/* ------------------------------------------------- the table's flag side */

TEST_CASE("report_ex records flags, exposes them in the snapshot, rejects unknown ones", "[health][flags]")
{
    espos_health_reset();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_health_report_ex("k", ESPOS_HEALTH_WARN, "", 1u << 7));
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_ex("k", ESPOS_HEALTH_ALARM, "m", ESPOS_HEALTH_F_REBOOT_ON_ALARM));
    espos_health_condition_t c[2];
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)espos_health_snapshot(c, 2));
    TEST_ASSERT_EQUAL_UINT32(ESPOS_HEALTH_F_REBOOT_ON_ALARM, c[0].flags);

    /* Plain report() is report_ex() with no flags, and the latest report wins. */
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report("k", ESPOS_HEALTH_ALARM, "m"));
    espos_health_snapshot(c, 2);
    TEST_ASSERT_EQUAL_UINT32(0, c[0].flags);
    TEST_ASSERT_FALSE(espos_health_fatal_alarm(NULL));
}

TEST_CASE("fatal_alarm finds only a flagged ALARM", "[health][flags]")
{
    espos_health_reset();
    espos_health_condition_t c;
    TEST_ASSERT_FALSE(espos_health_fatal_alarm(&c));
    espos_health_report_ex("warnOnly", ESPOS_HEALTH_WARN, "", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
    espos_health_report_ex("plainAlarm", ESPOS_HEALTH_ALARM, "", 0);
    TEST_ASSERT_FALSE(espos_health_fatal_alarm(&c));
    espos_health_report_ex("fatal", ESPOS_HEALTH_ALARM, "boom", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
    TEST_ASSERT_TRUE(espos_health_fatal_alarm(&c));
    TEST_ASSERT_EQUAL_STRING("fatal", c.key);
    TEST_ASSERT_EQUAL_STRING("boom", c.message);
    espos_health_report_ex("fatal", ESPOS_HEALTH_NORMAL, "", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
    TEST_ASSERT_FALSE(espos_health_fatal_alarm(&c));
}

/* A flags-only change is not a change for the sinks. */
static size_t s_flag_sink_calls;
static void flag_sink(const char *key, espos_health_state_t st, const char *msg, void *arg)
{
    (void)key;
    (void)st;
    (void)msg;
    (void)arg;
    s_flag_sink_calls++;
}

TEST_CASE("changing only the flags does not wake the sinks", "[health][flags]")
{
    espos_health_reset();
    s_flag_sink_calls = 0;
    espos_health_add_sink(flag_sink, NULL);
    espos_health_report_ex("k", ESPOS_HEALTH_ALARM, "m", 0);
    espos_health_report_ex("k", ESPOS_HEALTH_ALARM, "m", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_flag_sink_calls);
    TEST_ASSERT_TRUE(espos_health_fatal_alarm(NULL));
}

/* The singleton on the host: no tick and no record, but the calls exist and
 * are idempotent — api_system.c and espos_core link against them. */
TEST_CASE("the device-side entry points exist on the host", "[health][policy]")
{
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_policy_start());
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_policy_start());
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_watch_task("test", 1000));
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_watch_task("test", 2000)); /* same task: an update */
    espos_health_kick();
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_unwatch_task());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_health_unwatch_task());
    espos_health_reset_record_t rec;
    TEST_ASSERT_FALSE(espos_health_last_reset(&rec));
}

/*
 * The message must not carry the live figure.
 *
 * espos_health_report() suppresses a report whose state AND message both match
 * the previous one. A message containing the current byte count never matches,
 * so a condition that simply persists fanned out to every sink on every tick:
 * on an ESP32-C5, whose idle free heap (27-35 KB) sits below the default 40 KB
 * warn floor, that was a SignalK notification built, published and freed every
 * 10 s for the life of the device. The resulting churn fragmented the heap
 * until largest_block fell through its alarm floor and the watchdog rebooted
 * the board every ~9 minutes with 18 KB still free. espOS #124.
 */
TEST_CASE("a persisting condition yields an identical message", "[health][policy]")
{
    char first[ESPOS_HEALTH_MSG_MAX];
    char later[ESPOS_HEALTH_MSG_MAX];
    uint32_t flags = 0;
    espos_health_heap_t h = HEALTHY;

    /* Heap drifting down, still in the same warn band. */
    h.total_free = 39 * 1024;
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN,
                      espos_health_policy_memory(&CFG, &h, first, sizeof(first), &flags));
    for (unsigned drop = 1; drop <= 10; drop++) {
        h.total_free = 39 * 1024 - drop * 264;   /* the measured per-tick churn */
        TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN,
                          espos_health_policy_memory(&CFG, &h, later, sizeof(later), &flags));
        TEST_ASSERT_EQUAL_STRING(first, later);
    }

    /* Same for each of the other three bands. */
    h = HEALTHY;
    h.internal_free = 19 * 1024;
    espos_health_policy_memory(&CFG, &h, first, sizeof(first), &flags);
    h.internal_free = 17 * 1024;
    espos_health_policy_memory(&CFG, &h, later, sizeof(later), &flags);
    TEST_ASSERT_EQUAL_STRING(first, later);

    h = HEALTHY;
    h.internal_free = 11 * 1024;
    espos_health_policy_memory(&CFG, &h, first, sizeof(first), &flags);
    h.internal_free = 9 * 1024;
    espos_health_policy_memory(&CFG, &h, later, sizeof(later), &flags);
    TEST_ASSERT_EQUAL_STRING(first, later);

    h = HEALTHY;
    h.largest_block = 7 * 1024;
    espos_health_policy_memory(&CFG, &h, first, sizeof(first), &flags);
    h.largest_block = 6 * 1024;
    espos_health_policy_memory(&CFG, &h, later, sizeof(later), &flags);
    TEST_ASSERT_EQUAL_STRING(first, later);

    /* But crossing INTO a worse band must still change it, or the escalation
     * a sink needs to see would be swallowed along with the noise. */
    h = HEALTHY;
    h.total_free = 39 * 1024;
    espos_health_policy_memory(&CFG, &h, first, sizeof(first), &flags);
    h = HEALTHY;
    h.internal_free = 11 * 1024;
    espos_health_policy_memory(&CFG, &h, later, sizeof(later), &flags);
    TEST_ASSERT_TRUE(strcmp(first, later) != 0);
}

/* ---------------------------------------------------------------- memoryTrough */

TEST_CASE("a healthy low-water mark reports nothing", "[health_policy]")
{
    char msg[ESPOS_HEALTH_MSG_MAX];
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL,
                      espos_health_policy_trough(&CFG, &HEALTHY, msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("", msg);
}

TEST_CASE("a device that dipped warns even while it now looks healthy", "[health_policy]")
{
    /* The case the live checks cannot see, and the reason this exists: measured
     * on an ESP32-C5 at 27604 B free with a low-water mark of 4924 B after 27
     * hours (espOS #129). Every tick saw a healthy board. */
    espos_health_heap_t h = HEALTHY;             /* everything live is comfortable */
    h.internal_min = 4924;                      /* ...but it was not, earlier */
    char msg[ESPOS_HEALTH_MSG_MAX];
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN,
                      espos_health_policy_trough(&CFG, &h, msg, sizeof(msg)));
    TEST_ASSERT_NOT_NULL(strstr(msg, "since boot"));

    /* The live check must be NORMAL on the same reading, or this test would pass
     * because both fired and prove nothing about the trough being the only signal.
     *
     * Note what this costs to arrange: the real C5 also sat at 27604 B total free,
     * below the 40 KB heap_warn, so on that board lowMemory warns too. The trough
     * is the only signal where a device dips transiently while its STEADY state is
     * genuinely healthy -- which is the general case, and the C5 is the harsher
     * one where both are true. */
    uint32_t flags = 0;
    char live[ESPOS_HEALTH_MSG_MAX];
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL,
                      espos_health_policy_memory(&CFG, &h, live, sizeof(live), &flags));
}

TEST_CASE("the tick reports memoryTrough, non-fatally, while lowMemory stays normal",
          "[health][policy]")
{
    /* Everything above tests espos_health_policy_trough() directly, which proves the
     * decision and nothing about the WIRING: a condition computed correctly and never
     * handed to the report port is invisible on a device. This drives the real tick.
     *
     * It also pins the pair. The whole argument for a separate condition is that the
     * trough can fire while the live check does not, so a test where both warn would
     * demonstrate nothing. */
    fresh(false);
    tick();
    /* Reported every tick even when there is nothing wrong -- the tick states the
     * condition and lets the table dedup (espOS #124), so NORMAL here, not absent. */
    const report_t *quiet = last_report("memoryTrough");
    TEST_ASSERT_NOT_NULL(quiet);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, quiet->state);

    F.heap.internal_min = 4924; /* the measured C5 mark; live figures stay comfortable */
    tick();

    const report_t *t = last_report("memoryTrough");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, t->state);
    /* Never fatal, by construction: a mark does not recover within a boot, so a fatal
     * flag here would restart the device on every boot's first dip, for ever. */
    TEST_ASSERT_EQUAL_UINT32(0, t->flags);

    const report_t *l = last_report("lowMemory");
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, l->state);

    TEST_ASSERT_EQUAL_INT(0, F.restarts);
    TEST_ASSERT_EQUAL_INT(0, F.stores);
}

TEST_CASE("the trough message names the threshold, not the mark", "[health_policy]")
{
    /* espos_health_report() suppresses a repeat only when state AND message match,
     * so a message carrying the live figure fans out to every sink every tick --
     * the churn that fragmented a board into a reboot loop (espOS #124). Two
     * different marks must produce the SAME message. */
    espos_health_heap_t a = HEALTHY, b = HEALTHY;
    a.internal_min = 4924;
    b.internal_min = 1024;
    char ma[ESPOS_HEALTH_MSG_MAX], mb[ESPOS_HEALTH_MSG_MAX];
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, espos_health_policy_trough(&CFG, &a, ma, sizeof(ma)));
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, espos_health_policy_trough(&CFG, &b, mb, sizeof(mb)));
    TEST_ASSERT_EQUAL_STRING(ma, mb);
    TEST_ASSERT_NULL(strstr(ma, "4924"));
}

TEST_CASE("the trough check is never fatal", "[health_policy]")
{
    /* A low-water mark does not recover within a boot, so a fatal condition on it
     * would reboot, see the same mark on the next boot's first dip, and reboot
     * again. The signature has no flags argument at all -- this test pins the
     * report site, which passes 0. */
    espos_health_heap_t h = HEALTHY;
    h.internal_min = 512;                       /* as bad as it gets */
    char msg[ESPOS_HEALTH_MSG_MAX];
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN,
                      espos_health_policy_trough(&CFG, &h, msg, sizeof(msg)));
}

TEST_CASE("a zero trough threshold disables the check", "[health_policy]")
{
    espos_health_policy_cfg_t c = CFG;
    c.internal_trough_warn_kb = 0;
    espos_health_heap_t h = HEALTHY;
    h.internal_min = 128;
    char msg[ESPOS_HEALTH_MSG_MAX];
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL,
                      espos_health_policy_trough(&c, &h, msg, sizeof(msg)));
}

TEST_CASE("every built-in condition fits the table and reports", "[health_policy]")
{
    /* espOS raises five keys of its own. A table too small to hold them drops one
     * with ESP_ERR_NO_MEM (espos_health.c) -- and if the dropped one is fatal, the
     * device stops restarting on a condition that should restart it. The Kconfig
     * floor is 5 for that reason; this pins that the built-ins really are five and
     * that each one registers rather than being rejected. */
    static const char *const builtins[] = {
        "lowMemory",
        "memoryTrough",
        "taskStalled",
        "netDown",
        "skLinkStalled",
    };
    fresh(false);
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
                                  espos_health_report(builtins[i], ESPOS_HEALTH_WARN, "x"),
                                  builtins[i]);
    }
    /* And they are distinct keys, not one key reported five times. */
    espos_health_condition_t out[8];
    size_t n = espos_health_snapshot(out, 8);
    TEST_ASSERT_EQUAL(5, n);
}
