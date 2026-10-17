# Examples

Eleven complete ESP-IDF projects, one per thing a device built on espOS
does. Each lives under the component it shows off, builds on its own for
every supported target, and replaces a SensESP example — the table says
which, so a reader coming from SensESP knows where to look.

Levels: **Essential** — read first; the template a new firmware starts from.
**Newbie** — one espOS API in a sensor-sized program. **Advanced** —
threading, boot order, the server's REST tree, TLS.

| Example | Level | What it shows | Replaces (SensESP) | Path |
|---|---|---|---|---|
| `minimal` | Essential | `espos_start(NULL)` and one published value per second | `minimal_app`, `constant_sensor` | `components/espos_core/examples/minimal` |
| `from_registry` | Essential | The same application **without the prologue** — espOS installed from the component registry, as a project outside this repo does it | — | `components/espos_core/examples/from_registry` |
| `analog_input` | Essential | an ADC reading on a Signal K path | `analog_input` | `components/espos_sk/examples/analog_input` |
| `custom_settings` | Newbie | settings from a JSON descriptor, applied live, one migration | `ConfigItem` | `components/espos_config/examples/custom_settings` |
| `health_and_led` | Newbie | a status LED from the health table; a fatal condition and the watchdog policy | `SystemStatusLed` | `components/espos_health/examples/health_and_led` |
| `pulse_counter` | Newbie | pulses on a pin → frequency / RPM | `rpm_counter`, `pcnt_rpm_counter` | `components/espos_sk/examples/pulse_counter` |
| `digital_switch` | Newbie | a GPIO output switched from Signal K | `smart_switch` | `components/espos_sk/examples/digital_switch` |
| `two_phase_boot` | Advanced | `espos_init()` / `espos_start_network()`, a worker task, a blocking REST lookup | `freertos_tasks` | `components/espos_core/examples/two_phase_boot` |
| `app_endpoint_and_page` | Advanced | REST endpoints, an SSE event, a page in the web UI | frontend plugins | `components/espos_httpd/examples/app_endpoint_and_page` |
| `listener_relay` | Newbie | subscribe to a path, drive a relay | `listener` | `components/espos_sk/examples/listener_relay` |
| `json_and_meta` | Advanced | publish a JSON value, declare metadata for a path of your own | `raw_json`, the metadata example (SensESP #501) | `components/espos_sk/examples/json_and_meta` |
| `tls_server` | Advanced | https/wss to the server (`CONFIG_ESPOS_SK_TLS`) | `ssl_connection` | `components/espos_sk/examples/tls_server` |
| `ble_gateway` | Advanced | a BLE→Signal K gateway: the whole firmware is `espos_start(NULL)`, the configuration is the content | — (SensESP has no equivalent) | `components/espos_ble/examples/ble_gateway` |
| `n2k_candump` | Advanced | an NMEA 2000→Signal K gateway: CAN frames out as candump ASCII over TCP, decoded by canboatjs on the server | — (SensESP has no equivalent) | `components/espos_n2k/examples/n2k_candump` |
| `n2k_switch_bank` | Advanced | an NMEA 2000 switch bank any MFD can switch: the device claims its own address and answers as a node (PGN 127501/127502), with the candump server on the same bus | SensESP + the NMEA2000 library by hand | `components/espos_n2k/examples/n2k_switch_bank` |
| `ethernet` | Newbie | an espOS device on a cable: the whole firmware is `espos_start()`, WiFi off, the route on wired Ethernet | — (SensESP's `p4_ethernet`) | `components/espos_eth/examples/ethernet` |
| `usb_storage_follow` | Advanced | the board's microSD as a USB stick for a chartplotter, with espOS following the files the plotter appends to (ESP32-P4 and ESP32-S3 only: it needs USB OTG) | — (SensESP has no equivalent) | `components/espos_usb_storage/examples/usb_storage_follow` |
| `duty_cycle` | Advanced | a device on a battery: wake, publish, flush, deep-sleep, again — and how it stays reachable | — (SensESP has no equivalent) | `components/espos_power/examples/duty_cycle` |
| `ble_provisioning` | Advanced | WiFi credentials handed over BLE instead of the setup portal, with the Python client that speaks it | — (SensESP has no equivalent) | `components/espos_prov/examples/ble_provisioning` |

The three below are the same ideas as a **data-flow graph**, in C++. Nothing
needs them — a firmware is free to stay with the C examples above, which is
what the graph is sugar over — but each replaces a task, a queue or a
hand-written loop with a line of wiring.

| Example | Level | What it shows | Replaces (SensESP) | Path |
|---|---|---|---|---|
| `sensor_graph` | Newbie | the whole of a sensor firmware as four lines of wiring | `analog_input` as a graph | `components/espos_flow/examples/sensor_graph` |
| `dusk_relay` | Newbie | a value from the server through a hysteresis to a relay, with no queue and no worker task | `listener` + `Hysteresis` | `components/espos_sk_flow/examples/dusk_relay` |
| `smart_switch` | Advanced | a relay a phone can switch and a button that switches it back: inbound PUT | `smart_switch` with `SKPutRequest` | `components/espos_sk_flow/examples/smart_switch` |

## How to build one

An example is an ordinary ESP-IDF project. From its directory, with the IDF
environment exported (`. $IDF_PATH/export.sh` — the release named in
`.idf-version`):

```sh
cd components/espos_core/examples/minimal
idf.py set-target esp32c6                 # esp32 / esp32s3 / esp32c3 / esp32c6 / esp32c5 / esp32p4
idf.py build flash monitor
```

On a shared or small host, `scripts/build.sh` runs the same build under one
machine-wide lock, at half the cores, niced (docs/development.md):

```sh
cd components/espos_core/examples/minimal
../../../../scripts/build.sh -DIDF_TARGET=esp32c6 build      # build/ in the example directory
../../../../scripts/build.sh flash monitor
```

### An example signs with its own key

Each example is a project in its own right, so the first build in it
generates a signing key there (`secure_boot_signing_key.pem`, git-ignored)
and signs with that — except `from_registry`, which has no prologue to
generate one and tells you to run
`espsecure generate-signing-key --version 2 --scheme rsa3072 secure_boot_signing_key.pem`
before the first configure instead. The key is unique to that directory: an image built
here is **refused by a device that was flashed from anywhere else**, with
`esp_ota_ops: New image failed verification`.

That matters only when updating a device over the air. Flashing over USB
accepts any image, and the device then trusts whichever key that build
carried. To build an update a *particular* device will take, name its key:

```cmake
espos_project_prologue(... SIGNING_KEY "/path/to/that/devices/key.pem")
```

[ota.md](../docs/ota.md) has the whole story, including how to tell which key
an image carries.

One build directory per target, with its own sdkconfig — what CI and the
multi-target recipe in docs/development.md do:

```sh
../../../../scripts/build.sh -B build-esp32p4 -DSDKCONFIG=build-esp32p4/sdkconfig -DIDF_TARGET=esp32p4 build
```

The first configure warns that it generated a development app-signing key
(`secure_boot_signing_key.pem`, git-ignored): right for a bench, docs/ota.md
before a device leaves it. `idf.py flash` writes the app, the bootloader, the
partition table and the web UI bundle (`storage.bin`).

## What every example has

`CMakeLists.txt` includes the shared prologue (`cmake/espos_project.cmake`):
the IDF version policy, espOS's sdkconfig defaults, the partition table
(4 MB unless the README says otherwise), the components and the signing key
come from there, and nothing of it is copied. `main/CMakeLists.txt` names
the espOS components the example uses — projects on espOS build with IDF's
`MINIMAL_BUILD`, so naming a component is what puts it in the build, and
`espos_start()` starts what is built. `main/idf_component.yml` carries the
IDF pin and the two ESP32-P4 co-processor entries. `main/main.c` is at most
120 lines, comments included, and says why at every step that has a why.

Outside this tree an example is a firmware: copy it, add espOS as the
`espos/` submodule, point the include at `espos/cmake/espos_project.cmake`
(docs/development.md, "Building a firmware on espOS"). `minimal` is the one
to copy.
