/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_health: the condition table and the sink registry.
 *
 * The two properties everything else leans on are that a condition is a level
 * (re-report freely, sinks hear about changes only) and that a sink which
 * registers late still learns the current state. Get the first wrong and a
 * polling caller floods the SignalK server; get the second wrong and a
 * condition raised during boot is invisible for as long as it lasts, which is
 * precisely the case espos_health exists for.
 */
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "espos_health.h"
#include "unity.h"

/* ------------------------------------------------------------- recorder */

#define MAX_CALLS 16

typedef struct {
    char key[ESPOS_HEALTH_KEY_MAX];
    espos_health_state_t state;
    char message[ESPOS_HEALTH_MSG_MAX];
} call_t;

static call_t s_calls[MAX_CALLS];
static size_t s_n;

static void recorder(const char *key, espos_health_state_t state, const char *message, void *arg)
{
    (void)arg;
    if (s_n >= MAX_CALLS) return;
    snprintf(s_calls[s_n].key, sizeof(s_calls[s_n].key), "%s", key);
    s_calls[s_n].state = state;
    snprintf(s_calls[s_n].message, sizeof(s_calls[s_n].message), "%s", message ? message : "");
    s_n++;
}

/* A second sink with its own identity, to prove both are called. */
static size_t s_other_n;
static void other_sink(const char *key, espos_health_state_t state, const char *message, void *arg)
{
    (void)key;
    (void)state;
    (void)message;
    (void)arg;
    s_other_n++;
}

/* Sleep for at least `ms` of real time.
 *
 * NOT a bare usleep(): the FreeRTOS simulator delivers its tick as a SIGNAL, so
 * EINTR cuts the sleep short. Measured in this binary, usleep(20000) returns -1
 * with EINTR after 8-10 ms every time, and on CI it woke earlier still -- under the
 * 1 ms ttl a test was arming -- so the drill was not due and expire() correctly
 * returned false while the test insisted it should not have (espOS #149).
 *
 * Looping on the clock cannot exit early however often the signal arrives, which
 * makes the test depend on elapsed time rather than on a syscall running to
 * completion. */
static void sleep_at_least_ms(unsigned ms)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        /* The delta in nanoseconds FIRST, then one truncation. Truncating each
         * timestamp to milliseconds and subtracting overstates the gap by up to a
         * millisecond -- start at 1.999 ms and wake at 21.000 ms and it reports 20
         * for a real 19 -- which would let this helper return early, the one thing
         * it exists to prevent. */
        const int64_t elapsed = ((int64_t)(now.tv_sec - start.tv_sec) * 1000000000LL + (int64_t)now.tv_nsec -
                                 (int64_t)start.tv_nsec) /
                                1000000LL;
        if (elapsed >= (int64_t)ms) return;
        usleep(1000);
    }
}

static void fresh(void)
{
    espos_health_reset();
    memset(s_calls, 0, sizeof(s_calls));
    s_n = 0;
    s_other_n = 0;
}

/* --------------------------------------------------------------- tests */

TEST_CASE("a sink hears a raised condition", "[health]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_add_sink(recorder, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report("n2kBus", ESPOS_HEALTH_WARN, "no frames for 30 s"));

    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_n);
    TEST_ASSERT_EQUAL_STRING("n2kBus", s_calls[0].key);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, s_calls[0].state);
    TEST_ASSERT_EQUAL_STRING("no frames for 30 s", s_calls[0].message);
}

/* The property a polling caller depends on. */
TEST_CASE("re-reporting an unchanged condition stays quiet", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, espos_health_report("lowMemory", ESPOS_HEALTH_WARN, "18 KB free"));
    }
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_n);
}

TEST_CASE("a changed message is a change", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    espos_health_report("lowMemory", ESPOS_HEALTH_WARN, "18 KB free");
    espos_health_report("lowMemory", ESPOS_HEALTH_WARN, "12 KB free");
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)s_n);
    TEST_ASSERT_EQUAL_STRING("12 KB free", s_calls[1].message);
}

