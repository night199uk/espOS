# Concepts

This page is the map. It shows which components espOS has, the order
`espos_start()` brings them up in, which task calls your callbacks, how a
setting gets from a JSON file to the settings page, and which events you can
listen to. Each component's own page goes into detail.

## The components

```
                 application (main/)
                        │
                   espos_core            espos_start(): the order below, once
                        │
   ┌────────┬───────────┼───────────┬────────────┬───────────┐
espos_log  espos_config  espos_health  espos_httpd  espos_net   espos_event
                                          │            │
                                          ├─espos_time │      the clock; SNTP starts on NETWORK_UP
                                          │            │
                                          │        [espos_wifi]     reports into espos_net; not on esp32h2/h21/h4
                                          │            │
                                          └─────┬──────┘
                                             espos_sk              optional, started if built
                                                │
                                            espos_ota              optional
                                            espos_ble              optional
                       espos_n2k · espos_voice · espos_audio       optional, the application starts them
```

A line points at what a component needs. Three of them are worth knowing:

* `espos_event` and `espos_health` need nothing else in espOS, so any
  component can post an event or raise a warning without a dependency loop
  ([health](health.md)).
* `espos_net` is where every network link reports in, WiFi or Ethernet
  ([net](net.md)). Signal K, updates and mDNS ask `espos_net` whether the
  network is up and never talk to a radio, so a firmware without WiFi keeps
  all three.
* `espos_n2k`, `espos_voice` and `espos_audio` are not part of the start
  order. The application starts them when its board has the hardware.

