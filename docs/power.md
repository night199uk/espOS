# Power and deep sleep (`espos_power`)

A duty cycle for a device on a battery: wake, get on the network, publish,
flush the SignalK stream, deep-sleep for `power.interval_s`, again. Optional
and off by default: add the component to a project and set `power.mode` to
`cycle`. The example is
[`duty_cycle`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_power/examples/duty_cycle).

```c
ESP_ERROR_CHECK(espos_start(NULL));            /* starts the cycle's task last */
espos_sk_publish_number("sensors.x.value", v); /* what this wake is for */
/* nothing else: espos_power flushes and sleeps once the stream carried it */
```

## Staying reachable

A device that sleeps almost all the time is almost never reachable, so the
rules are mostly about not losing it. They are pure C
(`espos_power_policy.h`, tested in `test/host/espos_power_test`) and are checked
in this order, each winning over every later one:

| Rule | Why |
|---|---|
| `power.mode` off → stay awake | the default: a freshly flashed device must learn its network and server first |
| running image unconfirmed → stay awake | every wake from deep sleep is a boot, and the bootloader marks an image still pending verification as aborted. Sleeping before an update confirmed itself would roll it back at the next wake. espOS confirms an image once the network is up, and rolls it back itself after `ota.confirm_tmo_s` if it never gets there |
| an update being checked, downloaded or installed → stay awake | a wake lasts seconds, and an update longer than that would be cut off on every wake and never finish. `espos_ota`'s own timeouts end a download that hangs. Needs `espos_ota` in the build. A radio co-processor update (`/system/coprocessor/update`, [hardware.md](hardware.md#over-the-link)) counts too |
| a boot that is not the cycle's timer wake, inside `power.window_s` → stay awake | after a power-on, an update or a crash the web UI and OTA are reachable. **Cutting the power always reopens this window** — it is the way back in |
| past the wake's deadline → sleep | a network that never comes up must not drain the battery. The deadline is `power.awake_max_s` after a timer wake, and `window_s + awake_max_s` after any other boot |
| network down → stay awake | |
| SignalK built and streaming enabled, stream not connected → stay awake | |
| connected for less than `power.publish_ms` → stay awake | the application's values go out. Without SignalK this counts from the network coming up |
| an application hold → stay awake | `espos_power_hold()` / `espos_power_release()`, counted. A hold does not beat the deadline |
| otherwise → sleep | |

## Going to sleep

In a fixed order, because it cannot be taken back:

1. `espos_sk_flush(power.flush_ms)` while the stream is still connected — a
   message still being written counts as pending ([signalk.md](signalk.md),
   "Flushing before sleep");
2. a short pause, because handed to the socket is not yet on the wire;
3. the wake counter and this wake's duration into RTC memory;
4. deep sleep with a timer wake after `power.interval_s`.

`espos_power_sleep_now()` does the same on the application's schedule, and
still refuses while an update is unconfirmed.

## What survives a sleep

| What | Where |
|---|---|
| wake counter, duration of the previous wake | RTC memory (`espos_power`), trusted only after a deep-sleep reset with an intact record |
| wall clock | RTC memory ([time.md](time.md), "Deep sleep") |
| SignalK token, pending access request, TLS pin | NVS ([signalk.md](signalk.md)) |
| deltas buffered while offline | **nothing** — the ring is RAM; flush before sleeping |

A power-on starts the counter at 0.

## Configuration (`power`)

| Key | Default | |
|---|---|---|
| `mode` | `off` | `off` or `cycle`; followed live, never while an update is unconfirmed |
| `interval_s` | 300 | sleep between wakes (10–86400) |
| `window_s` | 300 | awake after a power-on, update or crash (30–86400) |
| `awake_max_s` | 30 | a wake's time budget (5–600) |
| `publish_ms` | 1500 | connected this long before sleeping (0–60000) |
| `flush_ms` | 3000 | how long to wait for buffered deltas (0–30000) |

## `GET /api/v1/power`

```json
{"mode": "cycle", "decision": "stay", "why": "stream", "timer_wake": true, "wake_count": 12,
 "last_awake_ms": 4210, "interval_s": 300, "uptime_ms": 2600, "deadline_ms": 30000, "holds": 0}
```

`why` is one of `off unconfirmed ota window network stream publishing hold
done deadline`.

## Not here yet

- **OTA for a sleeping fleet.** An update that has started holds the device
  awake until it is done, but nothing starts one on a short wake: the manifest
  check waits 20 s after boot and keeps its schedule in RAM. For now, push an
  update (`POST /api/v1/ota`) while the device is awake, or in its awake window
  after a power cycle.
- **WiFi fast connect.** A wake scans every channel; connecting straight to the
  cached access point and channel is the obvious next step.
- **Light sleep** between readings for a device that must stay connected.
