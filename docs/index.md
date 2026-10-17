# espOS

**espOS is the ESP-IDF-native successor to SensESP for Signal K devices.** It
is the plumbing every Signal K ESP32 device needs and nobody wants to write
again — WiFi with a provisioning portal, a persistent configuration store
with a generated settings UI, Signal K server discovery, access-token
handling and the delta stream in both directions, device health, and signed
over-the-air updates with rollback — as a set of ESP-IDF 6 components an
application sits on top of and calls through a small C API. It is
deliberately *not* a sensor framework: your code reads the sensor, espOS
gets the value onto the boat's network and keeps the device manageable from
a browser. espOS is a community project and not an official Signal K
repository; it lives at
[github.com/signalk-espOS/espOS](https://github.com/signalk-espOS/espOS)
under the Apache-2.0 license, and its components are published as
`signalk-espos/espos_*` on the Espressif Component Registry.

!!! tip "Ten-minute quick start"

    ESP-IDF 6.0.x installed and exported (`. $IDF_PATH/export.sh`), a board
    with an ESP32, -S3, -C3, -C6 or -P4, and a signalk-server on the same
    network — that is all. Every espOS example is a complete project:

    ```sh
    git clone https://github.com/signalk-espOS/espOS.git
    cd espOS/components/espos_core/examples/minimal
    idf.py set-target esp32c6            # or esp32 / esp32s3 / esp32c3 / esp32p4
    idf.py build flash monitor
    ```

    The monitor tells you what to do next: join the `espOS-xxxx` access
    point and pick your WiFi at `http://192.168.4.1`, then approve the
    device in signalk-server under **Security → Access Requests**. The
    device's own web UI is at `http://<hostname>.local` from then on, and
    its value is in the server's Data Browser. Step by step, with what each
    line of the monitor means: [Getting started](getting-started.md).

The whole of an application on espOS is one call; everything after it is
yours ([Concepts](concepts.md) has the order it runs in and the threading
rules):

```c
#include "espos.h"

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL)); /* log → config → httpd → wifi → sk → ota → ble */
    /* your application: espos_sk_publish_*, espos_sk_subscribe, espos_config_get_* */
}
```

## Components

Everything is an ESP-IDF component; an application depends on the ones it
needs and ignores the rest. The core — `espos_core`, `espos_config`,
`espos_httpd`, `espos_wifi`, `espos_log`, `espos_health`, `espos_event` —
is what "running espOS" means; `espos_sk`, `espos_ota` and `espos_ble` are
started by `espos_start()` when they are in the build; the rest are started
by the application when its board has the hardware.

| Component | What it gives you | Docs |
|---|---|---|
| `espos_core` | `espos_start()`: one call brings everything up in the order that works | [Concepts](concepts.md) |
| `espos_config` | NVS config store, JSON-Schema descriptors, REST-backed settings | [Configuration store](config.md) |
| `espos_httpd` | HTTP server, REST API, SSE, the web UI from a LittleFS partition | [REST API](rest-api.md) · [Web UI](ui.md) |
| `espos_net` | Interface-agnostic network status and default route, mDNS responder, device id; WiFi/Ethernet plug in underneath | [net.md](net.md) |
| `espos_wifi` | Station + provisioning portal, a pure-C state machine, co-processor watchdog | [WiFi and mDNS](wifi.md) |
| `espos_log` | Log ring served over REST, so a device is debuggable without a serial cable | [REST API → Logs](rest-api.md#logs-m5) |
| `espos_health` | Device conditions (warn/alarm), the sinks that consume them, the watchdog policy | [Device health](health.md) |
| `espos_event` | `ESPOS_EVENT` on the default event loop: config ready, network up, server found, token approved, update available | [Concepts → Events](concepts.md#events) |
| `espos_sk` | Signal K: mDNS discovery, access token, delta stream in and out, HTTP to the server | [Signal K](signalk.md) |
| `espos_ota` | Signed OTA with rollback, from a URL or a version manifest | [OTA](ota.md) |
| `espos_ble` | BLE gateway to signalk-server's BLE provider API | [BLE gateway](ble.md) |
| `espos_n2k` | NMEA 2000 over TWAI + a candump TCP server | [NMEA 2000 gateway](n2k.md) |
| `espos_usb_storage` | A microSD card exported over USB to a chartplotter, with new records followed off it | [USB storage bridge](usb-storage.md) |
| `espos_audio` | The `AudioDriver` contract a board implements (header-only) | [Voice satellite](voice.md) |
| `espos_voice` | Wyoming voice satellite with esp-sr wake word | [Voice satellite](voice.md) |

Board-specific code — display HALs, audio codecs, pin maps — stays in the
application. espOS defines the contracts and never assumes a particular
board.

## Where to go

* **New to espOS:** [Getting started](getting-started.md), then
  [Concepts](concepts.md), then the [examples](examples.md) and the
  [tutorials](tutorials/first-sensor.md).
* **Coming from SensESP:** [Migrating from SensESP](migration-from-sensesp.md)
  maps producers, transforms and the config UI onto espOS; the
  [hardware](hardware.md) page says what to expect from the boards you
  already own.
* **Building a product on it:** the component pages are the contracts —
  [configuration](config.md), [WiFi](wifi.md), [Signal K](signalk.md),
  [OTA](ota.md), [health](health.md) — with the [REST API](rest-api.md) and
  the [C API](api-c.md) as the reference; [Security](security.md) states the
  threat model, [Releasing](releasing.md) how versions and the registry work.
* **Something is wrong:** [Troubleshooting](troubleshooting.md), by symptom.
* **Changing espOS itself:** [Development](development.md) (build, host
  tests, the public API rules) and the
  [decisions](decisions.md) taken so far; the
  [changelog](changelog.md) lists what is in the next release.

espOS is pre-1.0 (`version.txt` says where it is today): the core is done
and in use, host-tested and running
on ESP32-P4 hardware against signalk-server 2.31; the BLE, NMEA 2000 and
voice components serve firmware built on top.