TEST_CASE("clearing is delivered like any other change", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    espos_health_report("wakeService", ESPOS_HEALTH_WARN, "unreachable");
    espos_health_report("wakeService", ESPOS_HEALTH_NORMAL, "");
    espos_health_report("wakeService", ESPOS_HEALTH_NORMAL, "");   /* still quiet */

    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)s_n);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, s_calls[1].state);
}

/* A first report of NORMAL is not a no-op: after a reboot the far end may
 * still hold an alert this device raised before it restarted, and this is the
 * clear that retires it. */
TEST_CASE("a first NORMAL for an unknown key is still delivered", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    espos_health_report("staleAlarm", ESPOS_HEALTH_NORMAL, "");
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_n);
    TEST_ASSERT_EQUAL_STRING("staleAlarm", s_calls[0].key);
}

/* espos_sk registers its sink only once the stream is up, long after
 * espos_voice may have reported a dead wake service. */
TEST_CASE("a late sink is told the current state on registration", "[health]")
{
    fresh();
    espos_health_report("wakeService", ESPOS_HEALTH_WARN, "unreachable");
    espos_health_report("lowMemory", ESPOS_HEALTH_NORMAL, "");
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)s_n);   /* nobody listening yet */

    TEST_ASSERT_EQUAL(ESP_OK, espos_health_add_sink(recorder, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)s_n);
    TEST_ASSERT_EQUAL_STRING("wakeService", s_calls[0].key);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, s_calls[0].state);
    TEST_ASSERT_EQUAL_STRING("lowMemory", s_calls[1].key);
}

/* espos_sk restates every condition on each stream connect, because the
 * server may have lost a clear it was sent while the stream was down. */
TEST_CASE("replay restates every condition, NORMAL included", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    espos_health_report("lowMemory", ESPOS_HEALTH_WARN, "heap low");
    espos_health_report("lowMemory", ESPOS_HEALTH_NORMAL, "");
    espos_health_report("n2kBus", ESPOS_HEALTH_ALARM, "bus off");
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)s_n);

    TEST_ASSERT_EQUAL(ESP_OK, espos_health_replay(recorder, NULL));
    TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)s_n);
    TEST_ASSERT_EQUAL_STRING("lowMemory", s_calls[3].key);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, s_calls[3].state);
    TEST_ASSERT_EQUAL_STRING("n2kBus", s_calls[4].key);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, s_calls[4].state);
    TEST_ASSERT_EQUAL_STRING("bus off", s_calls[4].message);

    /* Replaying does not count as a change: the next identical report stays quiet. */
    espos_health_report("n2kBus", ESPOS_HEALTH_ALARM, "bus off");
    TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)s_n);
}

TEST_CASE("replay reaches only the sink it is given", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    espos_health_add_sink(other_sink, NULL);
    espos_health_report("n2kBus", ESPOS_HEALTH_WARN, "quiet");
    espos_health_replay(recorder, NULL);
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)s_n);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_other_n);
}

/* Not registered: it changes the condition it is being told about, the way a
 * report from another task can land between replay's copy and its sink call. */
static size_t s_racing_n;
static void racing_sink(const char *key, espos_health_state_t state, const char *message, void *arg)
{
    (void)arg;
    recorder(key, state, message, NULL);
    if (s_racing_n++ == 0) {
        espos_health_report("n2kBus", ESPOS_HEALTH_NORMAL, "");
    }
}

TEST_CASE("a condition that changes during replay is sent again, newest last", "[health]")
{
    fresh();
    s_racing_n = 0;
    espos_health_report("n2kBus", ESPOS_HEALTH_ALARM, "bus off");
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_replay(racing_sink, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)s_n);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, s_calls[0].state);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, s_calls[1].state);
}

/* Changes the condition on every call, so the replay never sees it settle. */
static size_t s_flapping_n;
static void flapping_sink(const char *key, espos_health_state_t state, const char *message, void *arg)
{
    (void)arg;
    recorder(key, state, message, NULL);
    s_flapping_n++;
    espos_health_report("n2kBus", (s_flapping_n % 2) ? ESPOS_HEALTH_NORMAL : ESPOS_HEALTH_ALARM, "");
}

