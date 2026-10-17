# C API

The headers under `components/*/include` are espOS's public API, and their C
ABI is the stable contract every consumer gets — a firmware written in C, a
C++ application, or a binding generated from the headers as they are
([decisions](decisions.md), 2026-09-07). The pages in this section are
generated from those headers by Doxygen on every build of the site, so they
are as current as the branch they were built from; the prose that explains
*why* an API is shaped the way it is stays in the header itself and in the
component pages.

`ESPOS_ABI_VERSION` (`espos.h`, which also carries the history of what changed
at each value) is bumped by any change to a header that is not purely additive;
`espos_abi_version()` returns the value the linked `espos_core` was built with.
The rules the headers follow — `esp_err.h` as the only IDF include (two frozen
exceptions), opaque handles, fixed-width integers, callbacks with a trailing
`void *arg`, no `CONFIG_` in new headers — are in
[Development → Public API rules](development.md#public-api-rules),
and `tools/check_public_headers.py` checks them in CI.

## Reading the reference

* [**Headers**](api-c/files.md) is the entry point: one page per header,
  in the order of the include directories, each with the file-level comment
  (what the component is, which task calls back), its functions, types and
  macros, and a link to the source.
* [**Functions**](api-c/functions.md), [**Types and
  variables**](api-c/variables.md) and [**Macros**](api-c/macros.md) are
  the flat indexes across every header.
* [**Structures**](api-c/annotated.md) lists every `struct` (and the classes
  of the C++ components).

Every callback's threading contract is stated in its header and collected in
[Concepts → Threading contracts](concepts.md#threading-contracts); read that
table before calling anything from inside a callback.

## Headers by component

| Header | Component | What it is |
|---|---|---|
| [`espos.h`](api-c/espos_8h.md) | `espos_core` | `espos_start()`, `espos_init()`, `espos_start_network()`, versions, `ESPOS_ABI_VERSION` |
| [`espos_event.h`](api-c/espos__event_8h.md) | `espos_event` | `ESPOS_EVENT` base on the default loop: ids, payload structs, post/subscribe |
| [`espos_config.h`](api-c/espos__config_8h.md) | `espos_config` | the store: typed get/set, subscribe, export/import, migrations, factory reset |
| [`espos_config_desc.h`](api-c/espos__config__desc_8h.md) | `espos_config` | the descriptor tables the build-time generator instantiates |
| [`espos_config_backend.h`](api-c/espos__config__backend_8h.md) | `espos_config` | the storage backend interface (NVS on a device, memory on the host) |
| [`espos_log.h`](api-c/espos__log_8h.md) | `espos_log` | the in-RAM log ring behind `/api/v1/logs` |
| [`espos_health.h`](api-c/espos__health_8h.md) | `espos_health` | conditions, sinks, watched tasks, the reset record |
| [`espos_health_policy.h`](api-c/espos__health__policy_8h.md) | `espos_health` | the watchdog policy as a pure C state machine over a port |
| [`espos_httpd.h`](api-c/espos__httpd_8h.md) | `espos_httpd` | the HTTP server, `espos_httpd_register()` for application endpoints |
| [`espos_httpd_sse.h`](api-c/espos__httpd__sse_8h.md) | `espos_httpd` | publishing named events on `GET /api/v1/events` |
| [`espos_httpd_auth_policy.h`](api-c/espos__httpd__auth__policy_8h.md) | `espos_httpd` | the pure authentication policy (sessions, throttle, request verdict) behind a port; `espos_httpd_register_ex()` and `espos_httpd_request_authenticated()` are in `espos_httpd.h` |
| [`espos_wifi.h`](api-c/espos__wifi_8h.md) | `espos_wifi` | station manager, status, portal, `espos_wifi_short_id()` |
| [`espos_wifi_sm.h`](api-c/espos__wifi__sm_8h.md) | `espos_wifi` | the WiFi state machine and its port (host-testable) |
| [`espos_net.h`](api-c/espos__net_8h.md) | `espos_net` | the interface-agnostic network seam: default route status, subscriptions, transport registration, the device short id |
| [`espos_net_sm.h`](api-c/espos__net__sm_8h.md) | `espos_net` | the pure default-route selection behind a port (host-tested) |
| [`espos_mdns.h`](api-c/espos__mdns_8h.md) | `espos_net` | the mDNS responder: `espos_mdns_add_service()`, readiness |
| [`espos_sk.h`](api-c/espos__sk_8h.md) | `espos_sk` | discovery, token, `espos_sk_publish_*`, subscribe, PUT, notify |
| [`espos_sk_http.h`](api-c/espos__sk__http_8h.md) | `espos_sk` | HTTP to the selected server: GET/PUT/POST/DELETE, value and meta lookups, URLs |
| [`espos_sk_delta.h`](api-c/espos__sk__delta_8h.md) | `espos_sk` | delta batcher and offline ring (pure C) |
| [`espos_sk_parse.h`](api-c/espos__sk__parse_8h.md) | `espos_sk` | stream frame parser (pure C) |
| [`espos_sk_token_sm.h`](api-c/espos__sk__token__sm_8h.md) | `espos_sk` | the access-token state machine over a port |
| [`espos_ota.h`](api-c/espos__ota_8h.md) | `espos_ota` | signed OTA with rollback: start, status, check, install, confirm |
| [`espos_ota_manifest.h`](api-c/espos__ota__manifest_8h.md) | `espos_ota` | manifest parsing and version comparison (pure C) |
| [`espos_ble.h`](api-c/espos__ble_8h.md) | `espos_ble` | the BLE gateway: start, status |
| [`espos_prov.h`](api-c/espos__prov_8h.md) | `espos_prov` | BLE provisioning: WiFi credentials over GATT, written to config |
| [`espos_eth.h`](api-c/espos__eth_8h.md) | `espos_eth` | wired Ethernet as an espos_net transport: start, stop, link |
| [`espos_power.h`](api-c/espos__power_8h.md) | `espos_power` | the deep-sleep duty cycle: start, holds, sleep now, status |
| [`espos_power_policy.h`](api-c/espos__power__policy_8h.md) | `espos_power` | when a duty-cycling device may sleep (pure C) |
| [`espos_flow.h`](api-c/espos__flow_8h.md) | `espos_flow` | the loop task, timers and mailbox, callable from plain C |
| [`espos_sched.h`](api-c/espos__sched_8h.md) | `espos_sched` | the wrap-safe timer wheel over an injected clock (pure C) |
| [`espos_adc.h`](api-c/espos__adc_8h.md) | `espos_sensors` | calibrated one-shot ADC reads, in volts |
| [`espos_gpio_in.h`](api-c/espos__gpio__in_8h.md) | `espos_sensors` | debounced GPIO input, level and edge |
| [`espos_pcnt.h`](api-c/espos__pcnt_8h.md) | `espos_sensors` | pulse counting over PCNT, with a GPIO-ISR fallback where the SoC has no unit |
| [`espos_pwm.h`](api-c/espos__pwm_8h.md) | `espos_sensors` | LEDC PWM output |
| [`espos_i2c_bus.h`](api-c/espos__i2c__bus_8h.md) | `espos_sensors` | a shared i2c_master bus for breakout drivers |
| [`espos_onewire.h`](api-c/espos__onewire_8h.md) | `espos_sensors` | 1-Wire bus and DS18B20 (optional, `CONFIG_ESPOS_SENSORS_ONEWIRE`) |
| [`espos_sensor_math.h`](api-c/espos__sensor__math_8h.md) | `espos_sensors` | the sensor arithmetic the drivers share (pure C, host-tested) |

The following headers are **C++ interfaces**, by design
([decisions](decisions.md)): public, but not part of the C ABI until they get
C wrappers. `tools/check_public_headers.py` lists them as `CPP_ONLY`.

| Header | Component | What it is |
|---|---|---|
| [`espos_audio/audio_driver.h`](api-c/audio__driver_8h.md) | `espos_audio` | `AudioDriver`, the contract a board's codec implements |
| [`espos_audio/null_audio.h`](api-c/null__audio_8h.md) | `espos_audio` | `NullAudio`, the no-op driver for boards without audio |
| [`espos_flow/flow.hpp`](api-c/flow_8hpp.md) | `espos_flow` | the umbrella header: graph, nodes and transforms in one include |
| [`espos_flow/graph.hpp`](api-c/graph_8hpp.md) | `espos_flow` | `Graph`, node ownership and `make<T>()` |
| [`espos_flow/node.hpp`](api-c/node_8hpp.md) | `espos_flow` | `Producer<T>`, `Consumer<T>`, `Transform<In,Out>` and the edge pool |
| [`espos_flow/nodes.hpp`](api-c/nodes_8hpp.md) | `espos_flow` | `Poll`, `Lambda`, `Join`, `Sink`, `Value`, `Constant`, `Ticker`, `Mailbox` |
| [`espos_flow/transforms.hpp`](api-c/transforms_8hpp.md) | `espos_flow` | every transform node in one include |
| [`espos_formulas.hpp`](api-c/espos__formulas_8hpp.md) | `espos_formulas` | the umbrella header: units, curves and marine formulas |
| [`espos_formulas/units.hpp`](api-c/units_8hpp.md) | `espos_formulas` | `constexpr` SI conversions for the units the Signal K spec uses |
| [`espos_formulas/curve.hpp`](api-c/curve_8hpp.md) | `espos_formulas` | piecewise-linear interpolation over a sample table |
| [`espos_formulas/marine.hpp`](api-c/files.md) | `espos_formulas` | dew point, heat index, air density, dividers, tank level, battery charge |
| [`espos_sensors/sensors.hpp`](api-c/sensors_8hpp.md) | `espos_sensors` | `Analog`, `GpioState`, `GpioChange`, `PulseCounter`, `GpioCounter`, `GpioOutput`, `Pwm` |
| [`espos_sensors/system.hpp`](api-c/system_8hpp.md) | `espos_sensors` | the device's own numbers as producers: heap, uptime, reset reason |
| [`espos_sensors/onewire.hpp`](api-c/onewire_8hpp.md) | `espos_sensors` | `OneWireBus` and `Ds18b20` |
| [`espos_sk_flow/sk.hpp`](api-c/sk_8hpp.md) | `espos_sk_flow` | `Output`, `Meta`, `Listener`, `PutHandler`, `PutRequest`, `Notify`, `NetRssi`, `IpAddress` |
| [`espos_devices.hpp`](api-c/espos__devices_8hpp.md) | `espos_devices` | the umbrella header: every device class in one include |
| [`espos_devices/tank.hpp`](api-c/tank_8hpp.md) | `espos_devices` | `TankLevel`: a resistive sender, volts → ohms → ratio |
| [`espos_devices/engine.hpp`](api-c/engine_8hpp.md) | `espos_devices` | `EngineRpm` (revolutions per **second**), `EngineHours` |
| [`espos_devices/switches.hpp`](api-c/switches_8hpp.md) | `espos_devices` | `SmartSwitch` (server-operable relay), `BilgeSwitch` |
| [`espos_devices/temperature.hpp`](api-c/temperature_8hpp.md) | `espos_devices` | `OneWireTemperature` (opt-in, `CONFIG_ESPOS_SENSORS_ONEWIRE`) |
| [`espos_n2k/can_frame.h`](api-c/can__frame_8h.md) | `espos_n2k` | `CanMessage`, espOS's own CAN frame struct |
| [`espos_n2k/twai_receiver.h`](api-c/twai__receiver_8h.md) | `espos_n2k` | `TwaiReceiver`: owns the bus, emits frames on its task |
| [`espos_n2k/twai_transmitter.h`](api-c/twai__transmitter_8h.md) | `espos_n2k` | `TwaiTransmitter`: joins the receiver's bus |
| [`espos_n2k/candump_tcp_server.h`](api-c/candump__tcp__server_8h.md) | `espos_n2k` | candump-format TCP server, advertised over mDNS |
| [`espos_n2k/candump_format.h`](api-c/candump__format_8h.md) | `espos_n2k` | candump ASCII encode/decode (host-tested) |
| [`espos_n2k/twai_message.h`](api-c/twai__message_8h.md) | `espos_n2k` | compatibility alias `TwaiMessage` → `CanMessage` |
| [`espos_usb_storage/usb_storage.h`](api-c/usb__storage_8h.md) | `espos_usb_storage` | export the microSD card over USB, write tracking, `follow()` |
| [`espos_usb_storage/follower.h`](api-c/follower_8h.md) | `espos_usb_storage` | the follower that hands new records on, with its FS and progress ports (host-tested) |
| [`espos_voice/wyoming_satellite.h`](api-c/wyoming__satellite_8h.md) | `espos_voice` | the Wyoming satellite server |
| [`espos_voice/wake_engine.h`](api-c/wake__engine_8h.md) | `espos_voice` | esp-sr WakeNet wrapper |
| [`espos_voice/protocol/events.h`](api-c/events_8h.md), [`framing.h`](api-c/framing_8h.md) | `espos_voice` | the Wyoming wire protocol |

<!-- `marine.hpp` is the one basename that appears twice under the documented
     include directories (`espos_formulas/` and `espos_flow/transforms/`), so
     Doxygen disambiguates the generated page names itself and the row above
     points at the header index rather than a guessed filename. Replace it with
     the real `api-c/…md` page once the site has been built once. -->

## How the reference is built

`Doxyfile` at the repository root reads only `components/*/include`
(`EXTRACT_ALL`, so an undocumented declaration still appears; XML output
only). The site build runs it through the mkdoxy plugin (`mkdocs.yml`), which
writes the XML under the site output and renders these pages from it; nothing
is committed. Doxygen warnings name a header to fix and are printed by the
build but do not fail it. Standalone, from the repository root:

```sh
doxygen Doxyfile            # XML under build/doxygen/, warnings on stderr
```

Building the whole site locally is described in
[Development → Documentation site](development.md#documentation-site).
