# Device health

`espos_health` is where a component says what is wrong with the device, and
something else decides what to do about it. Two halves: a **condition table**
with sinks (this section), and a **watchdog policy** that acts on the table
([below](#the-watchdog-policy)) so no firmware hand-rolls one again.

```c
#include "espos_health.h"

/* Level-triggered: call it as often as you like. */
if (!bus_answering) {
    espos_health_report("n2kBus", ESPOS_HEALTH_WARN, "no frames for 30 s");
} else {
    espos_health_report("n2kBus", ESPOS_HEALTH_NORMAL, "");
}
```

A condition is a **level, not an event**. Reporting the same state and message
twice calls the sinks once, so the natural shape is to re-report on every poll
of whatever you are watching rather than to track edges yourself.
`ESPOS_HEALTH_NORMAL` clears the condition and is delivered like any other
change — it is what retires an alert raised before the last reboot.

The key (`"n2kBus"`, `"wakeService"`, `"lowMemory"`) is an identifier, not a
sentence: it ends up in a SignalK path and is what a rule or a dashboard keys
on. The message is the human-readable half and may change without
re-notifying. Both are rejected, not truncated, when too long — a clipped key
would never match on the next call.

## Sinks

Anything may consume conditions:

```c
static void led_sink(const char *key, espos_health_state_t state,
                     const char *message, void *arg)
{
    gpio_set_level(STATUS_LED, espos_health_worst() != ESPOS_HEALTH_NORMAL);
}

espos_health_add_sink(led_sink, NULL);
```

Registering replays every condition recorded so far, so a sink that comes up
late still learns the current state instead of waiting for the next change.
Sinks run on the reporting task with no lock held: report from inside one if
you want, but do not block for long.

`espos_sk` registers a sink of its own when `ESPOS_SK_NOTIFICATIONS` is on,
publishing each condition to `notifications.espos.<label>.<key>` with
`state`/`message` and a `visual` method. `espos_sk_notify()` is still there and
still works — it forwards to `espos_health_report()`, and is the right call
only when you specifically want the report refused if SignalK is not running.

## Why it is not part of espos_sk

Reporting a fault is a core concern; SignalK is one sink for it. When the only
way to raise a condition was `espos_sk_notify()`, `espos_voice` — a Wyoming
satellite that has nothing to do with SignalK — had to depend on the whole
SignalK stack for its one notification, and a firmware built without SignalK
could not report anything at all. One optional component depending on another
for a core concern generalises badly: the N2K gateway wants to report a silent
bus, an application wants to report its sensor, and each such edge drags the
SignalK stack behind it.

## The watchdog policy

A device on a boat has to look after itself. Before this existed every
firmware wrote its own health check — the cockpit panel's was 95 lines, and it
shipped a bug the policy is designed around: it treated "WiFi disconnected" as
fatal, so while the network was marginal the panel rebooted every ~90 s and
the user could barely interact between restarts. The policy exists so that
mistake is made once, here, and not again.

`espos_start()` arms it (`espos_start_opts_t.health_watchdog`, default true,
also gated by `CONFIG_ESPOS_CORE_HEALTH_WATCHDOG`); by hand it is
`espos_health_policy_start()`. Every **tick** (`CONFIG_ESPOS_HEALTH_POLICY_TICK_S`,
10 s) it:

1. reads the heap and raises or clears **`lowMemory`** (what is free *now*) and
   **`memoryTrough`** (how little was free at the worst point since boot);
2. checks the watched tasks and raises or clears **`taskStalled`**;
3. asks the table whether any condition raised with
   `ESPOS_HEALTH_F_REBOOT_ON_ALARM` is in `ALARM`. If so that is a **strike**;
   if not, the count is zero again.

After `CONFIG_ESPOS_HEALTH_POLICY_STRIKES` (3) consecutive strikes — 30 s of a
sustained fatal condition — it writes the [reset record](#the-reset-record)
and restarts. Everything else is a rule you can rely on:

* A **`WARN` never restarts** the device, flagged or not.
* An **`ALARM` without the flag never restarts** the device. `espos_health_report()`
  cannot set the flag; a condition is fatal only because the code that raised
  it said so with `espos_health_report_ex()`.
* **One clean tick resets the count.** A condition that flaps never adds up to
  a restart; only one that is held does.
* **Loss of WiFi is never a reason to restart.** `espos_core` reports
  `netDown` as a `WARN` on `ESPOS_EVENT_NETWORK_DOWN` and clears it on
  `NETWORK_UP`, and does so without the fatal flag by construction: a router
  reboot, an access-point roam or a trip out of range are normal, `espos_wifi`
  reconnects by itself, and a restart would throw away the UI, every socket
  and any unsaved state to fix nothing. The link that only *looks* connected
  is a different condition — see `skLinkStalled`.

The policy is pure C (`espos_health_policy.h`) behind an injected port —
clock, heap, table, record store, restart — in the same shape as the WiFi and
token state machines, so `test/host/espos_health_test` drives it with a fake
port: exactly N strikes, never on WARN, never on an unflagged ALARM, recovery
resets, record contents.

### Built-in conditions

| Key | Raised by | State | Fatal | When |
|---|---|---|---|---|
| `lowMemory` | the policy tick | `WARN` | no | total free heap below `CONFIG_ESPOS_HEALTH_HEAP_WARN_KB` (40) or free internal RAM below `CONFIG_ESPOS_HEALTH_INTERNAL_WARN_KB` (20) |
| `lowMemory` | the policy tick | `ALARM` | **yes** | free internal RAM below `CONFIG_ESPOS_HEALTH_INTERNAL_ALARM_KB` (12) or the largest free internal block below `CONFIG_ESPOS_HEALTH_LARGEST_BLOCK_ALARM_KB` (8) |
| `memoryTrough` | the policy tick | `WARN` | no (never) | the low-water mark of free internal RAM since boot is below `CONFIG_ESPOS_HEALTH_INTERNAL_TROUGH_WARN_KB` (10) |
| `taskStalled` | the policy tick | `ALARM` | **yes** | a task registered with `espos_health_watch_task()` has not called `espos_health_kick()` for its timeout |
| `netDown` | `espos_core` | `WARN` | no | `ESPOS_EVENT_NETWORK_DOWN`; cleared on `NETWORK_UP` |
| `coprocessorStale` | `espos_wifi`, on the first co-processor heartbeat | `WARN` | no | `esp_hosted`'s own compatibility verdict (`eh_host_mcu_transport_verify_fw_compat()`) says the co-processor is behind this build, which `esp_hosted` warns causes RPC timeouts. Its verdict, not a comparison espOS makes, so the two cannot disagree. Reported once per boot; `NORMAL` whenever the host is **not** newer, which covers equal versions, a patch-level difference, and a host that is itself behind -- a different problem, and not this condition's. Hosted targets only ([hardware.md](hardware.md#updating-the-c6-co-processor-firmware)) |
| `skLinkStalled` | `espos_sk` | `ALARM` | **yes** | WiFi reports connected, the stream has worked once this boot, yet it has been down for `sk.stall_s` (300 s, min 60) |

### `lowMemory` and `memoryTrough` answer different questions

`lowMemory` reads the free size on the tick. `memoryTrough` reads the low-water mark
since boot. A device can be healthy by the first and alarming by the second, and that
combination is the one worth knowing about: a Waveshare ESP32-C5 running the BLE
gateway measured **27604 B free after 27 hours of uptime with a low-water mark of
4924 B** — and on a later run, 148 B. No tick came anywhere near those figures; each
read a free size comfortably above the fatal floor.

That figure is not proof of how empty the board got *in total*: per the bound below it
is a sum of per-region floors, so the worst simultaneous total was at least 148 B and
may have been much more. What it does prove is sharper than a total would be. The
per-region minima are non-negative and sum to 148 B, so **every** matching region must
have been within 148 B of exhaustion at its own worst moment — and an allocation fails
on the region it asks for, never on the total.

(On that particular board `lowMemory` warns too, because 27604 B is below the default
40 KB `heap_warn_kb`. The two are still saying different things — "there is not much
free right now" versus "some region has been far tighter than this" — and the trough is
the only signal on a device whose *steady* state is healthy and which dips
transiently, which is the general case.)

A 10 s poll of an instantaneous value cannot see a trough between two ticks, and a
transient trough is what precedes an allocation failure. The marks were already being
sampled — they went into the reset record and were read only after a restart — so this
costs a comparison rather than a measurement.

**The mark is a lower bound, not a snapshot.**
`heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` **sums each matching region's
own minimum**, and those minima need not have happened at the same moment. Summing
them is therefore no *higher* than the worst simultaneous total the device really
reached — and is usually lower. Two 100 KB regions that each dip to 10 KB at
different times report 20 KB, while the total free never left 110 KB.

So the figure does **not** say the device ever had that little free; it says the
device never had *less*. It can only overstate how bad things got, which is the safe
direction for a warning — a board this fires on deserves a look even if the number
itself was never on the clock. It is also why the message says "low-water mark below
N KB" rather than claiming memory "fell below" N KB, and why the number is worth
reading beside the live `free_internal` rather than instead of it.

**`memoryTrough` is never fatal**, by construction rather than by configuration. A
low-water mark does not recover within a boot: a fatal condition on it would restart the
device, observe the same mark on the next boot's first dip, and restart again. It is a
diagnostic that tells an operator where to look; the `lowMemory` alarms remain what
actually restarts anything.

It follows that the condition is **sticky for the life of a boot** — once raised it stays
raised, which is correct and is why the message names the threshold rather than the mark
(a message carrying the live figure would defeat `espos_health_report()`'s duplicate
suppression and fan out to every sink on every tick).

The mark itself is read from `GET /api/v1/system/info`, as `min_internal_free` beside
the live `free_internal` ([rest-api.md](rest-api.md)); the watchdog also logs it at
`DEBUG` on every sample. That is the number to look at after the warning, because the
trough is over by the time anyone asks and no live figure records that it happened.
Read it against `free_internal`, not against `free_heap` — on a PSRAM build `free_heap`
counts PSRAM, so it can look healthy while internal RAM is still exhausted. A recovered
`free_internal` beside a tiny mark is a board that dipped; both low is a board simply
short of internal RAM, which is the C5 above.

Internal RAM is judged separately from the total because on a board with PSRAM
it is the scarce pool — the radio, DMA and every task stack come from it, and
tens of megabytes free overall hide its exhaustion. "Internal" means internal *and
byte-addressable* (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`): a classic ESP32 also
turns its spare IRAM into heap, but only 32-bit accesses may use it, so no buffer,
stack or WiFi frame can come from it. Counted, it kept `free_internal` ~31 KB above
the truth, and a HALMET whose `malloc()` heap had run down to 18 KB met the
largest-block alarm worded "fragmented" when it was simply running out. That alarm
now reads "internal RAM low (largest block below N KB)" whenever free internal RAM is
also under the warn floor, and "fragmented" only when plenty is free but none of it is
contiguous. The alarm thresholds sit
*below* the deepest legitimate trough measured so far (an ESP32-P4 panel at
~24 KB free / ~23 KB largest block while esp-sr's wake pipeline allocates,
settling near 95 KB), because a threshold above it restarts the device on
every boot. The same numbers are published as telemetry by `espos_sk`
(`espos.<label>.{internalFree,largestBlock}`), so a dashboard can watch the
trend before the policy has to act.

`skLinkStalled` is the fatal network condition and the reason `netDown` is
not: on a co-processor board (ESP32-P4 + C6 over SDIO) the transport can
wedge, every call into the radio times out, and the WiFi state machine keeps
reporting `CONNECTED` because the disconnect event never crosses the jammed
link. The SignalK stream is real traffic over that link and drops within
seconds, so "connected, yet no stream for minutes" is the honest signal — and
a restart is the fix. The threshold is generous so a SignalK server restart
does not reboot every device on the boat; if the server is routinely down
longer than five minutes, raise `sk.stall_s`.

### Marking your own fatal condition

Say what is wrong through the table like everything else, and add the flag
where a restart is genuinely the recovery:

```c
/* cockpit: the N2K receiver has seen frames before and now sees none */
if (rx.ever_received() && rx.seconds_since_last_rx() > 30) {
    espos_health_report_ex("n2kBus", ESPOS_HEALTH_ALARM, "no frames for 30 s",
                           ESPOS_HEALTH_F_REBOOT_ON_ALARM);
} else {
    espos_health_report_ex("n2kBus", ESPOS_HEALTH_NORMAL, "", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
}
```

That is the whole watchdog: the policy counts the strikes, writes the record
and restarts; the notification reaches SignalK through the same sink as any
other condition. Reserve the flag for what a restart fixes — a wedged
peripheral, memory that will not come back — and leave it off for anything
that is merely bad news (`espos_health_report()` and `_ex(..., 0)` are the
same). `espos_health_fatal_alarm()` tells a display which condition, if any,
is currently counting.

### Watched tasks

For a task that must never stop — the UI thread, a bus receiver:

```c
void ui_task(void *arg)
{
    espos_health_watch_task("ui", 15000);
    for (;;) {
        lv_timer_handler();
        espos_health_kick();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    /* a task that ever exits calls espos_health_unwatch_task() first */
}
```

`espos_health_watch_task()` does two things. It subscribes the calling task to
the IDF **task watchdog**, which espOS configures to panic after
`CONFIG_ESP_TASK_WDT_TIMEOUT_S` (30 s, `sdkconfig.d/espos.defaults`): a
stalled task then core-dumps (`GET /api/v1/system/coredump` has the stack),
restarts, and — while an OTA image is still `pending_verify` — rolls back to
the previous image. And it registers the task with the policy, which raises
`taskStalled` as a fatal ALARM once the task has been silent for `timeout_ms`
(shorter than the hard limit, so the stall is visible as a notification and a
strike before the panic; when the task watchdog is disabled in a consumer's
sdkconfig the policy's restart is the fallback). `espos_health_kick()` is
cheap and lock-free — call it from the loop being watched, at frame rate if
that is what the loop runs at. Up to `ESPOS_HEALTH_WATCHED_MAX` (8) tasks;
watching the same task again updates its name and timeout.

### The reset record

Right before it restarts, the policy writes `espos_health_reset_record_t` —
the condition's key and message, the low-water marks of total and internal
heap, the largest free internal block, the uptime and the wall-clock time — to
RTC memory (plain no-init RAM on chips without it), where it survives the
restart. The next boot reads it with `espos_health_last_reset()`, which
`GET /api/v1/system/info` exposes as `last_reset` ([rest-api.md](rest-api.md)):

```json
"last_reset": {"reason": "software", "health_key": "skLinkStalled",
               "message": "stream down for over 300 s while WiFi reports connected",
               "min_free_heap_before": 148216, "min_internal_before": 31720,
               "largest_block_before": 25600, "uptime_before_s": 86742,
               "at": "2026-09-07T04:12:31Z"}
```

The record is valid for **the whole of the boot that follows** — every caller
sees it, not just the first — and is cleared by that boot's first read so it
cannot be attributed to a later, unrelated reset; a record is also only
believed when the reset reason is `software`. `null` when the last reset was
anything else: power-on, a panic (the core dump is the record then), an OTA
reboot, `POST /system/reboot`.

### Turning it off

`espos_start_opts_t.health_watchdog = false`, or `CONFIG_ESPOS_CORE_HEALTH_WATCHDOG=n`
for the whole firmware: conditions are still reported and published, the
strikes are simply never counted. For a device where a restart would be worse
than the condition — a display mid-passage — that is a legitimate choice; so
is keeping the policy and raising `sk.stall_s`.

## Reading it back, and rehearsing a fault

`GET /api/v1/health` lists the conditions with the worst state and which one (if
any) the policy would restart for; `POST /api/v1/health/test` raises a synthetic
one. Both are in [rest-api.md](rest-api.md#health).

The test endpoint exists because **every built-in alarm is fatal**: both `lowMemory`
variants and `taskStalled` carry `ESPOS_HEALTH_F_REBOOT_ON_ALARM`, so there is no built-in
condition a person can hold in ALARM long enough to watch a buzzer repeat —
triggering one ends the observation. That made a hardware test like "confirm the
alarm buzzer repeats and the LED goes red" impossible to run on a boxed-up board
without a genuine fault to hand (espOS #137).

`espos_health_report_test()` reports through the same path as anything real — a
sink deliberately cannot tell the difference, which is the point — with flags
always `0`, so it is *structurally* incapable of arming the reboot path rather than
merely trying not to. Keys must start with `test.`, at most one drill is active at
a time, and a `ttl_s` (optional, 1–300, **default 45**) ends a drill whose test
session went away. A drill reaches every sink including the SignalK notifications,
so it is visible on a chartplotter while it lasts.

The ttl is a backstop, not a stopwatch. It is driven by the policy tick, so a drill
whose ttl has elapsed is cleared within one `CONFIG_ESPOS_HEALTH_POLICY_TICK_S`
(default **10 s**, up to 60) — expect `ttl_s: 45` to clear somewhere in 45–55 s, not
at 45. `GET /api/v1/health` also drives expiry, so a reader is never told about a
drill that has already timed out, and a test script polling the endpoint sees it go
sooner than the tick would manage. That is also what makes the endpoint correct on a
build with the watchdog disabled, where nothing ticks at all.

For a drill that must end at a known moment, clear it explicitly with
`state: "normal"` — which is what the ttl is a backstop *for*.

## Sizing

`CONFIG_ESPOS_HEALTH_MAX_CONDITIONS` (default 12) is the number of distinct
keys; espOS itself uses up to seven (`lowMemory`, `memoryTrough`, `taskStalled`,
`netDown`, `skLinkStalled`, `skCertificate`, and `coprocessorStale` on a hosted
target), plus one slot per distinct `test.` key a drill has used — and a key keeps its slot for the life of the boot, so a test script should
reuse one rather than invent one per run.
`CONFIG_ESPOS_HEALTH_MAX_SINKS` (default 4) is the number of
consumers. Both are fixed tables — the set of conditions a firmware can raise
is decided at build time. Reporting a key beyond the limit returns
`ESP_ERR_NO_MEM` and logs; it never grows silently.