TEST_CASE("a replay that never settles says so instead of claiming success", "[health]")
{
    fresh();
    s_flapping_n = 0;
    espos_health_report("n2kBus", ESPOS_HEALTH_ALARM, "");
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, espos_health_replay(flapping_sink, NULL));
    TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)s_flapping_n);
}

TEST_CASE("replay refuses a NULL sink", "[health]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_health_replay(NULL, NULL));
}

TEST_CASE("every registered sink is called", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    espos_health_add_sink(other_sink, NULL);
    espos_health_report("n2kBus", ESPOS_HEALTH_ALARM, "bus off");
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_n);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_other_n);
}

TEST_CASE("the same sink cannot register twice", "[health]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_add_sink(recorder, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_health_add_sink(recorder, NULL));

    /* Same function, different arg, is a different sink. */
    int ctx = 1;
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_add_sink(recorder, &ctx));
    espos_health_report("x", ESPOS_HEALTH_WARN, "");
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)s_n);
}

TEST_CASE("a removed sink hears nothing further", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    espos_health_report("x", ESPOS_HEALTH_WARN, "one");
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_remove_sink(recorder, NULL));
    espos_health_report("x", ESPOS_HEALTH_WARN, "two");
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)s_n);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_health_remove_sink(recorder, NULL));
}

/* Truncation would be worse than an error: a clipped key never matches on the
 * next report, so every call would consume another slot. */
TEST_CASE("an over-long key or message is rejected, not clipped", "[health]")
{
    fresh();
    char long_key[ESPOS_HEALTH_KEY_MAX + 8];
    memset(long_key, 'k', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, espos_health_report(long_key, ESPOS_HEALTH_WARN, ""));

    char long_msg[ESPOS_HEALTH_MSG_MAX + 8];
    memset(long_msg, 'm', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, espos_health_report("k", ESPOS_HEALTH_WARN, long_msg));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_health_report("", ESPOS_HEALTH_WARN, ""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_health_report(NULL, ESPOS_HEALTH_WARN, ""));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)espos_health_snapshot(NULL, 0));
}

TEST_CASE("a NULL message reads back as empty", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report("x", ESPOS_HEALTH_WARN, NULL));
    TEST_ASSERT_EQUAL_STRING("", s_calls[0].message);
}

TEST_CASE("the table fills and then refuses further keys", "[health]")
{
    fresh();
    char key[ESPOS_HEALTH_KEY_MAX];
    for (int i = 0; i < CONFIG_ESPOS_HEALTH_MAX_CONDITIONS; i++) {
        snprintf(key, sizeof(key), "k%d", i);
        TEST_ASSERT_EQUAL(ESP_OK, espos_health_report(key, ESPOS_HEALTH_WARN, ""));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, espos_health_report("overflow", ESPOS_HEALTH_WARN, ""));

    /* A key already in the table still works once it is full. */
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report("k0", ESPOS_HEALTH_NORMAL, ""));
    TEST_ASSERT_EQUAL_UINT32(CONFIG_ESPOS_HEALTH_MAX_CONDITIONS,
                             (uint32_t)espos_health_snapshot(NULL, 0));
}

TEST_CASE("snapshot copies at most what it is given room for", "[health]")
{
    fresh();
    espos_health_report("a", ESPOS_HEALTH_WARN, "one");
    espos_health_report("b", ESPOS_HEALTH_ALARM, "two");

    espos_health_condition_t got[1];
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)espos_health_snapshot(got, 1));
    TEST_ASSERT_EQUAL_STRING("a", got[0].key);
    TEST_ASSERT_EQUAL_STRING("one", got[0].message);
}

/* What a single status LED wants to know. */
TEST_CASE("worst reports the highest state currently held", "[health]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, espos_health_worst());

    espos_health_report("a", ESPOS_HEALTH_WARN, "");
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, espos_health_worst());

    espos_health_report("b", ESPOS_HEALTH_ALARM, "");
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, espos_health_worst());

    espos_health_report("b", ESPOS_HEALTH_NORMAL, "");
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_WARN, espos_health_worst());

    espos_health_report("a", ESPOS_HEALTH_NORMAL, "");
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, espos_health_worst());
}

