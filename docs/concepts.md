# Concepts

What espOS is made of, in what order it comes up, which task calls you back,
and how a setting travels from a JSON descriptor to the web UI. The
per-component documents ([config](config.md), [net](net.md), [wifi](wifi.md),
[signalk](signalk.md), [ota](ota.md), [health](health.md), [REST
API](rest-api.md)) go deeper; this one is the map.

## Component graph

```
                 application (main/)
                        │
                   espos_core            espos_start(): the order below, once
                        │
   ┌────────┬───────────┼───────────┬────────────┬───────────┐
espos_log  espos_config  espos_health  espos_httpd  espos_net   espos_event
                                          │            │
                                          ├─espos_time │                the wall clock; SNTP arms on NETWORK_UP
                                          │            │
                                          │        [espos_wifi]           transport: reports into espos_net; absent on esp32h2/h21/h4
                                          │            │
                                          └─────┬──────┘
                                             espos_sk                 optional, started if built
                                                │
                                            espos_ota                 optional
                                            espos_ble                 optional
                       espos_n2k · espos_voice · espos_audio          optional, application-started
```

Arrows point at what a component needs. `espos_event` is a leaf: it needs
nothing of espOS, so anything may post to it without creating a cycle.
`espos_health` is likewise a leaf so that raising a condition never drags the
SignalK stack in ([health.md](health.md)). `espos_net` is the network seam
([net.md](net.md)): transports (`espos_wifi`, later Ethernet) report their
link into it, and `espos_sk`, `espos_ota` and the mDNS responder ask it —
never a radio — so a firmware without WiFi keeps all three. `espos_n2k`,
`espos_voice` and `espos_audio` have no start order of their own; an
application starts them when its board has the hardware.

`espos_time` ([time.md](time.md)) is the device's wall clock. It sits on
`espos_httpd` (its `/time` endpoints) and pointedly NOT on `espos_sk`, even
though the SignalK stream is one of its sources: `espos_sk` reads the clock for
its delta timestamps, so the dependency already runs that way and the reverse
would close a cycle. `espos_time` offers `espos_time_set()` instead and
`espos_sk` calls it. `espos_log` and `espos_httpd` want the clock too and may
not depend on it for the same reason; both declare a weak "there is no clock"
function that `espos_time` overrides when it is in the build, so a firmware
without the component behaves exactly as it did before and neither names the
other.

