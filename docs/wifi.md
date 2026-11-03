# WiFi (`espos_wifi`)

`espos_wifi` connects the device to the boat's WiFi. It tries up to four
networks in order, waits longer between rounds of failed attempts, uses DHCP
or a fixed address, and runs a setup network (the portal) when it can't
connect. Its settings are the `wifi` namespace; its status is
`GET /api/v1/wifi/status` and the `wifi` SSE event.

WiFi is one of the links that report to [`espos_net`](net.md). To ask
whether the network is up, call `espos_net_is_up()`, not this component.
The device id, the hostname (`net.hostname`, called `wifi.hostname` until
0.7) and mDNS belong to `espos_net`, and `espos_wifi_start()` needs
`espos_net_start()` to have run first.

## State machine

```
            START (sta_enabled, ≥1 network)
                     │
     ┌───────────────▼────────────────┐   STA_CONNECTED   ┌──────────────┐  GOT_IP  ┌───────────┐
     │          CONNECTING            │──────────────────▶│ OBTAINING_IP │─────────▶│ CONNECTED │
     │  connect(net[i]); connect_tmo  │                   │  dhcp_tmo    │          │           │
     └───────────────┬────────────────┘                   └──────┬───────┘          └─────┬─────┘
        DISCONNECTED │ (reason) / timeout                DHCP timeout / drop         drop / LOST_IP
                     ▼                                          │                         │
          next network in the list? ──yes──▶ CONNECTING(i+1)   ◀────────────────────────┘
                     │ no (round complete)                (a drop from CONNECTED retries the
                     ▼                                     same network once before moving on)
     ┌────────────────────────────────┐
     │            BACKOFF             │  1 s · 2^round, capped at backoff_max_s, ±25 % jitter
     │  timer → CONNECTING(net[0])    │  round resets on success
     └────────────────────────────────┘

  DISABLED      sta_enabled=false: idle (portal may be up)
  UNCONFIGURED  no network has an SSID: idle, portal up immediately
                (no portal while another transport has the network)
```

When a connection fails, the status says why, as `reason: {code, text}`:

| Code | Text |
|------|------|
| 201  | network not in range (`NO_AP_FOUND`) |
| 202  | wrong password (`AUTH_FAIL`) |
| 2    | wrong password (auth expired) |
| 15   | wrong password (4-way handshake timeout) |
| 204  | auth timed out, weak signal? (`HANDSHAKE_TIMEOUT`) |
| 200  | lost beacon (out of range or AP off?) |
| 1001 | associated but no IP address (DHCP timeout) |
| 1002 | no answer from the driver (connect timeout) |
| 1003 | IP address lost |
| 1004 | reconnecting after configuration change |
| 1005 | station disabled |

Codes from 1001 up are espOS's own, not WiFi codes. The full table is in
`wifi_reason.c`. An unknown code reads `unknown reason` with the number next
to it.

