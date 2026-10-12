# ble_gateway

A BLE → Signal K gateway. Scans for Bluetooth Low Energy devices and bridges
them to [signalk-server](https://github.com/SignalK/signalk-server)'s BLE
provider API: advertisements are batched over HTTP, and the server drives GATT
sessions (connect, subscribe, read, write) over a control WebSocket.

The whole application:

```c
void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));
}
```

This was a separate repository — `dirkwa/espos-ble-gateway` — until it became
twenty lines. It is folded in here because what is worth keeping is not the
code but the configuration around it, and because a firmware nobody can build
from the same tree is a firmware that quietly drifts out of step.

## The gateway decodes nothing

What a device *is* — a battery monitor, a tank sender, a thermometer — and how
its bytes become Signal K paths is decided on the server, by
[`bt-sensors-plugin-sk`](https://github.com/naugehyde/bt-sensors-plugin-sk).
That keeps the firmware small and means supporting a new sensor never involves
reflashing anything.

## Where the gateway comes from

Nothing in `main.c` starts it. `espos_start()` starts what is *in the build*,
and two build-time decisions put it there:

* `PRIV_REQUIRES espos_ble` in `main/CMakeLists.txt`, because consumers build
  with IDF's `MINIMAL_BUILD` and a component nothing requires is never
  compiled;
* `COMPONENTS espos_ble` in the prologue call, because the prologue excludes
  optional espOS components a project does not name. Without that list the
  component manager resolves every visible manifest before `MINIMAL_BUILD`
  trims the graph, and esp-sr and esp-dl are downloaded and compiled for a
  headless gateway that links none of them.

Adding a gateway to a firmware is therefore a build-file change, not a code
change.

## The two sdkconfig fragments are the point

`sdkconfig.defaults` carries two decisions that cost bring-up time to find:

**Bluedroid, not NimBLE**, because the GATT client is Bluedroid-based.

**BLE 4.2, not 5.0.** On the ESP32-P4 the C6 slave's HCI bridge does not
correctly forward BLE 5.0 extended HCI commands over SDIO, and legacy scan is
the known-working path — the same choice ESPHome's `bluetooth_proxy` makes.
Kept uniform across targets so behaviour does not diverge per board.

`sdkconfig.defaults.esp32p4` carries the one that is genuinely load-bearing:

**`CONFIG_BT_CONTROLLER_DISABLED=y` is what makes the host stack talk HCI to
the C6** through esp_hosted instead of looking for a local controller. And the
init order matters: the remote controller must be initialised and enabled
*before* `esp_bluedroid_attach_hci_driver()`, or the first use of the driver's
function pointers faults. That is handled in `espos_ble`'s bring-up; it is
recorded here because it is the kind of thing that reads as a hardware fault.

## Hardware

| Board | Radio | Status |
|---|---|---|
| Waveshare ESP32-P4 (+ ESP32-C6 over SDIO) | HCI at the C6 via esp_hosted | **in daily use**: two gateways, 4.3 M and 41 M advertisements forwarded |
| ESP32 / C3 / S3 / C6 | native Bluedroid | buildable, not verified on hardware |
| ESP32-C5 with PSRAM | native Bluedroid | **runs on one Waveshare C5-WIFI6-KIT-N16R8**: PSRAM on (`sdkconfig.defaults.esp32c5`); not yet soaked or tried with GATT |

**The ESP32-C5 needs its PSRAM.** Without it, BLE plus WiFi plus the SignalK
client comes to ~157 KB of the ~172 KB of internal RAM free at `app_main`, and the
BLE controller cannot find the 24 KB block it needs. `sdkconfig.defaults.esp32c5`
switches the module's 8 MB on and moves the Bluedroid host, small allocations and
TLS buffers into it. A C5 without PSRAM is not a BLE bridge. The measurements and
the rev v1.0 PSRAM-encryption caveat are in
[hardware.md](../../../../docs/hardware.md#the-esp32-c5-as-a-ble-gateway).

The ESP32-P4 has no radio of its own, so Bluetooth — like WiFi — runs over the
C6 co-processor. Nothing needs flashing on the C6: its stock firmware already
carries BLE with HCI over SDIO.

**On the P4, check the antenna first.** The ESP32-C6-MINI-**1U** module has no
PCB antenna. Without an external 2.4 GHz antenna on its IPEX connector the
radio sees nothing at all, which looks exactly like a software failure.

## Build

```sh
idf.py set-target esp32p4      # or esp32c6, esp32, esp32c3, esp32s3, esp32c5
idf.py build flash monitor
```

Needs ESP-IDF 6.0.x and nothing else — the espOS web UI is a committed bundle,
so there is no Node step.

This example uses `components/espos_core/partitions/16mb.csv` from espOS rather than a table of its
own. The standalone repository carried one, and its comment is worth repeating
if you fork this: a device keeps the partition table it was flashed with,
because an OTA replaces app slots and never the table, so changing tables
turns the next update into a USB reflash.

Built for esp32c6 and esp32p4, zero warnings. Run on the PoE gateway
(ESP32-P4 + C6) on 2026-09-15, in a build that also enabled `espos_prov` --
which is how the incompatibility in [provisioning.md](../../../../docs/provisioning.md)
was found.

### Updating a gateway that already exists

The first build here generated a signing key in this directory, and it is
not the key your gateway was flashed with — the standalone repository had
its own. An OTA built here is therefore **refused** by that device:

```
E (21719384) esp_image: Secure boot signature verification failed
E (21719547) espos_ota: install failed: image rejected: bad signature or corrupt
```

Nothing is harmed — the device rejects the image before writing anything and
carries on — but the update does not install. Build with the key that device
trusts:

```cmake
espos_project_prologue(NAME "ble-gateway"
                       SIGNING_KEY "/path/to/the/gateways/key.pem"
                       ...)
```

`espos_ota` reports only "signature bad", never which key it wanted, so the
way to identify the mismatch is to compare the RSA modulus in the signature
block of the running image against the key you signed with.
[ota.md](../../../../docs/ota.md) has the commands.