Firmware updates are tied together the same way. A host OTA (`espos_ota`) and
a radio co-processor update (`espos_wifi`, [hardware.md](hardware.md#over-the-link))
each end in a restart, so neither starts while the other runs; each declares
the other's busy check weak (`espos_ota_busy()`, `espos_wifi_coproc_busy()`)
rather than depending on it, and `espos_power` reads both to stay awake through
either.

## espos_start(): the order and why

```c
#include "espos.h"
void app_main(void) { ESP_ERROR_CHECK(espos_start(NULL)); /* your application */ }
```

| Step | Call | Why here |
|---|---|---|
| 1 | `espos_log_init()` | The ring must exist before the first line worth keeping; `/api/v1/logs` shows the boot. |
| 2 | `espos_config_init()` | Everything after this reads its settings from the store. Posts `ESPOS_EVENT_CONFIG_READY`. |
| — | `espos_health_policy_start()` | Still inside `espos_init()`, when `espos_start_opts_t.health_watchdog` is set (the default) and `CONFIG_ESPOS_CORE_HEALTH_WATCHDOG` is on: the device watchdog is armed before anything can stall ([health.md](health.md)). `espos_core` also subscribes to `NETWORK_UP`/`NETWORK_DOWN` here to raise and clear the `netDown` warning. A failure logs and the boot goes on without the watchdog. |
| — | `before_network` hook | Display up, so it can show the portal SSID; anything that must exist before a client can connect. |
| 3 | `espos_httpd_start()` | Before the network: `/net/status` and the provisioning portal's page have to be there the moment there is a link or an access point. Posts `HTTPD_STARTED`. |
| 4 | `espos_net_start()` | Base MAC and device id, hostname (`net.hostname`, moved once from a 0.7 `wifi.hostname`), `/net/status`, the `net` SSE event, and the mDNS responder (`<hostname>.local`, `_http._tcp`, `_espos._tcp`, [net.md](net.md)). Before any transport, so the hostname is on every interface they create. Posts `NETWORK_UP` / `NETWORK_DOWN` as the default route comes, goes or moves; `MDNS_READY` follows every `NETWORK_UP`. |
| 5 | `espos_time_start()` | The wall clock ([time.md](time.md)): adopts a time carried through a deep sleep, registers `/time`, and arms SNTP so it starts polling on the first `NETWORK_UP` — after `espos_net` so that event is not missed, before every transport so it does not matter which one produces it. Not a `STAGE`: a device that boots without knowing the time still does everything else, and the log says which it is. |
| 6 | `espos_wifi_start()` (if built and `CONFIG_ESPOS_WIFI`) | Station, portal, `/wifi` endpoints; reports its link into `espos_net`. Built on every chip with a radio or a co-processor, excluded on the 802.15.4-only H-series. |
| 7 | `espos_sk_start()` (if built) | Discovery is mDNS and the stream needs a link; both ask `espos_net`, so it must exist. Just before it, `espos_sk_set_app_name(espos_app_name())`, so the access request reads `<app> <hostname>` in the server's list. Also starts the SignalK clock fallback (`src/sk_time.c`), which follows `navigation.datetime` while the clock is unset. |
| 8 | `espos_ota_start()` (if built) | Its API sits on the HTTP server; its confirm/rollback policy watches `espos_net`. |
| 9 | `espos_ble_start()` (if built) | Authenticates with the SignalK token. For BLE "built" means both the component in the list and `CONFIG_BT_BLUEDROID_ENABLED`; a firmware may link `espos_ble` without the stack and nothing is started. |

`espos_init()` is steps 1–2, `espos_start_network()` steps 3–9; both are
idempotent, as is every `espos_*_start()`. "If built" is decided at
configure time: `espos_core` links `espos_sk`, `espos_ota` and `espos_ble`
only when they are in the project's component list, so a firmware picks its
stacks by listing components, not by editing a start sequence.

The order is also enforced at runtime. Each start checks its prerequisite
and fails with `ESP_ERR_INVALID_STATE` plus one log line naming the missing
call — `espos_wifi_start: call espos_httpd_start() first (or espos_start())`:

| Call | Requires |
|---|---|
| `espos_httpd_start()` | `espos_config_is_ready()` |
| `espos_net_start()` | `espos_config_is_ready()` and `espos_httpd_handle() != NULL` |
| `espos_time_start()` | `espos_config_is_ready()` and `espos_httpd_handle() != NULL` |
| `espos_wifi_start()` | `espos_httpd_handle() != NULL` and `espos_net` started (`espos_net_get_status()` answers) |
| `espos_sk_start()` | `espos_net` started |
| `espos_ota_start()` | HTTP server and `espos_net` started |

A device coming up narrates itself on the monitor in this order: `espOS
<version> on <target> — app <name> <version>`; `no network configured: join
"<portal ssid>" and open http://192.168.4.1` (or `connected to "<ssid>" as
<ip> — web UI: http://<hostname>.local`); `found signalk-server "<name>" at
<host>:<port>`; `access requested — approve it in the server UI: Security →
Access Requests`; `approved, streaming`.

## Threading contracts

Every callback in espOS runs on a task that is not yours. The rule is the
same everywhere — copy what you need, return quickly, never block on
something that may itself be waiting for the task you are on — but the task
differs, and it matters when you reach for a lock:

| Callback | Runs on | Notes |
|---|---|---|
| `espos_config_subscribe` change callback | the writer's task (an HTTP handler, usually) | Store lock released; the callback may read or write config. |
| `espos_sk_subscribe` update callback | the SK stream task | Strings valid only during the call. Never call an `espos_sk_*` that could wait on the stream. |
| `espos_sk_put` result callback | the SK stream task | Same rules. |
| `espos_health_add_sink` sink | the reporting task | No lock held; a sink may report conditions of its own. |
| `espos_sk_http_*`, `espos_sk_get_value/meta` | the caller | **Blocks** up to 2×timeout_ms; not from the stream task, an event handler or a URI handler. |
| `espos_mdns_start/add_service/remove_service` | the caller | **May block** a few ms on the responder task; not from an event or URI handler. |
| `espos_net_subscribe` callback | the task of the transport that reported the change — for WiFi the default event loop task | No `espos_net` lock held; the status is valid for the call only. Copy it out and return; never block. |
| `espos_net_report()` (transports only) | the caller | Runs the subscribers and posts `NETWORK_UP/DOWN` before returning; never call it under a lock a subscriber might want. |
| `espos_health` policy tick | the `esp_timer` task | Every 10 s: reports `lowMemory`/`taskStalled`, counts strikes, restarts; sinks run there on that tick. |
| `espos_time_subscribe` callback | the task of the source that set the clock — IDF's SNTP task for `sntp`, the SignalK stream task for `sk`, the HTTP server's for a manual `PUT` | No `espos_time` lock held. Copy what you need and return; never block. Note that `espos_log`'s wall-clock prefix takes the `espos_time` lock while holding the ring lock, so nothing inside `espos_time` may log under its own lock. |
| HTTP URI handlers (`espos_httpd_register`) | the `esp_http_server` task | One task for all requests; a slow handler stalls the UI. |
| `espos_httpd_sse_on_connect` callback | the `esp_http_server` task | Send the client its snapshot with `espos_httpd_sse_send()` and return. |
| `espos_log_set_notify` callback | a FreeRTOS timer task | At most every 500 ms, never from inside the logging call — so it may itself log. |
| `ESPOS_EVENT` handlers | the default esp_event loop task | Shared with `WIFI_EVENT`/`IP_EVENT`; a blocked handler stalls the WiFi driver's own events. |
| `espos_log_read` visitor | the caller, with the ring locked | Do not log from inside. |
| `espos_wifi_refresh_rssi()` | the caller | **May block** on a co-processor RPC; not from a UI or event task. |

Publishing is the other way round: `espos_sk_publish_*`, `espos_sk_notify`,
`espos_health_report`, `espos_event_post`, `espos_net_get_status`,
`espos_net_is_up`, `espos_time_now_ms` and `espos_time_is_synced` are
thread-safe and never block for long (none of them touches a driver or waits
on a network), so they are safe from any task, including the callbacks above.

## Descriptor → keys → UI

A setting exists in exactly one place: a JSON descriptor per NVS namespace,
registered by the component that owns it
(`espos_config_add_descriptor(config/myns.json)` in its `CMakeLists.txt`).
At build time the generator turns every registered descriptor into

* `espos_cfg_keys.h` — `ESPOS_CFG_NS_<NS>` and `ESPOS_CFG_<NS>_<KEY>`
  constants, so code never spells an NVS key by hand;
* the C tables `espos_config` validates against at runtime — types, ranges,
  defaults, `secret`, `restart_required`;
* the JSON Schema served at `GET /api/v1/config/schema`, from which the web
  UI renders its forms without knowing any component.

So a new setting is one JSON entry: the key constant, the validation, the
REST document and the settings page all follow. A project that registers no
descriptor of its own still builds (empty tables, an empty schema).
Details, migrations and the descriptor format: [config.md](config.md).

## Events

`ESPOS_EVENT` on the default esp_event loop; subscribe with
`espos_event_subscribe(id, handler, arg)`, `ESP_EVENT_ANY_ID` for all.

| Event | Posted by | Data |
|---|---|---|
| `ESPOS_EVENT_CONFIG_READY` | `espos_init()` after the store is up | — |
| `ESPOS_EVENT_HTTPD_STARTED` | `espos_httpd_start()` | — |
| `ESPOS_EVENT_NETWORK_UP` | `espos_net`, a default route exists (a transport reported an address) | `espos_event_network_t {ip, hostname}` |
| `ESPOS_EVENT_NETWORK_DOWN` | `espos_net`, the default route is gone (one per UP; a route that moves to another interface or address is DOWN then UP) | — |
| `ESPOS_EVENT_MDNS_READY` | `espos_net` (mdns.c): on every `NETWORK_UP` once the responder runs, and once from `espos_mdns_start()` if the route is already up | — |
| `ESPOS_EVENT_SK_SERVER_SELECTED` | `espos_sk`, a server was chosen or changed | `espos_event_sk_server_t {host, port}` |
| `ESPOS_EVENT_SK_TOKEN_APPROVED` | `espos_sk`, token verified | — |
| `ESPOS_EVENT_SK_STREAM_CONNECTED` / `_DISCONNECTED` | `espos_sk` stream task, on every transition | — |
| `ESPOS_EVENT_OTA_AVAILABLE` | `espos_ota`, manifest names a newer build | `espos_event_ota_t {version}` |
| `ESPOS_EVENT_TIME_SYNCED` | `espos_time`, a source set the wall clock (every set, not only the first) | `espos_event_time_t {source, unix_ms}` |

Events are notifications, not state: a subscriber that comes late asks the
component's status API (`espos_net_get_status()`, `espos_wifi_get_status()`,
`espos_sk_get_server()`, `espos_ota_status_json()`) for the current picture and uses events to learn
about changes from then on. Handlers run on the event loop task (see above).
Ids are part of the ABI and are only ever appended.

## From SensESP's model to espOS's

SensESP is a graph: sensors, transforms and outputs are objects created in
`setup()`, wired with `connect_to()`, and ticked by one event loop; a
setting is an object's `config_path` plus a `ConfigItem()`. espOS is a
runtime with a C API: the components above come up in one call, and the
application is a task that reads, computes and calls `espos_sk_publish_*()`;
a setting is one entry in a descriptor. Both reach the same wire — deltas to
`vessels.self`, an access request, metadata for non-standard paths — and
that is why a SensESP author is at home here after one afternoon. A typed
C++ facade over this API is planned for a later tranche; until it exists the
C shape below is the shape. The full mapping, symbol by symbol, and a worked
port of SensESP's `analog_input.cpp` are in
[migration-from-sensesp.md](migration-from-sensesp.md).

| SensESP | espOS |
|---|---|
| `SensESPAppBuilder` … `get_app()` | `espos_start(NULL)` |
| `event_loop()->onRepeat()` + `RepeatSensor` | a task with `vTaskDelay()`, or an `esp_timer` |
| `connect_to()` chain of transforms | arithmetic in C between the read and the publish |
| `SKOutputFloat`, `SKMetadata` | `espos_sk_publish_number()`, `espos_sk_declare_meta()` |
| `SKValueListener` / `SKPutRequest` | `espos_sk_subscribe()` / `espos_sk_put()` |
| `config_path` + `ConfigItem()` | a key in `config/<ns>.json`; `espos_config_get_*()`, `espos_config_subscribe()` |
| `SystemStatusLed`, `enable_wifi_watchdog()` | `espos_health` sinks and its policy |
| `enable_ota()` | `espos_ota`: signed, rolled back, manifest-driven |
