# espOS

A minimal, modern device runtime for SignalK-connected ESP32 hardware, built
natively on ESP-IDF 6.

espOS is the plumbing every SignalK ESP32 device needs and nobody wants to
rewrite: WiFi, persistent config, a web config UI, SignalK server discovery,
token acquisition, delta output, and OTA. It is deliberately **not** a sensor
framework — application code sits on top and calls a small API.

Targets: ESP32, ESP32-S3, ESP32-C3, ESP32-C6, ESP32-P4 — one codebase.
Toolchain: ESP-IDF pinned in [`.idf-version`](.idf-version). HTTP:
`esp_http_server`. Storage: NVS for config and secrets.

espOS lives at [github.com/signalk-espOS/espOS](https://github.com/signalk-espOS/espOS)
and publishes its components as `signalk-espos/espos_*`. It is a community
project, not an official Signal K repository.

## Mission

signalk-espOS puts ESP32 boards to work on boats without asking the owner to
be a developer. A boat owner picks a job, flashes a board from the browser
and gets signed updates over the air from then on. A developer starts from a
maintained base on the latest ESP-IDF and spends their time on what makes
their project different.

We hold ourselves to four things:

1. **Community first.** The [registry](https://github.com/signalk-espOS/registry)
   lists projects maintained by their developers. Anyone can add a project
   with one registry entry and no credentials; a maintainer reviews new
   entries.
2. **Upstream, not around.** A fix to the shared base lands in espOS so every
   project gets it. Forks are welcome when they feed back.
3. **One clean ecosystem.** A rebranded copy that only splits users and effort
   doesn't belong in the registry. We talk to its maintainer first; registry
   maintainers decide on the listing.
4. **Safe to update.** A device accepts only firmware signed with the key it
   was flashed with, so a project keeps one signing key for all its releases,
   and an update must leave a boat's devices working as before. A build made
   with a throwaway key installs over USB but takes no updates, and the
   manager says so.

## Status

espOS is pre-1.0; `version.txt` and the
[releases](https://github.com/signalk-espOS/espOS/releases) say where it is
today. The core is done and in use: config store, HTTP
server and REST API, WiFi state machine with captive portal, SignalK
discovery, access token and delta stream in both directions, web UI,
device-health notifications and signed OTA with rollback -- all host-tested
and running on ESP32-P4 hardware against signalk-server 2.31; the BLE,
NMEA 2000 and voice components serve firmware built on top. What comes next
is in [docs/roadmap.md](docs/roadmap.md); the decisions taken so far are in
[docs/decisions.md](docs/decisions.md).

## Quick start

```sh
. $IDF_PATH/export.sh                # ESP-IDF 6.0.x, the release in .idf-version
idf.py set-target esp32c6            # or esp32 / esp32s3 / esp32c3 / esp32p4
idf.py build flash monitor           # on a shared or small host: scripts/build.sh build
# the monitor says what to do next: join the "espOS-xxxx" access point and open
# http://192.168.4.1 to pick a WiFi; then approve the device in signalk-server
# (Security → Access Requests). Web UI afterwards: http://<hostname>.local
```

The web UI bundle is committed, so Node is not needed to build a device that
serves it. `scripts/build.sh` wraps `idf.py` with a machine-wide lock and a
capped job count for hosts that freeze under a full parallel build.

### Or from the component registry

Every component is published to the [Espressif Component
Registry](https://components.espressif.com/components?q=signalk-espos) under
the `signalk-espos` namespace, so a firmware can use espOS without cloning
it:

```sh
idf.py add-dependency "signalk-espos/espos_sk^0.9.0"
```

`espos_sk` names the rest of the core as its own dependencies, so that one
line installs what a SignalK device needs. Keep the `^` range: espOS is
pre-1.0, where a minor bump does the work a major will do later, so an
unpinned dependency would take the next one unannounced. The components are
released in lockstep — one version of any of them works with the same
version of every other.

That line is the dependency; a firmware needs three more things, because a
registry consumer has no `espos_project_prologue()` to set them (the prologue
must run *before* `project()`, and a component's `project_include.cmake` runs
after, so it cannot ship as a component):

1. **A root `CMakeLists.txt`** — plain IDF, plus the web UI partition:

   ```cmake
   cmake_minimum_required(VERSION 3.22)
   include($ENV{IDF_PATH}/tools/cmake/project.cmake)
   project(my_firmware)
   espos_project_ui_partition()          # after project(), always
   ```

2. **A partition table** with two OTA slots and a `storage` partition, which
   IDF's single-app default has neither of. Copy one of the bundled tables
   rather than pointing into `managed_components/`, which is a build artefact:

   ```sh
   cp managed_components/signalk-espos__espos_core/partitions/4mb.csv partitions.csv
   ```

3. **`sdkconfig.defaults`** — the task stacks, core dump, OTA rollback, image
   signing, that partition table and a matching `CONFIG_ESPTOOLPY_FLASHSIZE_*`.
   You do not have to guess: `espos_core` checks at configure time and prints
   every missing line with the reason it matters.

Then generate the app-signing key once — it is deliberately **not** created for
you, because a key invented by a build step is a key nobody kept, and a device
accepts an OTA only from the key it was flashed with ([docs/ota.md](docs/ota.md)):

```sh
espsecure generate-signing-key --version 2 --scheme rsa3072 secure_boot_signing_key.pem
```

[`components/espos_core/examples/from_registry`](components/espos_core/examples/from_registry)
is all of the above as a working project, built by CI on every change.

The whole of an application on espOS is one call; everything else is yours
([docs/concepts.md](docs/concepts.md) has the order and the threading rules):

```c
#include "espos.h"

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL)); /* log → config → httpd → wifi → sk → ota → ble */
    /* your application: espos_sk_publish_*, espos_sk_subscribe, espos_config_get_* */
}
```

Docs: **[signalk-espos.github.io/espOS](https://signalk-espos.github.io/espOS/)** —
[Getting started](docs/getting-started.md) · [Concepts](docs/concepts.md) ·
[Examples](docs/examples.md) · [REST API contract](docs/rest-api.md) · [Config store &
descriptors](docs/config.md) · [WiFi](docs/wifi.md) · [SignalK
discovery & token](docs/signalk.md) · [OTA & signing](docs/ota.md) ·
[Web UI](docs/ui.md) · [Device health](docs/health.md) · [BLE gateway](docs/ble.md) · [NMEA 2000
gateway](docs/n2k.md) · [Voice satellite](docs/voice.md) · [Hardware](docs/hardware.md) ·
[Troubleshooting](docs/troubleshooting.md) ·
[Development & host tests](docs/development.md) · [Releasing](docs/releasing.md) · [Security
notes](docs/security.md)

## Components

Everything is an ESP-IDF component; an application depends on the ones it
needs and ignores the rest. The core four are what "running espOS" means;
the rest are optional.

| Component | What it gives you | Docs |
|---|---|---|
| `espos_core` | `espos_start()`: brings up everything below in the right order | [concepts.md](docs/concepts.md) |
| `espos_config` | NVS config store, JSON-Schema descriptors, REST-backed settings | [config.md](docs/config.md) |
| `espos_httpd` | HTTP server, REST API, SSE, the web UI from a LittleFS partition | [rest-api.md](docs/rest-api.md) · [ui.md](docs/ui.md) |
| `espos_net` | Interface-agnostic network status and default route, mDNS responder, device id; WiFi/Ethernet plug in underneath | [net.md](docs/net.md) |
| `espos_wifi` | Station + provisioning portal, a pure-C state machine, co-processor watchdog | [wifi.md](docs/wifi.md) |
| `espos_log` | Log ring served over REST, so a device is debuggable without a serial cable | — |
| `espos_health` | Device conditions (warn/alarm) and the sinks that consume them | [health.md](docs/health.md) |
| `espos_sk` | SignalK: mDNS discovery, access token, delta stream in and out | [signalk.md](docs/signalk.md) |
| `espos_ota` | Signed OTA with rollback, from a URL or a version manifest | [ota.md](docs/ota.md) |
| `espos_event` | The `ESPOS_EVENT` base other components post their state on | [concepts.md](docs/concepts.md) |
| `espos_time` | SNTP, wall-clock timestamps on deltas and log lines | [time.md](docs/time.md) |
| `espos_eth` | Ethernet as an `espos_net` transport (P4 EMAC, W5500 over SPI) | [net.md](docs/net.md) |
| `espos_flow` | Typed data-flow graph, timer wheel, one loop task | [flow.md](docs/flow.md) |
| `espos_formulas` | Marine maths with no IDF dependency: curves, dew point, densities | [transforms.md](docs/transforms.md) |
| `espos_sensors` | ADC, GPIO, pulse counter, PWM, I2C, 1-Wire as flow nodes | [sensors.md](docs/sensors.md) |
| `espos_sk_flow` | SignalK output, listener and PUT handler as flow nodes | [signalk.md](docs/signalk.md) |
| `espos_devices` | Whole devices composed from the above: tank level, engine RPM, … | [devices.md](docs/devices.md) |
| `espos_power` | Deep-sleep duty cycle: wake, publish, flush, sleep | [power.md](docs/power.md) |
| `espos_prov` | BLE provisioning: WiFi credentials from a phone over GATT | [provisioning.md](docs/provisioning.md) |
| `espos_ble` | BLE gateway | [ble.md](docs/ble.md) |
| `espos_n2k` | NMEA 2000 over TWAI + a candump TCP server | [n2k.md](docs/n2k.md) |
| `espos_sdlog` | A microSD card over SDMMC or SPI, and sessions of rows on it that survive a power cut | [sdlog.md](docs/sdlog.md) |
| `espos_audio` | The `AudioDriver` contract a board implements (header-only) | [voice.md](docs/voice.md) |
| `espos_voice` | Wyoming voice satellite with esp-sr wake word | [voice.md](docs/voice.md) |

Board-specific code — display HALs, audio codecs, pin maps — stays in the
application. espOS defines the contracts and never assumes a particular
board; `espos_audio::AudioDriver` is the pattern to copy when something
similar is needed.

## Layout

```
components/espos_*/        one directory per component (table above), each with
                           include/ src/ and, where it has one, examples/
main/                      example app
tools/                     generators
test/host/                 linux-target tests (no hardware needed)
test/fuzz/                 libFuzzer harnesses for the network-facing parsers
docs/                      contracts and guides
```

## License

espOS is open source under the [Apache License 2.0](LICENSE); every source
file carries an SPDX header naming the license and the tree is
[REUSE](https://reuse.software)-compliant. Contributions are accepted under the
same license with a `Signed-off-by` line (Developer Certificate of Origin).

Third-party components pulled in by the build (ESP-IDF, Espressif component
registry packages, `joltwallet/littlefs`, npm packages) keep their own
licenses; see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