TEST_CASE("state_str names the three levels", "[health]")
{
    TEST_ASSERT_EQUAL_STRING("normal", espos_health_state_str(ESPOS_HEALTH_NORMAL));
    TEST_ASSERT_EQUAL_STRING("warn", espos_health_state_str(ESPOS_HEALTH_WARN));
    TEST_ASSERT_EQUAL_STRING("alarm", espos_health_state_str(ESPOS_HEALTH_ALARM));
}

/* ------------------------------------------------- synthetic conditions (#137) */

/* Find a condition in the table by key, or NULL. */
static const espos_health_condition_t *find_cond(espos_health_condition_t *buf, size_t n, const char *key)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(buf[i].key, key) == 0) return &buf[i];
    }
    return NULL;
}

TEST_CASE("a synthetic condition needs the reserved prefix and a name after it", "[health]")
{
    fresh();
    /* Without the prefix a drill could impersonate a real fault -- and worse,
     * could collide with a consumer's own key and clobber its state. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      espos_health_report_test("relayExpander", ESPOS_HEALTH_ALARM, "x", 1000));
    /* The prefix alone names nothing; "test." as a condition reads as a bug. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_health_report_test("test.", ESPOS_HEALTH_ALARM, "x", 1000));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_health_report_test(NULL, ESPOS_HEALTH_ALARM, "x", 1000));
    /* Nothing reached the table. */
    TEST_ASSERT_EQUAL(0, espos_health_snapshot(NULL, 0));

    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.buzzer", ESPOS_HEALTH_ALARM, "drill", 1000));
    TEST_ASSERT_EQUAL(1, espos_health_snapshot(NULL, 0));
}

TEST_CASE("a synthetic ALARM cannot arm the reboot path", "[health]")
{
    fresh();
    /* The whole point of #137: every built-in alarm carries
     * ESPOS_HEALTH_F_REBOOT_ON_ALARM, so triggering one to watch a buzzer ends
     * the observation. A drill must reach the sinks and not the restart. */
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.buzzer", ESPOS_HEALTH_ALARM, "drill", 60000));

    espos_health_condition_t buf[8];
    size_t n = espos_health_snapshot(buf, 8);
    const espos_health_condition_t *c = find_cond(buf, n, "test.buzzer");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, c->state);
    TEST_ASSERT_EQUAL_UINT32(0, c->flags);

    /* Visible to a status LED as the worst state, yet not fatal -- the pair of
     * facts a hardware test needs in order to assert anything. */
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, espos_health_worst());
    TEST_ASSERT_FALSE(espos_health_fatal_alarm(NULL));
}

TEST_CASE("a synthetic condition reaches the sinks like a real one", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.buzzer", ESPOS_HEALTH_ALARM, "drill", 60000));
    /* A buzzer hangs off a sink, so this is the property the endpoint exists to
     * exercise. A sink deliberately cannot tell a drill from the real thing. */
    TEST_ASSERT_EQUAL(1, s_n);
    TEST_ASSERT_EQUAL_STRING("test.buzzer", s_calls[0].key);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, s_calls[0].state);
    TEST_ASSERT_EQUAL_STRING("drill", s_calls[0].message);
}

TEST_CASE("raising a second drill clears the first", "[health]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.one", ESPOS_HEALTH_ALARM, "a", 60000));
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.two", ESPOS_HEALTH_ALARM, "b", 60000));

    espos_health_condition_t buf[8];
    size_t n = espos_health_snapshot(buf, 8);
    /* One at a time, so a script that loops cannot leave a trail of fake alarms
     * on a live boat. Both keys keep their table slots -- that is the cost of a
     * new key and why a test should reuse one. */
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, find_cond(buf, n, "test.one")->state);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, find_cond(buf, n, "test.two")->state);
}