Why the clock and the two kinds of update are wired the way they are is in
[Decisions](decisions.md#why-the-clock-and-the-update-checks-use-weak-references).

## What espos_start() does, in order

```c
#include "espos.h"
void app_main(void) { ESP_ERROR_CHECK(espos_start(NULL)); /* your application */ }
```

| Step | Call | Why at this point |
|---|---|---|
| 1 | `espos_log_init()` | So the boot itself shows up in `/api/v1/logs`. |
| 2 | `espos_config_init()` | Everything after this reads its settings. |
| 3 | `espos_health_policy_start()` | The device watchdog, armed before anything can hang ([health](health.md)), unless it is turned off (below) or fails to start. `espos_core` also starts listening for `NETWORK_UP` and `NETWORK_DOWN` here, to raise and clear the `netDown` warning. Then `espos_init()` posts `CONFIG_READY`. |
| | your `before_network` hook | For anything that must exist before someone connects, such as a display that shows the setup network's name. |
| 4 | `espos_httpd_start()` | The setup page has to be there as soon as there is a link or a setup network. Posts `HTTPD_STARTED`. |
| 5 | `espos_net_start()` | Device id, hostname (`net.hostname`), `/net/status` and the mDNS responder (`<hostname>.local`, [net](net.md)). It comes before WiFi and Ethernet so the hostname is set on every interface. Posts `NETWORK_UP` and `NETWORK_DOWN`. |
| 6 | `espos_time_start()` | The clock ([time](time.md)). It keeps a time carried through deep sleep and starts SNTP on the first `NETWORK_UP`. |
| 7 | `espos_wifi_start()` | Station, setup network and `/wifi`, when WiFi is in the build and `CONFIG_ESPOS_WIFI` is on. Every chip with a radio or a radio co-processor has the component; the H-series chips don't. |
| 8 | `espos_eth_start()` | Wired Ethernet, when the component is in and both `CONFIG_ETH_USE_ESP32_EMAC` (the chip's own EMAC) and `CONFIG_ESPOS_ETH` are on. It runs beside WiFi, and `espos_net` prefers the cable. |
| 9 | `espos_sk_start()` | Signal K, if built. It needs `espos_net` for discovery and the stream. Just before it, the application name is set, so the access request reads `<app> <hostname>`. It also sets the clock from `navigation.datetime` while the clock is unset. |
| 10 | `espos_ota_start()` | Updates, if built. Its API sits on the web server and it watches `espos_net` to decide when to confirm a new image. |
| 11 | `espos_ble_start()` | The Bluetooth gateway, if built. It signs in with the Signal K token. It only counts as built when both the component and `CONFIG_BT_BLUEDROID_ENABLED` are in. |
| 12 | `espos_prov_start()` | Bluetooth WiFi setup ([provisioning](provisioning.md)), if built with both `CONFIG_ESPOS_PROV` and `CONFIG_BT_BLUEDROID_ENABLED`, and only while no WiFi network is configured. |
| 13 | `espos_power_start()` | The deep-sleep duty cycle ([power](power.md)), if built. Last, because it waits for everything above. |

The time, Ethernet, Bluetooth, provisioning and power steps don't stop the
boot when they fail. The device logs a warning and comes up without that
part, because a device that serves its web page is more use than one that
restarts in a loop. A watchdog that fails to start doesn't stop it either.

`espos_init()` runs steps 1 to 3 and `espos_start_network()` runs 4 to 13,
for an application that wants to do its own work in between
([`two_phase_boot`](examples.md)). Calling a start function twice is harmless,
except `espos_prov_start()`, which returns `ESP_ERR_INVALID_STATE` while it
runs. To turn the watchdog off, clear
`CONFIG_ESPOS_CORE_HEALTH_WATCHDOG`, or set
`espos_start_opts_t.health_watchdog` to false. The option only works through
`espos_start()`, so a two-phase boot uses the Kconfig switch.

"If built" means the component is in your project. `espos_core` links
`espos_sk`, `espos_ota` and `espos_ble` only when you list them, so you pick
features by listing components, not by editing a start sequence.

If you call the start functions yourself in the wrong order, the call fails
with `ESP_ERR_INVALID_STATE` and logs what is missing, for example
`espos_wifi_start: call espos_httpd_start() first (or espos_start())`:

| Call | Needs first |
|---|---|
| `espos_httpd_start()` | config |
| `espos_net_start()` | config and the web server |
| `espos_time_start()` | config and the web server; call it after `espos_net_start()` or it can miss the first `NETWORK_UP` |
| `espos_wifi_start()` | the web server and `espos_net` |
| `espos_eth_start()` | `espos_net` |
| `espos_sk_start()` | `espos_net` |
| `espos_ota_start()` | the web server and `espos_net` |
| `espos_power_start()` | `espos_start_network()` |

On the serial monitor a device tells you where it is, in this order: the
espOS and app versions, the setup network to join (or the WiFi it joined),
the Signal K server it found, the access request, and finally
`approved, streaming`. [Getting started](getting-started.md#2-read-the-monitor)
shows the lines.

## Which task calls you

Most callbacks run on an espOS task, and some on whichever task did the
thing they report, which can be yours. The rule is the same everywhere: copy what you need, return quickly, and don't wait for
anything that might be waiting for the task you are on. Which task it is
matters when you take a lock:

| Callback | Runs on | Watch out for |
|---|---|---|
| `espos_config_subscribe` | the task that changed the setting, usually a web request | You may read and write config from it. |
| `espos_sk_subscribe` | the Signal K stream task | Strings are only valid during the call. Don't call anything that waits on the stream. |
| `espos_sk_put` result | the Signal K stream task | Same as above. |
| `espos_health_add_sink` | the task that raised the condition; when you register, the conditions so far are replayed on your task | You may raise conditions of your own. |
| `espos_net_subscribe` | the task of the link that changed; for WiFi, the `espos_wifi` task | The status is only valid during the call. |
| `espos_time_subscribe` | whatever called `espos_time_set()`: SNTP's task, the Signal K stream task, a web request, or your own code | Don't block. |
| `espos_httpd_register` handlers | the web server task | One task serves every request, so a slow handler stalls the web page. |
| `espos_httpd_sse_on_connect` | the web server task | Send the new client its first update and return. |
| `espos_log_set_notify` | a timer task | At most every 500 ms. You may log from it. |
| `espos_log_read` visitor | your task, with the log locked | Don't log from it. |
| `ESPOS_EVENT` handlers | the default event loop | WiFi uses the same task, so a handler that blocks stalls WiFi. |
| `espos_health` policy | the `esp_timer` task, every 10 s | Health sinks run there on that tick. |

Some calls block, so they need a task of your own:

* `espos_sk_http_*` and `espos_sk_get_value/meta` wait up to twice their
  timeout. Never call them from the stream task, an event handler, a
  Bluetooth callback or a web handler; [`two_phase_boot`](examples.md) shows
  a worker task for them.
* `espos_mdns_start/add_service/remove_service` can take a few milliseconds.
  Not from an event or web handler.
* `espos_wifi_refresh_rssi()` asks the radio co-processor on a P4 and waits
  for the answer, which can take the whole transport timeout when the link is
  stuck. Never from a display task or an event handler.

The calls that send things are safe from any task, callbacks included:
`espos_sk_publish_*`, `espos_sk_notify`, `espos_health_report`,
`espos_event_post`, `espos_net_get_status`, `espos_net_is_up`,
`espos_time_now_ms` and `espos_time_is_synced`. None of them waits on a
driver or the network.

Two notes for people writing transports or touching the clock code:
`espos_net_report()` runs the subscribers and posts the event before it
returns, so don't call it while holding a lock a subscriber might want. And
`espos_log` takes the `espos_time` lock while it holds its own, so nothing
inside `espos_time` may log under its lock.

## From a JSON file to the settings page

Every setting is written down once, in a JSON file that belongs to the
component that uses it. The component registers the file in its
`CMakeLists.txt` with `espos_config_add_descriptor(config/myns.json)`. The
build then makes three things from it:

* `espos_cfg_keys.h`, with a constant for every key, so code never types an
  NVS key by hand;
* the tables `espos_config` checks every write against: type, range,
  default, whether it is secret, whether it needs a restart;
* the schema at `GET /api/v1/config/schema`, which the web page turns into
  forms.

So a new setting is one JSON entry, and the constant, the check, the REST
API and the field on the settings page all follow from it.
[Config](config.md) has the file format and how to migrate a key.

## Events

espOS posts `ESPOS_EVENT` on the default esp_event loop. Subscribe with
`espos_event_subscribe(id, handler, arg)`, or `ESP_EVENT_ANY_ID` for all of
them.

| Event | When | Data |
|---|---|---|
| `CONFIG_READY` | `espos_init()` has loaded the settings and tried to arm the watchdog, if it is on | |
| `HTTPD_STARTED` | the web server runs | |
| `NETWORK_UP` | the device has a default route, or it moved to another link | `{ip, hostname}` |
| `NETWORK_DOWN` | the default route is gone. One per `NETWORK_UP`; moving to another link is a DOWN then an UP | |
| `MDNS_READY` | after every `NETWORK_UP` once mDNS runs, and once from `espos_mdns_start()` if the network is already up | |
| `SK_SERVER_SELECTED` | a Signal K server was picked or changed | `{host, port}` |
| `SK_TOKEN_APPROVED` | the server approved the device | |
| `SK_STREAM_CONNECTED` / `_DISCONNECTED` | the data stream opened or closed | |
| `OTA_AVAILABLE` | a newer firmware is on offer | `{version}` |
| `TIME_SYNCED` | something set the clock, every time | `{source, unix_ms}` |
| `PORTAL_UP` / `PORTAL_DOWN` | the setup network came up or went away. On a P4 WiFi and Bluetooth share one radio, so anything that scans should pause in between | |
| `UPDATE_STARTED` / `_FINISHED` | a firmware or radio co-processor update began or ended, successful or not. Bluetooth scanning pauses in between | |

The full names start with `ESPOS_EVENT_`, and the data types are
`espos_event_network_t`, `espos_event_sk_server_t`, `espos_event_ota_t` and
`espos_event_time_t`.

An event tells you something changed; it doesn't tell you the state. If
your code starts late, ask the component first (`espos_net_get_status()`,
`espos_wifi_get_status()`, `espos_sk_get_server()`, `espos_ota_status_json()`)
and use events for changes after that. Handlers run on the event loop task,
so the rules above apply. New events are only ever added at the end, so the
ids don't change.

## Coming from SensESP

SensESP wires sensors, transforms and outputs into a graph. espOS can do the
same with [`espos_flow`](flow.md), or you can write plain C: a task that
reads, computes and calls `espos_sk_publish_*()`. Both send the same data.
[Migrating from SensESP](migration-from-sensesp.md) maps each SensESP class
to espOS and ports the `analog_input` example both ways.
