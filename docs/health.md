# Device health

A device on a boat has nobody watching it. When something goes wrong, it has
to say so, and in a few cases it has to restart itself. `espos_health` does
both. Any component can report what is wrong with the device into one table,
and anything that wants to know can read from it: the Signal K notifications,
a status LED, the web page. A small [watchdog policy](#the-watchdog-policy)
reads the same table and decides whether a restart would help.

```c
#include "espos_health.h"

/* Call it as often as you like, the same state twice does nothing. */
if (!bus_answering) {
    espos_health_report("n2kBus", ESPOS_HEALTH_WARN, "no frames for 30 s");
} else {
    espos_health_report("n2kBus", ESPOS_HEALTH_NORMAL, "");
}
```

A condition describes how things are, not something that happened. If you
report the same state and message twice, the sinks hear about it once. So the
easy way to use it is to report on every poll of whatever you are watching,
without remembering what you said last time. `ESPOS_HEALTH_NORMAL` clears the
condition and goes out to the sinks like any other change; that is also what
clears an alert raised before the last reboot.

The key (`"n2kBus"`, `"wakeService"`, `"lowMemory"`) is a name, not a
sentence. It ends up in a Signal K path, and rules and dashboards match on
it. The message is for people and can change without a new notification. A
key must be shorter than 24 characters and a message shorter than 96. A
longer one is refused rather than cut short, because a shortened key would never match the next report.

## Sinks

Anything can listen to the table:

```c
static void led_sink(const char *key, espos_health_state_t state,
                     const char *message, void *arg)
{
    gpio_set_level(STATUS_LED, espos_health_worst() != ESPOS_HEALTH_NORMAL);
}

espos_health_add_sink(led_sink, NULL);
```

When you register a sink, it is told about every condition so far, so a sink
that starts late still knows the current state. A sink whose far end can lose
what it was told calls `espos_health_replay()` to tell it all again;
`espos_sk` does that on every stream connect. Sinks run on the task that
made the report, with no lock held. You can report from inside a sink, but
don't block there for long.

With `ESPOS_SK_NOTIFICATIONS` on, `espos_sk` registers a sink of its own. It
publishes each condition to `notifications.espos.<label>.<key>` with `state`,
`message` and a `visual` method, so a chartplotter shows it, and publishes
them all again each time the stream connects.
`espos_sk_notify()` still works and simply calls `espos_health_report()`. Use
it only when you want the report refused while Signal K isn't running.

## Why it is not part of espos_sk

Reporting a fault is something every device needs, and Signal K is only one
place to send it. When `espos_sk_notify()` was the only way to raise a
condition, `espos_voice`, a voice satellite with nothing to do with Signal K,
had to pull in the whole Signal K stack for its one notification, and a
firmware without Signal K couldn't report anything at all. The N2K gateway
wants to report a silent bus and an application wants to report its sensor;
none of them should need Signal K for that.

## The watchdog policy

Before this existed, every firmware wrote its own health check. The cockpit
panel's was 95 lines and had a bug the policy is now built around: it treated
"WiFi disconnected" as fatal. While the network was marginal, the panel
restarted about every 90 s and you could barely use it in between. The
policy exists so that this mistake is made once, here, and not again.

`espos_init()`, and so `espos_start()`, turns the policy on, unless
`espos_start_opts_t.health_watchdog` is false or
`CONFIG_ESPOS_CORE_HEALTH_WATCHDOG` is off. A firmware that doesn't use
`espos_init()` calls `espos_health_policy_start()` itself. Every 10 s
(`CONFIG_ESPOS_HEALTH_POLICY_TICK_S`) it:

1. looks at the free memory and raises or clears **`lowMemory`** (what is
   free now) and **`memoryTrough`** (the least that was free at any point
   since boot);
2. checks the [watched tasks](#watched-tasks) and raises or clears
   **`taskStalled`**;
3. looks for any condition in `ALARM` that was raised with
   `ESPOS_HEALTH_F_REBOOT_ON_ALARM`. If there is one, that counts as a
   **strike**. If there isn't, the count goes back to zero.

After 3 strikes in a row (`CONFIG_ESPOS_HEALTH_POLICY_STRIKES`), which is
30 s of a fatal condition that didn't go away, it writes the
[reset record](#the-reset-record) and restarts. The rest are rules you can
count on:

* **A `WARN` never restarts the device**, whatever its flags.
* **An `ALARM` without the flag never restarts the device.**
  `espos_health_report()` can't set the flag. A condition is fatal only
  because the code that raised it said so, with `espos_health_report_ex()`.
* **One clean tick resets the count.** A condition that comes and goes never
  adds up to a restart. Only one that stays does.
* **Losing WiFi is never a reason to restart.** `espos_core` raises `netDown`
  as a `WARN` when the network goes down and clears it when it comes back,
  never with the fatal flag. A router reboot, roaming between access points
  or sailing out of range is normal, and `espos_wifi` reconnects by itself. A
  restart would throw away the screen, every connection and anything unsaved,
  and fix nothing. A link that only looks connected is a different problem,
  covered by `skLinkStalled` below.

The policy is plain C (`espos_health_policy.h`) that gets the clock, the
memory figures, the table, the record store and the restart through a small
interface, like the WiFi and token state machines. That is how
`test/host/espos_health_test` checks it on a PC: exactly N strikes, never on
a `WARN`, never on an unflagged `ALARM`, the count resetting after a clean
tick, and what goes into the record.

### Built-in conditions

These are the conditions espOS raises itself. Which ones a device can show
depends on the components in its build.

| Key | Raised by | State | Restarts | When |
|---|---|---|---|---|
| `lowMemory` | the policy | `WARN` | no | Less than 40 KB free in total (`CONFIG_ESPOS_HEALTH_HEAP_WARN_KB`), or less than 20 KB of internal RAM (`CONFIG_ESPOS_HEALTH_INTERNAL_WARN_KB`). |
| `lowMemory` | the policy | `ALARM` | **yes** | Less than 12 KB of internal RAM free (`CONFIG_ESPOS_HEALTH_INTERNAL_ALARM_KB`), or no free internal block of 8 KB (`CONFIG_ESPOS_HEALTH_LARGEST_BLOCK_ALARM_KB`). |
| `memoryTrough` | the policy | `WARN` | never | The low-water mark of internal RAM since boot is below 10 KB (`CONFIG_ESPOS_HEALTH_INTERNAL_TROUGH_WARN_KB`). Stays until the next boot. |
| `taskStalled` | the policy | `ALARM` | **yes** | A task watched with `espos_health_watch_task()` hasn't called `espos_health_kick()` within its timeout. |
| `netDown` | `espos_core` | `WARN` | no | The network went down. Cleared when it comes back. |
| `wifiStalled` | `espos_wifi` | `ALARM` | **yes** | WiFi has been trying for `wifi.stall_s` (1800 s, at least 300; 0 turns it off) without associating once, nothing else carries the network and nobody is on the setup access point. At most once per outage: after such a restart the device doesn't restart for it again until WiFi has connected. |
| `skLinkStalled` | `espos_sk` | `ALARM` | **yes** | WiFi says it is connected and the Signal K stream worked earlier in this boot, but it has now been down for `sk.stall_s` (300 s, at least 60). |
| `skCertificate` | `espos_sk` | `ALARM` | no | The Signal K server's TLS certificate was refused ([Signal K](signalk.md)). Cleared when a connection succeeds. A restart wouldn't bring a new certificate. |
| `tlsMemory` | `espos_sk` | `WARN` | no | There wasn't a large enough free block for a TLS handshake, so the device didn't try. Cleared when the next attempt finds one. |
| `coprocessorStale` | `espos_wifi` | `WARN` | no | On a board with a radio co-processor, esp_hosted itself says the co-processor firmware is older than this build expects, which causes RPC timeouts. Checked once per boot, at the first co-processor heartbeat ([Hardware](hardware.md#updating-the-c6-co-processor-firmware)). |
| `flowMailbox` | `espos_flow` | `WARN` | no | The [flow](flow.md) mailbox was full and work was dropped. Stays until the next boot. |
| `flowConfig` | `espos_config` | `ALARM` | no | A flow node's settings couldn't be registered, so they won't appear on the settings page ([Config](config.md)). |
| `wakeService` | `espos_voice` | `WARN` | no | The network wake word service couldn't be reached or set up, so the device uses its on-device wake word. |

`coprocessorStale` uses esp_hosted's own verdict
(`eh_host_mcu_transport_verify_fw_compat()`), not a comparison espOS makes,
so the two can't disagree. It is `NORMAL` whenever the main chip is not
newer: equal versions, a patch-level difference, or a main chip that is
itself behind, which is a different problem.

### `lowMemory` and `memoryTrough` answer different questions

`lowMemory` looks at what is free at each tick. `memoryTrough` looks at the
low-water mark: the least that was free at any time since boot. A device can
look fine by the first and worrying by the second, and that is exactly the
case worth knowing about. A Waveshare ESP32-C5 running the BLE gateway had
27604 bytes free after 27 hours, with a low-water mark of 4924 bytes, and on
a later run 148 bytes. No tick ever saw anything close to those numbers,
because a tick every 10 s can't see a dip that happens between two ticks.
Those dips are what come right before an allocation fails. (That board also
shows `lowMemory` as a warning, because 27604 bytes is under the 40 KB
default. The two still mean different things.)

**The mark can only make things look worse than they were, never better.**
The ESP-IDF call behind it (`heap_caps_get_minimum_free_size()`) adds up the
lowest point of each memory region, and those lowest points may have
happened at different times. Two 100 KB regions that each dipped to 10 KB at
different moments report 20 KB, even though the total never went below
110 KB. So the mark doesn't prove the device ever had that little free in
total. What it does prove is useful anyway: since every region's own low
point is included, each one came within that many bytes of running out, and
an allocation fails in the region it asks, not in the total. That is why the
message says "low-water mark below N KB" and not "memory fell below N KB".

**`memoryTrough` never restarts the device**, and no setting changes that.
The mark doesn't recover during a boot, so a fatal condition on it would
restart the device, see the same mark at the next dip and restart again. For
the same reason it stays raised until the next boot once it is up. The
message names the threshold, not the live figure, so it doesn't change on
every tick and flood the sinks.

To see the mark, read `min_internal_free` in `GET /api/v1/system/info`, next
to the live `free_internal` ([REST API](rest-api.md)). Compare it with
`free_internal`, not `free_heap`: on a board with PSRAM, `free_heap` counts
the PSRAM and can look fine while internal RAM is gone. A healthy
`free_internal` next to a tiny mark means the board dipped and recovered.
Both low means the board is short of internal RAM all the time, like the C5
above.

**Why internal RAM is checked on its own.** On a board with PSRAM, internal
RAM is the scarce part: the radio, DMA and every task stack come from it, and
megabytes of free PSRAM hide it running out. "Internal" here means internal
and usable byte by byte (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`). A classic
ESP32 also adds spare instruction RAM to the heap, but only 32-bit accesses
can use it, so no buffer or stack can live there, and counting it would
overstate free internal RAM by about 31 KB. The largest-block alarm says
"internal RAM low" when free internal RAM is also under the warning level
or under the block floor itself, and "internal RAM fragmented" when there is
more free but not in one piece.

The alarm levels sit below the deepest normal dip measured so far: an
ESP32-P4 panel goes down to about 24 KB free (23 KB largest block) while the
on-device wake word starts, then settles near 95 KB. A threshold above that
would restart the panel on every boot. `espos_sk` also publishes the same
figures as `espos.<label>.internalFree` and `.largestBlock`, so a dashboard
can show the trend long before the policy has to act.

### Why `skLinkStalled` restarts and `netDown` doesn't

On a board with a radio co-processor, such as an ESP32-P4 with a C6, the link
between the two chips can jam. Every call into the radio then times out, and
WiFi keeps saying "connected", because the disconnect never gets across the
jammed link. The Signal K stream is real traffic over that same link and
drops within seconds, so "connected, but no stream for minutes" is the
reliable sign, and a restart is the fix. The 300 s limit is generous so that
restarting the Signal K server doesn't restart every device on the boat. If
your server is often down for longer than that, raise `sk.stall_s`.

### Why `wifiStalled` restarts, once

A station that never gets back in is the other half of the same story. An
ESP32-P4 panel retried a known-good access point every 11 s for 45 hours and
connected 10 s after a reset ([espOS #136](https://github.com/signalk-espOS/espOS/issues/136)).
`netDown` only warns, and `skLinkStalled` needs WiFi to say it is connected,
which this station never did. So `wifiStalled` restarts after `wifi.stall_s`
of unbroken trying. Only association counts as getting in: a station that
associates and then gets no address has a working radio and a broken DHCP
server, and a restart doesn't mend that.

It must not turn an access point that is simply off into a reboot loop: a
marina switching its router off, a boat left on battery. So it restarts once,
and holds off until WiFi has connected again. The hold is kept in RTC memory
rather than in the reset record, so a different restart in between, an update
or the co-processor watchdog, doesn't lift it; a power cycle does. The count
also starts again while another transport carries the network (a restart
would cut the working link) and while someone is on the setup access point
(they are about to fix the cause). Changing the networks or their passwords
starts it again too and allows one more restart, and so does someone joining
the setup access point. Ethernet coming up does not: the access point that
stayed off is still off.

### Marking your own fatal condition

Report through the table like anything else, and add the flag only where a
restart really is the cure:

```c
/* cockpit: the N2K receiver got frames before and now gets none */
if (rx.ever_received() && rx.seconds_since_last_rx() > 30) {
    espos_health_report_ex("n2kBus", ESPOS_HEALTH_ALARM, "no frames for 30 s",
                           ESPOS_HEALTH_F_REBOOT_ON_ALARM);
} else {
    espos_health_report_ex("n2kBus", ESPOS_HEALTH_NORMAL, "", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
}
```

That is all. The policy counts the strikes, writes the record and restarts,
and the notification reaches Signal K through the same sink as every other
condition. Keep the flag for things a restart fixes, such as a stuck
peripheral or memory that won't come back, and leave it off for anything
that is only bad news. (`espos_health_report()` is the same as
`espos_health_report_ex(..., 0)`.) `espos_health_fatal_alarm()` tells a
display which condition, if any, is counting towards a restart.

### Watched tasks

For a task that must never stop, such as the screen's UI task or a bus
receiver:

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

`espos_health_watch_task()` does two things:

* It adds the task to ESP-IDF's **task watchdog**, which espOS sets to panic
  after 30 s (`CONFIG_ESP_TASK_WDT_TIMEOUT_S` in `sdkconfig.d/espos.defaults`).
  A stuck task then leaves a core dump (`GET /api/v1/system/coredump` shows
  the stack) and the device restarts. If that happens while a new update is
  still waiting to be confirmed, the update is rolled back ([OTA](ota.md)).
* It tells the policy about the task. Once the task has been quiet for
  `timeout_ms`, the policy raises `taskStalled` as a fatal `ALARM`. The
  timeout is shorter than the 30 s, so you see a notification before the
  panic. If a firmware turns the task watchdog off, the policy's restart is
  what's left.

`espos_health_kick()` is cheap and takes no lock, so call it from the loop
you are watching, even at frame rate. Up to 8 tasks can be watched
(`ESPOS_HEALTH_WATCHED_MAX`). Watching the same task again updates its name
and timeout.

### The reset record

Just before it restarts, the policy writes down why: the condition's key and
message, the low-water marks of total and internal memory, the largest free
internal block, the uptime and the time of day. It keeps this in RTC memory
(plain RAM that isn't cleared at boot, on chips without RTC memory), which
survives the restart. The next boot reads it with `espos_health_last_reset()`,
and `GET /api/v1/system/info` shows it as `last_reset` ([REST API](rest-api.md)):

```json
"last_reset": {"reason": "software", "health_key": "skLinkStalled",
               "message": "stream down for over 300 s while WiFi reports connected",
               "min_free_heap_before": 148216, "min_internal_before": 31720,
               "largest_block_before": 25600, "uptime_before_s": 86742,
               "at": "2026-09-07T04:12:31Z"}
```

Everything that asks during the boot that follows gets the same record. It is
removed from RTC memory at that boot's first read, so a later restart for
another reason can't be blamed on it, and it is only believed when the reset
reason is `software`. `last_reset` is `null` after any other kind of reset:
power-on, a crash (the core dump tells that story), an update, or
`POST /system/reboot`.

### Turning it off

Set `espos_start_opts_t.health_watchdog = false`, or
`CONFIG_ESPOS_CORE_HEALTH_WATCHDOG=n` for the whole firmware. Conditions are
still reported and published; the strikes just aren't counted. For a device
where a restart would be worse than the problem, say a display in the middle
of a passage, that is a fair choice. So is keeping the policy and raising
`sk.stall_s`.

## Reading it back, and rehearsing a fault

`GET /api/v1/health` lists the conditions, the worst state, and which
condition, if any, the policy would restart for. `POST /api/v1/health/test`
raises a pretend one. Both are in [REST API](rest-api.md#health).

The test endpoint exists because the built-in alarms you could provoke on
purpose restart the device. Both `lowMemory` alarms and `taskStalled` carry
the fatal flag, so you couldn't hold one long enough to check, say, that the
alarm buzzer repeats and the LED turns red on a board that is already boxed
up (espOS #137).

`espos_health_report_test()` reports through the same path as a real
condition, so sinks can't tell the difference, which is the point. Its flags
are always 0, so a drill can't restart the device even by mistake. Keys must
start with `test.`, only one drill runs at a time, and a drill ends by itself
after `ttl_s` (1 to 300, 45 by default) in case the test script went away.
A drill reaches every sink, Signal K notifications included, so you see it on
the chartplotter while it lasts.

The `ttl_s` is a safety net, not a stopwatch. It is checked on the policy
tick, so a drill ends up to one tick late: with `ttl_s: 45` and the default
10 s tick, somewhere between 45 and 55 s (the tick can be set up to 60 s).
`GET /api/v1/health` also checks it, so a script polling that endpoint sees
the drill go sooner, and it works on a firmware with the watchdog turned off,
where nothing ticks. To end a drill at an exact moment, clear it yourself with
`state: "normal"`.

## Sizing

`CONFIG_ESPOS_HEALTH_MAX_CONDITIONS` (12 by default) is how many different
keys the table holds. espOS itself can use up to eleven of them, the
[built-in conditions](#built-in-conditions) above, depending on which
components are in the build. Leave room for those and your own keys: a
fatal key that finds the table full can never restart the device. Each `test.` key a drill used also takes a slot,
and keeps it until the next boot, so a test script should reuse one key
rather than make up a new one every run.
`CONFIG_ESPOS_HEALTH_MAX_SINKS` (4 by default) is how many listeners there
can be. Both are fixed at build time. Reporting a new key when the table is
full returns `ESP_ERR_NO_MEM` and logs it; the table never grows by itself.