TEST_CASE("a drill's ttl must be present and sane when raising", "[health]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_health_report_test("test.x", ESPOS_HEALTH_ALARM, "", 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      espos_health_report_test("test.x", ESPOS_HEALTH_ALARM, "", ESPOS_HEALTH_TEST_TTL_MAX_MS + 1));
    TEST_ASSERT_EQUAL(ESP_OK,
                      espos_health_report_test("test.x", ESPOS_HEALTH_ALARM, "", ESPOS_HEALTH_TEST_TTL_MAX_MS));
    /* Clearing needs no ttl: it is the normal way to end a drill. */
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.x", ESPOS_HEALTH_NORMAL, "", 0));
}

TEST_CASE("the ttl clears a drill, and only once it is due", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.buzzer", ESPOS_HEALTH_ALARM, "drill", 60000));
    /* Not due for a minute: the backstop must not cut an observation short. */
    TEST_ASSERT_FALSE(espos_health_test_expire());
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, espos_health_worst());

    /* Due almost immediately. 1 ms rather than a faked clock because the
     * deadline is read from the component's own port; the wait is real but
     * bounded, and what is being checked is the arithmetic, not the delay. */
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.buzzer", ESPOS_HEALTH_ALARM, "drill", 1));
    sleep_at_least_ms(20);
    TEST_ASSERT_TRUE(espos_health_test_expire());
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, espos_health_worst());
    /* Idempotent: nothing left armed, so the next policy tick -- or the next GET
     * that drives expiry -- does not re-report a clear that already happened. */
    TEST_ASSERT_FALSE(espos_health_test_expire());

    /* The clear reached the sinks, which is how a buzzer stops. */
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, s_calls[s_n - 1].state);
    TEST_ASSERT_EQUAL_STRING("test.buzzer", s_calls[s_n - 1].key);
}

TEST_CASE("clearing a drill disarms its ttl", "[health]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.buzzer", ESPOS_HEALTH_ALARM, "drill", 1));
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.buzzer", ESPOS_HEALTH_NORMAL, "", 0));
    sleep_at_least_ms(20);
    /* Nothing to expire: an explicit clear is the normal mechanism, and a stale
     * deadline firing afterwards would re-report NORMAL to every sink for no
     * reason. */
    TEST_ASSERT_FALSE(espos_health_test_expire());
}

TEST_CASE("a rejected drill leaves the running one alone", "[health]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.running", ESPOS_HEALTH_ALARM, "watch me", 60000));

    /* The invariant: a rejected report changes nothing. The active drill stays
     * raised, keeps its message, and keeps its expiry tracking -- a caller's bad
     * request must not end an observation somebody else is in the middle of.
     * Every rejection path is tried while a drill is live. */
    char too_long_msg[ESPOS_HEALTH_MSG_MAX + 1];
    memset(too_long_msg, 'm', sizeof(too_long_msg) - 1);
    too_long_msg[sizeof(too_long_msg) - 1] = '\0';
    char too_long_key[ESPOS_HEALTH_KEY_MAX + 8];
    snprintf(too_long_key, sizeof(too_long_key), "%skkkkkkkkkkkkkkkkkkkkkkkk", ESPOS_HEALTH_TEST_PREFIX);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_health_report_test("nope", ESPOS_HEALTH_ALARM, "", 1000));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_health_report_test("test.other", ESPOS_HEALTH_ALARM, "", 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      espos_health_report_test("test.other", ESPOS_HEALTH_ALARM, too_long_msg, 1000));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      espos_health_report_test(too_long_key, ESPOS_HEALTH_ALARM, "", 1000));

    /* Raised, unchanged, and still ARMED. The arming matters as much as the state:
     * a drill raised with nothing tracking it is an alarm no ttl can ever clear. */
    espos_health_condition_t buf[8];
    size_t n = espos_health_snapshot(buf, 8);
    const espos_health_condition_t *c = find_cond(buf, n, "test.running");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, c->state);
    TEST_ASSERT_EQUAL_STRING("watch me", c->message);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, espos_health_worst());
    /* And its ttl is intact, so the backstop still applies. */
    TEST_ASSERT_FALSE(espos_health_test_expire());
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, espos_health_worst());
}

