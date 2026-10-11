# Hardware

What espOS runs on, what differs between the chips, and what to expect from
the boards Signal K users already own. Board-specific code — display HALs,
audio codecs, pin maps — stays in the application; espOS only needs a chip
it builds for and a radio.

## Chips

CI builds the reference application for six targets on every change
(`.github/workflows/ci.yml`); every example builds for `esp32c6` and the ones
with peripherals for `esp32p4` too.

| Target | Status | What to know |
|---|---|---|
| `esp32` | in CI | Native radio, Bluedroid BLE. Needs **chip revision 3.0 or later** for the RSA app-signing scheme ([OTA](ota.md)); `sdkconfig.d/espos.defaults.esp32` pins `ESP32_REV_MIN_3`, and every module sold since 2020 (WROOM-32E/UE, WROVER-E) qualifies. The monitor's boot banner prints the revision. |
| `esp32s3` | in CI | Native radio. With `esp32p4` the only target esp-sr supports, so the only other one that can run the voice satellite's wake word. |
| `esp32c3` | in CI | RISC-V, single core, the smallest. Native radio. |
| `esp32c6` | in CI | RISC-V, WiFi 6, native radio. The target the getting-started guide and the examples use. |
| `esp32p4` | in CI, in daily use | **No radio of its own.** WiFi and BLE come from an ESP32-C6 co-processor over SDIO (below). PSRAM; internal RAM is the scarce pool ([health](health.md)). Rev 1.x silicon allowed. |
| `esp32c5` | in CI | RISC-V, native radio, same shape as the C6. [BLE provisioning](provisioning.md) was verified end to end on this target. Its radio is BLE 5.0, which is worth knowing because protocomm still advertises with the 4.2 API: a build with `espos_prov` asks for `BT_BLE_42_FEATURES_SUPPORTED`. **Not a BLE gateway board** — it cannot run the BLE stack alongside WiFi and the SignalK client at once ([below](#the-esp32-c5-cannot-host-the-ble-gateway)). |
| `esp32c61` | later | Same shape as the C6; waiting for hardware on the bench and a CI slot. |
| `esp32h2`, `esp32h4` | not planned as such | No WiFi radio (BLE + 802.15.4 only). The runtime no longer assumes WiFi -- [`espos_net`](net.md) is the seam and a headless esp32h2 build is a CI gate -- so what these still need is a transport (Thread, or Ethernet on a board that has it). |

The toolchain is one ESP-IDF for all of them: 6.0.x, tested on the release in
`.idf-version` ([Development](development.md)).

## The ESP32-C5 cannot host the BLE gateway

A C5 runs WiFi and the SignalK client well, and it provisions over BLE. It cannot
run the [BLE gateway](ble.md) — BLE scanning plus WiFi plus the SignalK client at
the same time — and no configuration changes that. If you are choosing a board in
order to bridge BLE sensors to SignalK, this is not it; use an ESP32-P4, where BLE
comes from a separate co-processor, or a C6/S3/ESP32.

Measured on a Waveshare ESP32-C5 (espOS #127), internal RAM consumed by each stage
of `espos_start()`:

| stage | internal RAM |
|---|---|
| HTTP server | 25.2 KB |
| `espos_net` | 5.4 KB |
| WiFi station | 37.5 KB |
| SignalK client | 21.2 KB |
| OTA | 9.6 KB |
| **BLE (Bluedroid host + controller)** | **58.2 KB** |
| **total** | **157.0 KB of the 171.5 KB free at `app_main`** |

Which end it fails at depends on PSRAM, and both are dead. **Without** it the BLE
controller never starts — `r_ble_controller_init failed 257`, which is
`ESP_ERR_NO_MEM`, because it needs ~24 KB in one contiguous block and the largest is
~15 KB; WiFi and Signal K keep running, so the device is useful as a WiFi node with
no BLE. **With** it the controller starts and the budget above applies instead.

That leaves **14.5 KB** of internal 8-bit RAM, of which **7.3 KB** is also
DMA-capable. Both numbers matter, for different consumers:

* **14.5 KB** (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`) is what TLS draws from —
  mbedTLS's allocator asks for exactly those caps, and it is the pool espOS's own
  handshake pre-flight measures (`CONFIG_ESPOS_SK_TLS_MIN_FREE_BLOCK_KB`, 24 KB).
  With 14.5 KB free and a largest block well under that, a handshake is refused
  before it is attempted.
* **7.3 KB** (`MALLOC_CAP_DMA`) is the subset left for what genuinely needs DMA —
  the radio and the network driver.

Either way the gateway's HTTP posts start failing and the
[health watchdog](health.md) restarts the board; it alarms below 12 KB, with good
reason. The failure is not subtle: a device that scans happily for about 30 seconds
and then reboots, repeatedly.

### Why the usual escapes do not apply

* **PSRAM does not fix it, in this configuration.** These modules carry 8 MB, which
  a default build leaves switched off (`CONFIG_SPIRAM`), and enabling it *is* what
  lets the controller start at all — `esp_bt_controller_init()` needs ~24 KB in one
  contiguous block. What remains after that is task stacks and radio/WiFi DMA
  buffers, and as the gateway is built they come from internal RAM: the radio's
  buffers because the WiFi and BLE drivers require internal memory for them on this
  part, and the stacks because `xTaskCreate()` allocates them internally. IDF 6 can
  place a stack in PSRAM
  (`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`), but only for a stack passed to
  `xTaskCreateStatic()` and only where it is never touched while the cache is
  disabled — which is not how espOS, Bluedroid or the WiFi driver create theirs, so
  it is a porting exercise rather than a setting. (`CONFIG_SPIRAM` itself is not a
  bootloader change on this part — `esp_psram_init()` runs from `cpu_start.c` during
  application startup, after the app is loaded — so enabling it ships by OTA like
  any other build.)
* **`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` makes it worse, not better.** It is
  the one option that would move the largest single block (38 KB of WiFi and lwIP
  buffers) out of internal RAM, and with it the station never associates at all:
  `connecting to '<ssid>'`, then reason 36 every ~21 s, indefinitely. The
  association failure is measured; the reason is not — the likeliest explanation is
  that the driver needs those buffers in internal memory, but nothing here proves
  that, so treat the option as known-broken on this part rather than as evidence
  about PSRAM in general.
* **Shrinking what is left buys single-digit KB**, against a 40 KB shortfall, and
  each candidate is a real capability: the WiFi buffers are already capped, TLS
  needs 24 KB contiguous for a handshake, and the GATT client is the gateway's
  entire purpose.

### What the C5 is good for

WiFi and SignalK: sensors, relays, switch panels, anything in
[`espos_sensors`](sensors.md) or [`espos_devices`](devices.md). BLE provisioning
also works, because that runs *before* the station comes up rather than beside it —
which is exactly why "BLE provisioning was verified on this target" does not imply
the gateway will fit.

## Flash size and partition tables

The bundled tables have the same layout and differ in the app slots and the
storage partition; the prologue's `PARTITIONS` argument picks one and sets
the flash size with it (`espos_project_prologue(PARTITIONS "${ESPOS_PARTITIONS_DIR}/8mb.csv")`).

| Table | Flash | App slots (`ota_0`, `ota_1`) | `storage` (LittleFS: web UI + your files) | Fits |
|---|---|---|---|---|
| `components/espos_core/partitions/4mb.csv` (default) | 4 MB | 2 × 1728 KB | 384 KB | the core, Signal K, OTA — the getting-started device |
| `components/espos_core/partitions/8mb.csv` | 8 MB | 2 × 3 MB | 1792 KB | a native-radio target with Bluedroid: the BLE gateway measures 2.13 MB on an ESP32-C6, which does **not** fit a 1728 KB slot |
| `components/espos_core/partitions/16mb.csv` | 16 MB | 2 × 6656 KB | 1792 KB | LVGL + esp-sr + hosted WiFi/BLE on the P4 panels; 1 MB left for the esp-sr model partition (commented out in the CSV) |

All three carry `nvs` (48 KB — configuration, WiFi credentials, the Signal K
token), `otadata`, `phy_init`, `nvs_keys` (for NVS encryption in a release
build, [Security](security.md)) and a 64 KB `coredump` partition. A project
with its own table sets `CONFIG_ESPTOOLPY_FLASHSIZE_*` in its own defaults.
`esptool.py flash_id` reports the flash size of a board you are unsure about.
CI's size report fails the build when the app grows past 90 % of its slot
(`tools/espos_size_diff.py`).

## ESP32-P4 and the C6 co-processor

The ESP32-P4 has no radio. On the boards espOS is developed on — the
**Waveshare ESP32-P4-WIFI6-Touch-LCD-7B** (7", 1024 × 600) and **-4B** (4",
720 × 720) panels — an ESP32-C6 module provides WiFi 6 and BLE over SDIO
through Espressif's `esp_hosted` transport and the `esp_wifi_remote` shim, so
the application still calls the ordinary `esp_wifi_*` API. Both panels carry
P4 rev 1.x silicon, 16 MB flash, 32 MB PSRAM and a GT911 touch controller;
they differ in the display panel, which is the application's HAL, not
espOS's.

Everything espOS knows about that transport is in
`sdkconfig.d/espos.defaults.esp32p4`, which every P4 build inherits:

* **SDIO pinout** for the Waveshare family: slot 1, 4-bit bus, `CLK` 18,
  `CMD` 19, `D0..D3` 14–17, C6 reset on GPIO 54, 40 MHz.
  Another P4 board changes these lines in its own `sdkconfig.defaults`.
* **Three load-bearing settings**, explained in [WiFi → Design
  notes](wifi.md#design-notes): the receive block-ack window
  `CONFIG_WIFI_RMT_RX_BA_WIN=6` (IDF raises it to 16 when PSRAM is on and the
  SDIO Rx path then wedges under sustained inbound TCP), the transport
  mempool in PSRAM with **64-byte L2 cache lines** (the 1600-byte stride is
  not 128-aligned; the two options are pinned together), and SDIO streaming
  mode (the stock C6 firmware is fixed in it).
* `CONFIG_ESP32P4_REV_MIN_100` for the rev 1.x silicon, an 8 KB main task
  stack, `FREERTOS_HZ=1000`.
* **PSRAM on** (`CONFIG_SPIRAM=y`). `esp_hosted`'s startup allocations leave
  internal RAM so short that without PSRAM FreeRTOS cannot allocate its
  timer task's stack and the board panics within seconds of every boot
  (`assert failed: vApplicationGetTimerTaskMemory`). The hosted mempool
  setting above also needs it, and IDF drops that one silently without.

### Updating the C6 co-processor firmware

For what espOS uses, the co-processor needs no reflashing — but that is about
its *capabilities*, not its version, and the two fail differently.

`esp_hosted` compares its own version against the slave's at every boot and
says so once, into the log ring, which rotates:

```
I transport: esp-hosted fw versions: host=3.0.9 coprocessor=0.0.0
E transport: major version mismatch — OTA coprocessor from host
```

`0.0.0` means the slave's init event carried no `ESP_PRIV_FIRMWARE_VERSION`
TLV — an image old enough not to announce itself. The same facts are also
readable at any time as `hardware.coprocessor` in
`GET /api/v1/system/info`, and raised as the `coprocessorStale` health
condition, because the lines above are the only other evidence and a device
that has been up for days has lost them to the log ring.

espOS reads them from the TLVs `esp_hosted` already parsed, not from
`esp_hosted_get_coprocessor_fwversion()`. The public API is an RPC to the
co-processor, and a co-processor worth warning about is the one whose RPC
times out — so the query would fail exactly when the answer matters. The
`stale` flag is `esp_hosted`'s own comparison, not a second opinion.

Whether it matters in practice is not something espOS can tell you: both P4
panels here have run for months on a stock slave, and the one device seen with
`0.0.0` also spent 45 hours failing to associate before a reboot cleared it
([espOS #136](https://github.com/signalk-espOS/espOS/issues/136)) — suggestive,
not demonstrated. Treat the condition as a precondition worth removing before
chasing RPC timeouts, not as a diagnosis.

#### Over the link

A P4 board updates its own co-processor: `POST /api/v1/system/coprocessor/update`
([REST API](rest-api.md#get-systemcoprocessorupdate-post-systemcoprocessorupdate-protected))
downloads the C6 image into PSRAM, checks it and streams it to the C6 through
`esp_hosted`'s OTA calls (begin, write, end, activate). The C6 restarts itself
into the new image a moment after `activate()`, and the P4 follows as soon as it
notices (esp_hosted restarts it on the broken link, the hosted watchdog when the
C6 announces itself, or a fallback timer); its start resets the C6 again, so
both chips start together on the new pair. The heartbeat watchdog is held for
the transfer: `begin()` erases a 1.75 MB slot before it answers, which is not
the wedge it exists for. The update is refused while the P4's own firmware is
still pending verification, because that restart would roll it back, and while a
P4 firmware update runs (and the other way round), because each ends in a
restart. The download parks the same real-time work an OTA download does
(`espos_ota_quiesce_hook()`). A client such as the Signal K manager can offer it
whenever `coprocessor.stale` is set.

Why it is safe to offer to someone who cannot reach the C6 with a cable:

- **One image only.** The C6 image carries no signature the P4 can check, so
  the trust is in the P4 firmware, which is signed: it accepts exactly the image
  whose SHA-256 is `ESPOS_COPROC_FW_SHA256` in
  `components/espos_wifi/src/coproc_image.h`, from whatever URL it was fetched.
  A source header rather than Kconfig, because an existing `sdkconfig` keeps an
  old Kconfig default and would go on offering the previous image. A mirror
  serving the wrong file fails the hash, not the radio.
- **Checked before written.** The whole image (about 1.4 MB) is hashed in PSRAM
  before the first byte goes over SDIO. A co-processor older than esp_hosted
  2.6 switches its boot slot at `end()` with no separate `activate()`, so
  hashing while streaming would be too late.
- **Two slots.** The C6 writes into its inactive app slot. An interrupted
  transfer, a refused image or a power cut leaves the running image in place,
  and the next attempt erases the slot again.
- **Confirmed once it boots.** An update does not replace the C6's bootloader.
  If the one it shipped with has app rollback on, an image still unconfirmed at
  its second boot is rolled back. The image cannot wait for a host to talk to it
  first: it usually first boots when the C6 restarts itself, which breaks the
  link, and the P4's restart then resets the C6 again, so the first boot a host
  negotiates with is the second. It confirms itself once it has booted and
  opened NVS instead. One that crashes before that goes back to the image that
  worked, where the bootloader has rollback; one that boots but cannot talk to
  the P4 needs the cable route below.

Rolling the P4 back after updating the C6 is the one way to lose the radio
this way: the 3.x image speaks only to an esp_hosted 3.x host, and the P4's
other OTA slot may still hold an older firmware with a 2.x host (espOS before
0.14.0). Update the P4 forward, not back, once its C6 runs 3.x; the cable route
below is the way out if it happens.

The image is `tools/coprocessor`: `esp_hosted`'s Wi-Fi + BLE co-existence
co-processor (BLE as HCI over SDIO, which is what the [BLE gateway](ble.md)
uses), built with `CONFIG_APP_REPRODUCIBLE_BUILD`. The `coprocessor` workflow
rebuilds it on every change, fails if the result differs from the pinned hash,
and on `main` publishes it to the `coprocessor-assets` branch under a name
carrying its version and the first 12 hex digits of its hash, so a published
file is never replaced. Any change to the image's bytes (an IDF bump, an
sdkconfig change) is a new hash and a new file name in `ESPOS_COPROC_FW_SHA256`
and `ESPOS_COPROC_FW_URL`; the workflow's error says what to set. Take the hash
from the workflow, not from a local build: a different toolchain image or
component checkout can shift the bytes, and the workflow's build is the one
that gets published. Bumping `esp_hosted` also moves four versions the workflow
checks agree: `components/espos_wifi/idf_component.yml`,
`tools/coprocessor/main/idf_component.yml`, `PROJECT_VER` in
`tools/coprocessor/CMakeLists.txt` and the version in `coproc_image.h`, which
the build also checks against the host's esp_hosted.

Two things a bench has to show before this is offered widely, because no C6
has yet been updated this way here: that a stock Waveshare C6 (esp_hosted
2.x) accepts the 3.x image over the link, and that its app slots are large
enough for it (the image is 1.4 MB; esp_hosted's own 4 MB table gives 1.75 MB).
A C6 whose partition table has no second slot refuses at `begin()` and keeps
running.

#### Over USB

The same image can be flashed by hand from `tools/coprocessor` with
`idf.py -p <port> flash`, with the C6 on its own USB port, where the board
exposes one. On the PoE board, remember that USB and Ethernet must not be
connected at the same time.

### Capabilities

A stock C6 slave reports
`capabilities: 0xd` — WLAN and BT over SDIO, BLE only — which is what the BLE
gateway uses (HCI over SDIO, Bluedroid host on the P4). Two limits of that
path, from [BLE gateway → Targets](ble.md#targets): **BLE 4.2 only** (the
slave does not forward BLE 5.0 extended HCI commands correctly) and **passive
scan** by default (active scan over SDIO is unreliable). The C6-MINI-1U
module on these panels has **no PCB antenna**: with nothing on its IPEX
connector the radio hears nothing at all.

When the SDIO link wedges, the WiFi state machine still reports connected
because the disconnect never crosses the jammed link. espOS's answer is the
co-processor heartbeat watchdog (`espos_wifi_hosted_watchdog_start()`, armed
by `espos_wifi_start()`: three missed 20 s beats restart the device) and the
`skLinkStalled` health condition; an in-place `esp_hosted_deinit()`/`init()`
recovery is **not** attempted, because it asserts instead of failing
([WiFi → Co-processor link watchdog](wifi.md#co-processor-link-watchdog-esp_hosted-boards)).

Flashing is `idf.py -p /dev/ttyACM0 flash` over the USB port as on any
target; the P4's USB-Serial/JTAG console on that same port is also the only
place a panic backtrace appears.

## Boards SensESP users own: SH-ESP32 and HALMET

Hat Labs' **SH-ESP32** and **HALMET** are the boards most SensESP projects
run on. From espOS's point of view they are plain ESP32 (original) boards
with a CAN transceiver and a wide-range power supply — the `esp32` target,
nothing special to configure:

* **Target and revision.** `idf.py set-target esp32`. The module on these
  boards is a current WROOM/WROVER, so the revision-3 requirement for signed
  images is met; the boot banner (`chip revision: v3.x`) confirms it.
* **Flash size** decides the partition table: `esptool.py flash_id` on the
  board tells you whether `4mb.csv` (the default) or `8mb.csv` applies. A
  WROVER module adds PSRAM; a WROOM has none, and none of the core needs it.
* **NMEA 2000.** The transceiver is on the board, so `espos_n2k` is a
  receiver, a transmitter and the pins ([NMEA 2000 gateway](n2k.md)); the
  bitrate is 250 kbit/s. On the SH-ESP32 the CAN transceiver is wired to
  **GPIO 34 (RX)** and **GPIO 32 (TX)** — the pins SensESP's SH-ESP32 examples
  use — so:

  ```cpp
  static espos_n2k::TwaiReceiver rx({.tx_pin = GPIO_NUM_32, .rx_pin = GPIO_NUM_34, .bitrate = 250000});
  ```

  HALMET routes its transceiver differently; take the pins from the HALMET
  firmware's board constants rather than from this page.
* **Everything else** SensESP did on these boards — 1-Wire temperature,
  I²C sensors, the HALMET's analog and digital inputs — is application code
  on top of `espos_sk_publish_*`; the [examples](examples.md) `analog_input`
  and `pulse_counter` are the starting points, and [Migrating from
  SensESP](migration-from-sensesp.md) has the mapping.
* **BLE.** The BLE gateway on an original ESP32 links Bluedroid and needs the
  8 MB table (above).

## Before you buy

Any development board with one of the six chips works for the core:
a bare ESP32-C6 module on a breakout is the cheapest way to try espOS, and a
Waveshare P4 panel is the reference for anything with a display or voice.
For a co-processor board other than Waveshare's, budget a session for the
SDIO pinout and the three settings above; for a board with an external
antenna connector, budget an antenna.