A station that keeps trying for `wifi.stall_s` (30 minutes) without associating
once restarts the device, once per outage, as the `wifiStalled` condition
([Health](health.md#why-wifistalled-restarts-once)).

## Networks and priority

There are four slots, `ssid0..3`, `psk0..3` and `bssid0..3`, tried in that
order; empty slots are skipped.

* One round tries every configured network once, with no pause in between.
  The backoff wait comes between rounds.
* Changing the settings keeps a working connection as long as its network
  (SSID, password, BSSID pin) is still in the list. Reordering or adding
  networks never drops the link. Changing the current network's password
  reconnects from the top of the list.
* `bssidN` (`aa:bb:cc:dd:ee:ff`) pins one access point and turns off roaming
  for that slot.

With a password set, the device refuses open and WEP networks. The minimum
is WPA-PSK; WPA2 and WPA3-SAE/H2E are used when the access point offers
them, and PMF is supported. Passwords must be 8 to 63 characters. A shorter
one isn't valid WPA, so that slot is skipped with a warning. An empty
password means an open network.

## Access points that keep a restarted station

A device that restarts without saying goodbye (a power cut, a crash, a flash
that resets the chip) leaves its access point holding the old connection.
Most access points replace it when the device joins again. Some don't: a
MikroTik hAP ax S on RouterOS 7.24 logs `reconnecting` instead of
`connected`, the device gets its address and sees broadcasts, but everything
sent to it alone is lost until the access point times the old entry out,
minutes later. The device looks connected and reaches nothing (espOS #289).

`wifi.fresh_assoc = true` works around it: after each start the device drops
its first association once, which tells the access point to forget the old
one, and joins again straight away on the same channel. That costs about two
seconds per boot. The state machine only sees the second association; if that
one fails, it counts as a failed attempt like any other. It is off by default
because most access points don't need it.

A restart the firmware asks for itself, such as after an OTA update or a
settings change, says goodbye on its own: ESP-IDF stops WiFi in its shutdown
handler before `esp_restart()`. The setting is for every restart that can't:
a power cut, a crash, a reset from the flasher, and on an ESP32-P4 a restart
by the co-processor link watchdog, whose link to the radio is already dead.

## Addressing

`wifi.ip_mode` is `dhcp` (the default) or `static`. With `static`,
`wifi.ip` and `wifi.netmask` are required, and `wifi.gateway`, `wifi.dns0`
and `wifi.dns1` are optional. An empty `dns0` means the gateway, and an
empty `dns1` means none.

The addresses are applied before every connection attempt, so a change
takes effect at the next reconnect or reboot; that is why these keys are
marked restart-required. An `ip` or `netmask` that doesn't parse falls back
to DHCP with one warning in the log, so a typo doesn't leave the device
unreachable. A fixed address still raises `IP_EVENT_STA_GOT_IP`, so
everything above sees the same thing it would with DHCP.

## Portal (SoftAP provisioning)

The portal is the setup network. It runs alongside the WiFi connection
(APSTA); the device keeps trying its networks while the portal is up.

| Situation | Portal |
|---|---|
| no network configured | up at once |
| `sta_enabled = false` | up at once |
| retrying without success | up after `portal_after_s` (default 90 s), or when the connection attempt in progress ends, whichever is later |
| connected | down |
| **another link, such as Ethernet, has the network** | **down**, unless `portal_online` |
| `portal_enabled = false` | never |
| the application called `espos_wifi_portal_open(true/false)` | up or down, until `espos_wifi_portal_auto()` |

**While Ethernet (or any other link) carries the network, there is no setup
network by default.** You can reach the device over that link, so a setup
network adds nothing. Without this rule an Ethernet-only board would keep an
open setup network up for as long as it is powered, because WiFi never
reaches `connected`. espOS used to do exactly that (espOS #158).

It works both ways. Ethernet coming up takes a running setup network down,
and Ethernet going away brings it back at once instead of after another
`portal_after_s`. `wifi.portal_online = true` brings back the old behaviour,
for a device whose only way in must be the setup network.

That matters if the API key is lost. The power-cycle recovery in
[Security](security.md#the-setup-portal-and-the-way-back-in) only lifts the
key on the setup network; it doesn't bring one up. So on a device that is
online over Ethernet, unplug the cable first. With no WiFi network
configured the setup network comes back at once. With one configured it
comes back when `portal_after_s` has passed since WiFi dropped, which on a
device that has been running a while is straight away.

`espos_wifi_portal_open(bool)` lets the application decide instead, for a
key switch, a hatch sensor or a commissioning mode. It isn't saved, so a
reboot forgets it. It doesn't override `portal_enabled = false`: if the
owner turned the setup network off, it stays off. Forcing it **up** isn't
instant. The request waits for any connection attempt in progress, so on a
device cycling through attempts the setup network appears when the current
one ends. `GET /api/v1/wifi/status` shows the request as `portal.force`
(`auto`, `up` or `down`), which is the only way to tell an application's
choice from the rules above.

The setup network is called `portal_ssid`, by default `espOS-<id>`, where
the id is the last 4 hex digits of the chip's base MAC address from eFuse
([Net](net.md)). On a P4 that is not the MAC the router shows, which belongs
to the C6 radio chip. It is
open unless `portal_psk` (at least 8 characters) is set. The device is
`192.168.4.1/24` and hands out addresses by DHCP. A small DNS server answers
every name with that address, and the usual phone and laptop probe URLs
(`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, …) redirect to
`/`. So phones open their "sign in to network" page and land on the setup
page, which scans for networks and saves `wifi.ssid0/psk0` through the normal
API. Once the device connects, the setup network goes away and the phone
drops it; that is expected.

Two log lines show how far a phone got:

```
portal up: join "espOS-2be9" (open) and open http://192.168.4.1/
portal: lease 192.168.4.2 to ce:ee:ba:0d:3f:35
```

The lease line means the DHCP server answered. A phone that joins without a
lease line got associated but no address; look into that. With both lines
the network is fine and the problem is higher up. Most often the setup page
never opened, which on a board with a display can simply be a blank screen.

## Setting WiFi without the portal

Settings live in plain NVS, so you can flash them as a factory image. Put
the credentials in a CSV **outside the repository** (names like
`*.nvs.csv` are git-ignored), build the partition and flash it at the `nvs`
offset:

```sh
printf 'key,type,encoding,value\nwifi,namespace,,\nssid0,data,string,MyBoat\npsk0,data,string,secret\n' > wifi.nvs.csv
python -m esp_idf_nvs_partition_gen generate wifi.nvs.csv wifi.nvs.bin 0x20000
esptool.py --port /dev/ttyACM0 write_flash 0x9000 wifi.nvs.bin
```

`config_version` can be left out; the device adds it on first boot. The
size is the `nvs` partition's, 128K (`0x20000`) in the tables espOS ships. A
project with its own partition table uses its own size. A board first
flashed with espOS 0.18 or older keeps the 48K (`0xc000`) it was flashed
with: use that, or the image overwrites the partitions after it.

## Status document

`GET /api/v1/wifi/status`:

```json
{
  "state": "connected", "sta_enabled": true, "hostname": "espos-1a2b",
  "network_index": 0, "ssid": "Boat", "bssid": "aa:bb:cc:dd:ee:ff", "channel": 6, "rssi": -59,
  "ip": "192.168.1.23", "netmask": "255.255.255.0", "gateway": "192.168.1.1", "connected_s": 26,
  "reason": {"code": 0, "text": ""},
  "attempt": 0, "round": 0, "connect_count": 1, "disconnect_count": 0,
  "portal": {"active": false, "ssid": "espOS-1a2b", "force": "auto"}
}
```

Some fields only appear in some states:

* `ssid`, `bssid`, `channel` and `rssi` from `obtaining_ip` on;
* `ip`, `netmask`, `gateway` and `connected_s` only in `connected`;
* `backoff_ms` only in `backoff`;
* `portal.ip` and `portal.clients` only while the setup network is up.

`portal.force` is always there, because it says what was asked for, which
matters even when the answer is no setup network. `hostname` is
`net.hostname` as `espos_net` set it at boot. The same document goes out as
the `wifi` SSE event on every change, and once when an SSE client connects. For
the view that doesn't care which link is up (is there a route, on which
interface, with which address) use `GET /api/v1/net/status` ([Net](net.md)).

## Co-processor link watchdog (esp_hosted boards)

On boards where the radio is a second chip, such as an ESP32-P4 with an
ESP32-C6 over SDIO, the link between the two can jam while WiFi still says
`connected`. The log fills with

```
rpc_core: Timeout waiting for Resp for [0x126](Req_WifiStaGetApInfo)
```

and nothing else: the channel to the radio is gone, so the main chip can't
even ask whether it is connected. Espressif tracks this as
[esp-hosted-mcu#197](https://github.com/espressif/esp-hosted-mcu/issues/197)
and [#220](https://github.com/espressif/esp-hosted-mcu/issues/220).

esp_hosted raises `ESP_HOSTED_EVENT_TRANSPORT_FAILURE` for faults it can
see itself, such as a dropped SDIO read or an all-ones `PKT_LEN`, and with
`CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE=y` (the default, which
espOS keeps) that restarts the device. **A link that jams silently raises no
event at all**, because there is nothing to see, only an absence.

`espos_wifi_hosted_watchdog_start()` adds the missing signal. It turns on
the radio chip's heartbeat (every 20 s) and watches for it. The heartbeat
uses the same channel that jams, so when it stops, the link is gone. After
three missed beats the device restarts.

If the radio chip announces a fresh start (`ESP_HOSTED_EVENT_CP_INIT`) after
the first heartbeat, it reset under a running device, and the device
restarts at once. The same event before the first heartbeat is part of
normal boot: the main chip resets the radio chip while bringing up the link,
and the event can arrive after the watchdog is watching, so the watchdog
ignores it and logs `boot handshake, ignored`. Restarting on it
put a P4 panel into a boot loop, and on the first boot of an update any
restart before the confirm rolls the update back.

The watchdog starts by itself from `espos_wifi_start()`. On chips with their
own radio, and in the simulator, it is a stub that returns
`ESP_ERR_NOT_SUPPORTED`. An error from it means a jammed link won't be
noticed, not that WiFi is broken.

### Why a restart and not a transport re-init

Espressif's `host_hosted_events` example recovers with `esp_hosted_deinit()`,
`esp_hosted_init()` and `esp_hosted_connect_to_slave()`. That repairs the
link in seconds and keeps the device's state. **Don't do this here**,
however much better it looks than a restart.

That pair **crashes instead of returning an error** when it can't allocate
its buffers again:

```text
assert failed: sdio_mempool_create sdio_drv.c:258 (buf_mp_g)
```

Deinit frees the buffer pool and init allocates it again. If that fails,
because memory is fragmented or the old pool wasn't fully released, the
assert fires on whichever task called it, which for this watchdog is
`esp_timer`. Checking the return value can't help, because the assert fires
first.

The link is already broken when recovery runs, so recovery must not be able
to fail in its own way. `esp_restart()` always works. A device that restarts
60 s after its radio link dies is much better than one that sits unreachable
until someone switches it off and on, which is what happened before.

## esp_hosted 2.x → 3.x key names

espOS pins `espressif/esp_hosted` at exactly `3.0.10`. Version 3.0 renamed
most of the main chip's Kconfig options and **removed** the 2.x names.
ESP-IDF leaves `KCONFIG_WARN_UNDEF_ASSIGN` off, so a setting for an option
that no longer exists is dropped silently. A project that still carries 2.x
settings gets the built-in SDIO defaults, the radio chip never answers, and
the only symptom is a P4 with no WiFi. So `espos_core/project_include.cmake`
looks for these names in the defaults files and stops the configure step.

| esp_hosted 2.x | esp_hosted 3.x |
| --- | --- |
| `CONFIG_ESP_HOSTED_ENABLED` | `CONFIG_ESP_HOSTED` (role: `CONFIG_ESP_HOSTED_HOST`) |
| `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE` | `CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO` |
| `CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS` | `CONFIG_ESP_HOSTED_HOST_SDIO_BUS_WIDTH_4` |
| `CONFIG_ESP_HOSTED_SDIO_PIN_{CLK,CMD,D0..D3}` | `CONFIG_ESP_HOSTED_HOST_SDIO_PIN_{CLK,CMD,D0..D3}` |
| `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE` | `CONFIG_ESP_HOSTED_HOST_RESET_GPIO` |
| `CONFIG_ESP_HOSTED_{SDIO,SPI,SPI_HD,UART}_RESET_ACTIVE_{HIGH,LOW}` | *(removed, no replacement; see below)* |
| `CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ` | `CONFIG_ESP_HOSTED_HOST_SDIO_CLK_KHZ` |
| `CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE` | `CONFIG_ESP_HOSTED_HOST_SDIO_RX_STREAMING_MODE` |
| `CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP` | `CONFIG_ESP_HOSTED_HOST_CP_RESET_STRATEGY_ALWAYS` |
| `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` | `CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM` |
| `CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE` | `CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE` |
| `CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID` + `..._BLUEDROID_HCI_VHCI` | `CONFIG_ESP_HOSTED_HOST_FEAT_BT` + `CONFIG_BT_BLUEDROID_ENABLED` ([BLE](ble.md)) |
| *(no equivalent)* | `CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT`, opt-in in 3.x and required by espOS |

The reset polarity can't be set any more. 3.x always sends the pulse that
2.x's `ACTIVE_HIGH` produced: high, 10 ms low, high (`eh_host_port_power.c`:
*"CP reset is its EN pin: LOW asserts reset, HIGH runs it"*). The Waveshare
boards used `ACTIVE_HIGH` with 2.x, so dropping the key changes nothing
electrically on them, and espOS's P4
settings simply lose the line. A board that needed the **opposite** polarity
has no setting for it any more, and its radio chip won't come out of reset.
That is a hardware fact, so the check says so instead of suggesting a
replacement key.

A 3.x main chip still works with radio chip firmware from 0.0.6 on, 1.x and
2.x, so moving to 3.x doesn't by itself mean reflashing the C6. We checked
this on a Waveshare P4 with a radio chip reporting 2.12.3: SDIO came up in
streaming mode, RPC used V2, and WiFi and BLE both worked.

It doesn't mean the radio chip firmware is current, though. Both of our test
boards raise `coprocessorStale` against a 3.0.9 host, and esp_hosted says a
major version mismatch is what causes RPC timeouts. Update it before chasing
one ([Hardware](hardware.md#updating-the-c6-co-processor-firmware)).

Radio chip firmware older than the version handshake reports no version at
all, so the boot log says `Co-proc [0.0.0]` on a perfectly healthy link.
That is also why espOS reads the version from the handshake (the
`ESP_PRIV_FIRMWARE_VERSION` field) instead of calling
`esp_hosted_get_coprocessor_fwversion()`, which those chips don't answer.
It falls back to that call only when esp_hosted's handshake header isn't
reachable from the build, and then old radio chips time out on the query. `hardware.coprocessor` in
[`GET /api/v1/system/info`](rest-api.md) and the `coprocessorStale` health
condition report it. There, `0.0.0` means "present and answering, but silent
about its version", which is not the same as the field being missing.

## Sharing the radio with Zigbee or Thread

On an ESP32-C5 or C6 running Zigbee or Thread on its own radio, Wi-Fi and
802.15.4 take turns on the same 2.4 GHz radio. espOS turns on that sharing
when the station starts, in every build that has the 802.15.4 driver and
`CONFIG_ESP_COEX_SW_COEXIST_ENABLE` (on by default). There is nothing to
call from the application. Without it, a Zigbee coordinator, which always
listens, keeps the radio to itself: the access point drops the station for
missing acknowledgements (reason 34) and every reconnect times out. A build
without 802.15.4 is unchanged; Bluetooth shares the radio without this step.

Such a build also keeps the station in modem sleep: `wifi.ps_mode` `none`
runs as `min`, and the log says so at start (`power save 'none' runs as
'min'`). The arbiter gives 802.15.4 reception the lowest priority, so it
only hears what arrives while Wi-Fi leaves the radio alone (IDF's
coexistence guide). A station that never sleeps left so little of that time
that a Zigbee device trying to join a coordinator was never heard; with
`min` it joined within minutes. `min` and `max` are used as set. Modem
sleep needs a station that is connected and running on its own (IDF's Wi-Fi
power-save guide): while the setup portal is up, the radio runs station and
access point together and the station does not sleep, whatever the setting,
so a device may not get through until the board has joined its network. The
price is latency: the station wakes for the access point's beacons, so a
ping can take a few hundred milliseconds. `min` is also IDF's own default
and what its Zigbee gateway example sets.

## mDNS

The mDNS responder moved to [`espos_net`](net.md#mdns) along with the
network handling. The API is unchanged (`espos_mdns.h`,
`espos_mdns_add_service()`, `ESPOS_EVENT_MDNS_READY`). It follows whichever
interface carries the default route, its options are
`CONFIG_ESPOS_NET_MDNS` and `CONFIG_ESPOS_NET_MDNS_MAX_SERVICES`, and the
`.local` name is `net.hostname`.

## Design notes

* The state machine (`wifi_sm.c`) is plain C over an injected port, so it
  runs unchanged on the host (`test/host/espos_wifi_test` covers every
  transition above). The device port (`port_idf.c`) is glue to esp_wifi and
  esp_netif. The host port (`port_sim.c`) scripts a fake driver, so the REST
  and SSE layer can be tested too.
* **Driver calls are never made while the state machine is locked.** The
  state machine queues actions, and `espos_wifi_dispatch()` runs them after
  unlocking. On the ESP32-P4 the driver is a radio chip behind an RPC that
  itself delivers events into the state machine; calling it under the lock
  deadlocked within a second of boot.
* One FreeRTOS timer handles the connect, DHCP and backoff timeouts and the
  portal deadline, whichever comes first, so there is nothing to keep in
  sync.
* On the ESP32-P4, WiFi is an ESP32-C6 over SDIO (`esp_hosted` and
  `esp_wifi_remote`, P4-only dependencies in `main/idf_component.yml`; the
  pins are in `sdkconfig.d/espos.defaults.esp32p4`). The `esp_wifi_*` API is
  the same, but the MAC address comes from the driver, not from eFuse.

  Four settings on that link are essential, and all are set in
  `sdkconfig.d/espos.defaults.esp32p4`:

  * **`CONFIG_WIFI_RMT_RX_BA_WIN=6`.** ESP-IDF defaults this to 6 but raises
    it to 16 as soon as a project turns on PSRAM
    (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`), which any board with a display does.
    At 16 the SDIO receive path overruns under steady incoming TCP. The link
    jams, `H_SDIO_DRV` repeats *"task still writing Rx data to queue!"*,
    every RPC to the C6 times out, and WiFi stays dead until the main chip
    restarts. Meanwhile the state machine still says `CONNECTED`, because the
    disconnect event can't cross the jammed link
    (espressif/esp-hosted-mcu#184). On a Waveshare 7B with repeated ~30 KB
    HTTP reads, it jammed after 85 requests at 16 and survived 400 at 6. The
    option is `WIFI_RMT_*` because the radio is remote; the local
    `ESP_WIFI_RX_BA_WIN` doesn't reach it.
  * **`CONFIG_ESP_HOSTED_HOST_SDIO_RX_STREAMING_MODE=y` must stay on.** The
    C6 firmware always uses streaming mode and the main chip has to match,
    or the link stops at boot with *"SDIO mode mismatch: slave is in
    streaming mode, but host is in packet mode. Aborting."*
  * **`CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT=y`.** The heartbeat is the main
    chip's only evidence that the link is alive, and the watchdog above is
    built on it. esp_hosted 3.x made it opt-in and off by default, which
    leaves `esp_hosted_configure_heartbeat()` out of the build. espOS doesn't
    carry on quietly without it: `espos_core`'s check stops the configure
    step with the line to add, and for a project that bypasses the check
    `hosted_watchdog.c` builds as a stub with a `#warning`. What it must
    never do is start the 60 s timer for a heartbeat that can't arrive; that
    would restart a healthy device every minute.
  * **`CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM=y`.** The transport's DMA
    buffer pool is its largest. Left in internal RAM (the default) it is the
    biggest user of `MALLOC_CAP_INTERNAL|DMA` memory. With steady incoming
    TCP (~270 KB/s of ACK-paced MJPEG on a Waveshare 7B), internal free
    memory fell to ~12 KB, SDIO transfers stalled, the heartbeat stopped, and
    the watchdog restarted the device about 90 s into every stream. In PSRAM,
    which the P4's GDMA reaches through the cache, the same load left over
    200 KB internal free and the link held. **Limit:** this only works with
    64-byte L2 cache lines. The 1600-byte buffer stride is a multiple of 64
    but not of 128, so with `CONFIG_CACHE_L2_CACHE_LINE_128B` the SDIO driver
    rejects PSRAM buffers with `ESP_ERR_INVALID_ARG`
    ([esp-hosted-mcu#219](https://github.com/espressif/esp-hosted-mcu/issues/219)).
    Keep the pool internal with 128-byte lines. The option only exists with
    PSRAM, which the P4 defaults turn on (`CONFIG_SPIRAM=y`); without PSRAM
    ESP-IDF drops it silently, and the board doesn't boot anyway
    ([Hardware](hardware.md)).

  A jammed link can't be repaired from the main chip: there is no reconnect
  API, and the RPC that would carry one is exactly what times out. An
  application that must survive it unattended should restart on its own sign
  of life, such as real traffic, not on `ESPOS_WIFI_ST_CONNECTED`, which keeps
  saying all is well.

* The state machine posts nothing itself. On every status change,
  `espos_wifi.c` queues a report for `espos_net` and delivers it outside the
  state machine's lock, because `espos_net_report()` takes its own lock and
  runs subscriber callbacks. `espos_net` decides whether the default route
  changed and posts `NETWORK_UP` or `NETWORK_DOWN`. The port's `GOT_IP`
  handler only logs the "connected to … web UI" line.
* Bluetooth setup is a separate optional component,
  [`espos_prov`](provisioning.md). It writes the same `wifi` keys this
  component reads and never touches the radio itself.
* Not done yet: the country code.