TEST_CASE("a drill refused for want of a table slot rolls the arming back", "[health]")
{
    fresh();
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.running", ESPOS_HEALTH_ALARM, "watch me", 60000));

    /* Fill the condition table so the next NEW key cannot be admitted: the one
     * failure a well-formed request still meets, and so the only one that can
     * exercise the rollback rather than an early return. */
    char k[ESPOS_HEALTH_KEY_MAX];
    esp_err_t fill = ESP_OK;
    for (int i = 0; fill == ESP_OK && i < CONFIG_ESPOS_HEALTH_MAX_CONDITIONS + 2; i++) {
        snprintf(k, sizeof(k), "filler%d", i);
        fill = espos_health_report(k, ESPOS_HEALTH_NORMAL, "");
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, fill);

    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, espos_health_report_test("test.other", ESPOS_HEALTH_ALARM, "", 60000));

    /* The running drill survived, raised and still tracked. */
    espos_health_condition_t buf[CONFIG_ESPOS_HEALTH_MAX_CONDITIONS];
    size_t n = espos_health_snapshot(buf, sizeof(buf) / sizeof(buf[0]));
    const espos_health_condition_t *c = find_cond(buf, n, "test.running");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, c->state);
    /* Armed: not due yet, so expire leaves it alone rather than finding nothing
     * to track. A stale arming would have made this a no-op for the wrong reason,
     * so the drill is then cleared explicitly and expire checked again. */
    TEST_ASSERT_FALSE(espos_health_test_expire());
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_ALARM, espos_health_worst());
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.running", ESPOS_HEALTH_NORMAL, "", 0));
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, espos_health_worst());
}

TEST_CASE("the longest legal key and message are accepted", "[health]")
{
    fresh();
    /* The other side of the bound the test above rejects: KEY_MAX and MSG_MAX
     * are sizes including the NUL, so one less is legal. Without this the size
     * checks would pass just as well if they refused everything. */
    char key[ESPOS_HEALTH_KEY_MAX];
    memset(key, 'k', sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';
    memcpy(key, ESPOS_HEALTH_TEST_PREFIX, strlen(ESPOS_HEALTH_TEST_PREFIX));
    char msg[ESPOS_HEALTH_MSG_MAX];
    memset(msg, 'm', sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = '\0';

    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test(key, ESPOS_HEALTH_WARN, msg, 1000));
    espos_health_condition_t buf[8];
    size_t n = espos_health_snapshot(buf, 8);
    const espos_health_condition_t *c = find_cond(buf, n, key);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING(msg, c->message);
}

TEST_CASE("expire clears the drill only once the report has gone out", "[health]")
{
    fresh();
    espos_health_add_sink(recorder, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, espos_health_report_test("test.buzzer", ESPOS_HEALTH_ALARM, "drill", 1));
    sleep_at_least_ms(20);

    /* The invariant: the tracking is dropped only after the clearing report
     * succeeds. Were it dropped first, a failed report would leave the drill
     * raised and unowned, and no later pass would retry it. */
    TEST_ASSERT_TRUE(espos_health_test_expire());
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, espos_health_worst());
    TEST_ASSERT_EQUAL(ESPOS_HEALTH_NORMAL, s_calls[s_n - 1].state);
    TEST_ASSERT_EQUAL_STRING("test.buzzer", s_calls[s_n - 1].key);

    /* Dropped now, so a second pass has nothing to do and does not re-report. */
    const size_t calls = s_n;
    TEST_ASSERT_FALSE(espos_health_test_expire());
    TEST_ASSERT_EQUAL(calls, s_n);
}

TEST_CASE("expire is a no-op when no drill was ever raised", "[health]")
{
    fresh();
    TEST_ASSERT_FALSE(espos_health_test_expire());
    TEST_ASSERT_EQUAL(0, espos_health_snapshot(NULL, 0));
}
