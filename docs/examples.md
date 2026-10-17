# Examples

Every example is a complete ESP-IDF project of its own under
`components/<component>/examples/<name>/`, built the same way as any firmware
on espOS: it includes `cmake/espos_project.cmake`, calls
`espos_project_prologue()` and `espos_project_ui_partition()`, and its
`main.c` boots with `espos_start()`. What each one adds on top is the point
of the example and fits in about a hundred lines. Each README says what the
example does, what it needs wired, what appears in Signal K, and — for
readers coming from SensESP — which SensESP example it replaces, and carries
a label: **Essential** (read these first), **Newbie** (one concept, no
surprises) or **Advanced**.

The index in the repository is
[`examples/README.md`](https://github.com/signalk-espOS/espOS/blob/main/examples/README.md);
the table below is the same list with the page it pairs with on this site.

## Build any of them

```sh
cd components/espos_sk/examples/analog_input     # or any other example directory
idf.py set-target esp32c6                        # esp32 / esp32s3 / esp32c3 / esp32c6 / esp32c5 / esp32p4
idf.py build flash monitor
```

Every example builds for `esp32c6`; the ones that drive a peripheral also
build for `esp32p4`, with the pins as `#define`s at the top of `main.c`. On
a small or shared machine use the locked wrapper instead of a bare build:
`/path/to/espOS/scripts/build.sh build` from inside the example directory
([Development](development.md)). The first configure generates a
*development* signing key with a warning; that is expected
([OTA → Signing key](ota.md#signing-key)).

`usb_storage_follow` builds only for `esp32p4` and `esp32s3`, the targets with
the USB OTG port mass storage needs, and its `main.cpp` is C++ because the
follower API is.

`from_registry` is the exception, deliberately: it has no prologue, so nothing
generates a key for it and the build stops until you run
`espsecure generate-signing-key --version 2 --scheme rsa3072 secure_boot_signing_key.pem`
once, BEFORE the first configure. A key
invented by a build step is a key nobody kept, and a registry consumer's
project is not espOS's to put one in — see
[its README](https://github.com/signalk-espOS/espOS/tree/main/components/espos_core/examples/from_registry).

## The examples

| Example | Component | What it shows | Read with |
|---|---|---|---|
| [`minimal`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_core/examples/minimal) | `espos_core` | **Essential.** The whole of an espOS application: `espos_start(NULL)`, then `environment.inside.temperature` once a second, a constant (293.65 K) standing in for the sensor. No wiring. Replaces SensESP's `minimal_app` and `constant_sensor`. The [getting started](getting-started.md) target. | [Concepts](concepts.md) |
| [`from_registry`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_core/examples/from_registry) | `espos_core` | **Essential.** The same application as `minimal`, built the way a project outside this repository builds it: espOS from the component registry, a plain IDF root `CMakeLists.txt`, its own partition table and sdkconfig. Start your own firmware from this one. | [Releasing](releasing.md) |
| [`two_phase_boot`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_core/examples/two_phase_boot) | `espos_core` | **Advanced.** `espos_init()` and `espos_start_network()` instead of one call, with the application's own work between them, and the pattern for a blocking espOS call — a worker task fed through a queue. Replaces `freertos_tasks`. | [Concepts → espos_start()](concepts.md#espos_start-the-order-and-why), [Threading contracts](concepts.md#threading-contracts) |
| [`custom_settings`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_config/examples/custom_settings) | `espos_config` | **Newbie.** An application's own settings: declared once in `main/config/app.json`, used through generated key constants, applied live from the web UI, carried across a renamed key by a migration. Replaces SensESP's `ConfigItem`. | [Configuration store](config.md), [Add a setting](tutorials/add-a-setting.md) |
| [`app_endpoint_and_page`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_httpd/examples/app_endpoint_and_page) | `espos_httpd` | **Advanced.** A firmware's own REST endpoints on the espOS server, a live value on the SSE stream the web UI already listens to, and a page of your own in that UI. Replaces SensESP's frontend plugins. | [REST API](rest-api.md), [Web UI](ui.md), [An app endpoint and a UI tab](tutorials/app-endpoint-and-ui-tab.md) |
| [`health_and_led`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_health/examples/health_and_led) | `espos_health` | **Newbie.** A status LED driven from the device's health table, and an application condition the watchdog policy acts on — and what it never restarts for. Replaces SensESP's `SystemStatusLed`. | [Device health](health.md) |
| [`analog_input`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk/examples/analog_input) | `espos_sk` | **Essential.** One ADC pin read every second — factory-calibrated, averaged, scaled by two settings from the web UI — published to a Signal K path: the shape of every "sensor on a wire" device. Replaces SensESP's `analog_input`, `repeat_sensor_analog_input` and its tank-level tutorial. | [Your first sensor](tutorials/first-sensor.md), [Tank level](tutorials/tank-level.md) |
| [`pulse_counter`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk/examples/pulse_counter) | `espos_sk` | **Newbie.** Pulses on a GPIO counted by the PCNT peripheral with its glitch filter, published as revolutions per second, with a running total kept in the config store across reboots — engine RPM, shaft speed, chain or flow. Replaces `rpm_counter`, `pcnt_rpm_counter`, `chain_counter`, `time_counter`. | [Signal K → Delta stream](signalk.md#delta-stream-m4) |
| [`digital_switch`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk/examples/digital_switch) | `espos_sk` | **Newbie.** A GPIO output whose state is a Signal K switch path, toggled by a debounced button; published on change and every 10 s so a late dashboard still shows the truth. Replaces SensESP's `smart_switch`. | [Signal K → Inbound](signalk.md#inbound-m7) |
| [`listener_relay`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk/examples/listener_relay) | `espos_sk` | **Newbie.** A relay driven by a value the server streams: subscribe to `environment.outside.illuminance`, switch on below 50 lux and off above 100 (hysteresis), publish the relay's state back. Replaces SensESP's `listener`. | [Concepts → Threading contracts](concepts.md#threading-contracts) |
| [`json_and_meta`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk/examples/json_and_meta) | `espos_sk` | **Advanced.** The three calls a plain number does not cover, framed as a windlass controller: `espos_sk_publish_json()` for an object, `espos_sk_declare_meta()` for a path only this device knows, `espos_sk_notify()` for a condition of the device. No hardware. Replaces `raw_json` and the `metadata` example. | [Signal K → Delta stream](signalk.md#delta-stream-m4), [Device health](health.md) |
| [`tls_server`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk/examples/tls_server) | `espos_sk` | **Advanced.** The Signal K connection over https and wss: `CONFIG_ESPOS_SK_TLS` in the build, `sk.scheme` defaults to `auto` and `sk.tls_trust` to `tofu`, so a self-signed boat server is pinned on first use and works with no configuration | [Signal K → TLS](signalk.md#tls-https-wss), [Security](security.md) |
| [`sensor_graph`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_flow/examples/sensor_graph) | `espos_flow` | **Newbie.** The whole of a sensor firmware as four lines of wiring, in C++: a poll, a calibration, a Signal K sink, connected with `>>`. The same device as `analog_input` above, which is not obsolete — the graph is sugar over exactly those calls. A fake reading keeps it about the wiring and lets it build for every target. | [Data flow](flow.md), [Migrating from SensESP](migration-from-sensesp.md) |
| [`dusk_relay`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk_flow/examples/dusk_relay) | `espos_sk_flow` | **Newbie.** `listener_relay` as a graph: a value the server streams, through a hysteresis node, to a relay, published back. The queue and the worker task the C version needs are gone — `Listener` posts into a mailbox and the flow loop is the worker. | [Data flow](flow.md), [Transforms and formulas](transforms.md) |
| [`smart_switch`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk_flow/examples/smart_switch) | `espos_sk_flow` | **Advanced.** A relay a phone can switch and a button that switches it back: the example that could not be written before inbound PUT existed. Shows why a device must publish a path before the server will route a PUT to it, and the difference between a spec path and one of your own. | [Signal K → Inbound PUT](signalk.md#inbound-put-control), [Sensors](sensors.md) |
| [`ble_gateway`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_ble/examples/ble_gateway) | `espos_ble` | **Advanced.** A BLE-to-Signal K gateway, and a whole firmware in one call: advertisements batched to signalk-server's BLE provider API, GATT sessions driven by the server over a control WebSocket. The gateway decodes nothing — what a device *is* is decided by `bt-sensors-plugin-sk` on the server. Was the separate `espos-ble-gateway` repository until it became twenty lines; what is worth reading is its two sdkconfig fragments. | [BLE gateway](ble.md) |
| [`n2k_candump`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_n2k/examples/n2k_candump) | `espos_n2k` | **Advanced.** An NMEA 2000 gateway: CAN frames off the bus, out over TCP as candump ASCII for canboatjs. Decodes nothing on the device — PGN decoding changes more often than firmware should. Read it for the wiring, which is what actually goes wrong: a transceiver and termination are not optional, and both fail looking exactly like a software fault. `GET /api/v1/n2k` tells a quiet bus from a misconfigured one. | [N2K gateway](n2k.md) |
| [`n2k_switch_bank`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_n2k/examples/n2k_switch_bank) | `espos_n2k` | **Advanced.** The device as an NMEA 2000 node of its own: it claims an address, shows up in an MFD's device list, reports eight channels on PGN 127501 and switches them on 127502 from any MFD. The bank instance is the device instance, settable from the web UI or the MFD. The candump server runs on the same bus. | [N2K node](n2k.md#the-device-as-an-nmea-2000-node) |
| [`ethernet`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_eth/examples/ethernet) | `espos_eth` | **Newbie.** An espOS device on a cable, for the Waveshare ESP32-P4 PoE board: the whole firmware is `espos_start()`, WiFi is off, and SignalK, OTA and the web UI run over wired Ethernet. Read it for the power caution — USB and PoE never at once — and for checking the route over the network, since the serial console is out of reach once the board is on PoE. | [Network](net.md) |
| [`usb_storage_follow`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_usb_storage/examples/usb_storage_follow) | `espos_usb_storage` | **Advanced.** A Waveshare ESP32-P4 PoE board shows its microSD to a chartplotter as a USB stick, and espOS follows the RealBathy files the plotter appends to, handing each settled run of whole records to a sink. espOS never writes the card and reads it only while the plotter is quiet. Read it for the wiring: the host goes on the high-speed header, not the USB-C. | [USB storage bridge](usb-storage.md) |
| [`sd_logger`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sdlog/examples/sd_logger) | `espos_sdlog` | **Newbie.** A row a second onto a microSD card, in hourly sessions that survive a power cut: pull the plug at any moment and the next boot closes the cut session after its last complete row, with a row count and a sha256 a server can check an upload against. On a Waveshare P4 board the slot is on the board; any other chip takes a card over SPI. | [microSD logging](sdlog.md) |
| [`ble_provisioning`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_prov/examples/ble_provisioning) | `espos_prov` | **Advanced.** WiFi credentials handed to a sealed device over BLE, with no access point and no captive portal — for the cases the setup portal cannot serve. Ships the Python client that speaks the protocol, because Espressif's phone app does not: `espos_prov` uses protocomm as a transport only, so espOS's own WiFi state machine stays the one thing that owns the radio. Read the client's `srp6a.py` for the handshake, where ESP-IDF's own header comments are wrong. | [BLE provisioning](provisioning.md) |
| [`duty_cycle`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_power/examples/duty_cycle) | `espos_power` | **Advanced.** A device on a battery: wake, publish two readings, flush the stream, deep-sleep, again. The code is a few lines; read it for the rules that keep a mostly-sleeping device reachable — off until you turn it on, an awake window after every power-on or update, and no sleep while an update is unconfirmed, because every wake is a boot and would roll it back. | [Power](power.md) |

The reference application in `main/` at the repository root is not an
example but the app espOS's own CI builds on every target; it exercises every
descriptor type and every optional component, which is why it is larger than
any example.

## Where to go from an example

* Something in the runtime behaves unexpectedly: [Troubleshooting](troubleshooting.md).
* The example does nearly what you want: the [tutorials](tutorials/first-sensor.md)
  walk from an example to a device of your own, one addition at a time.
* You have a SensESP project: [Migrating from SensESP](migration-from-sensesp.md)
  maps its concepts onto these examples.
