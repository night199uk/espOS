# WiFi (`espos_wifi`)

Station connection manager with an explicit status model, a priority list of
networks, exponential backoff, DHCP or static addressing and a SoftAP
provisioning portal. Configuration is the `wifi` namespace (see the schema);
status is `GET /api/v1/wifi/status` and the `wifi` SSE event.

The station is one transport of [`espos_net`](net.md): it reports its link
there, and "is the network up" is `espos_net_is_up()`, not this component's
state. The device id, the hostname (`net.hostname`, until 0.7
`wifi.hostname`) and the mDNS responder are `espos_net`'s. `espos_wifi_start()`
requires `espos_net_start()` to have run.

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

Every driver event carries the raw reason code; the status exposes it as
`reason: {code, text}` with a human explanation:

| code | text                                   |
|------|----------------------------------------|
| 201  | network not in range (`NO_AP_FOUND`)   |
| 202  | wrong password (`AUTH_FAIL`)           |
| 2    | wrong password (auth expired)          |
| 15   | wrong password (4-way handshake timeout) |
| 204  | auth timed out, weak signal? (`HANDSHAKE_TIMEOUT`) |
| 200  | lost beacon (out of range or AP off?)  |
| 1001 | associated but no IP address (DHCP timeout) — **ours**, not a WiFi code |
| 1002 | no answer from the driver (connect timeout) — ours |
| 1003 | IP address lost — ours                 |
| 1004 | reconnecting after configuration change — ours |
| 1005 | station disabled — ours                |

The full table is in `wifi_reason.c`. Unknown codes read `unknown reason`
with the number alongside.

## Networks and priority

Four slots `ssid0..3` / `psk0..3` / `bssid0..3`. Slot order is priority; empty
slots are skipped. A round tries every configured network once (no backoff
in between); backoff applies between rounds. A live connection is **kept**
when the config changes as long as its network (SSID, password, BSSID pin)
still appears in the new list — reordering or adding networks never drops
the link; changing the password of the current network reconnects from the
top of the list. `bssidN` pins one access point (`aa:bb:cc:dd:ee:ff`) and
disables roaming for that slot.

Security: with a password set the station refuses open/WEP networks
(threshold WPA-PSK; WPA2 and WPA3-SAE/H2E are negotiated when the AP offers
them, PMF capable). Passwords must be 8..63 characters — shorter ones are
not valid WPA and the slot is skipped with a warning. Empty password = open
network.

## Addressing

`wifi.ip_mode` is `dhcp` (default) or `static`; with `static`, `wifi.ip`
and `wifi.netmask` are required, `wifi.gateway`, `wifi.dns0` and `wifi.dns1`
optional (empty `dns0` = the gateway, empty `dns1` = none). The port applies
them to the station netif before every `esp_wifi_connect()`
(`esp_netif_dhcpc_stop` + `esp_netif_set_ip_info` + `esp_netif_set_dns_info`,
or `esp_netif_dhcpc_start` back to DHCP), so a change takes effect at the
next (re)connection or reboot — the keys are flagged restart-required for
that reason. An `ip` or `netmask` that does not parse falls back to DHCP
with one warning in the log rather than leave the device unreachable. A
static address still raises `IP_EVENT_STA_GOT_IP` on association, so the
state machine, `espos_net` and everything above them see exactly what they
see with DHCP.

## Portal (SoftAP provisioning)

The station keeps retrying while the portal is up — the portal never
replaces the station, it runs alongside (APSTA).

| Situation                                        | Portal                                    |
|--------------------------------------------------|-------------------------------------------|
| no network configured                            | up immediately                            |
| `sta_enabled = false`                            | up immediately                            |
| retrying without success                         | up after `portal_after_s` (default 90 s)  |
| connected                                        | down                                      |
| **another transport has the network**            | **down**, unless `portal_online`          |
| `portal_enabled = false`                         | never                                     |
| `espos_wifi_portal_open(true/false)` was called  | up / down, until `espos_wifi_portal_auto()` |

**By default, while Ethernet (or any other transport) is carrying the network,
there is no access point.** The device is reachable over that transport, so an
access point adds nothing — and the rows above would otherwise leave one up for as long as
the device is powered on an Ethernet-only board, because this machine never
reaches `connected` and nothing else would take it down. That is what espOS
used to do, and with `portal_psk` empty by default it was an open network
advertised indefinitely (espOS #158).

It applies in both directions: Ethernet coming up takes a running portal
*down*, and Ethernet going away puts it back at once rather than after another
`portal_after_s`. `wifi.portal_online = true` restores the old behaviour, for a
device whose only way in has to be the access point. It matters if the API key
is then lost: the power-cycle recovery in
[security.md](security.md#the-setup-portal-and-the-way-back-in) exempts
requests arriving on the access point but does not raise one, so on a device
that is online over Ethernet the cable has to come out first. That brings the
portal back at once when no WiFi network is configured; with one configured it
returns when `portal_after_s` has passed since the station link dropped, which
on a device that has been up a while is immediately.

`espos_wifi_portal_open(bool)` lets an application decide for itself — a key
switch, a hatch sensor, a commissioning mode. It is not persisted, so a reboot
forgets it, and it does not override `portal_enabled = false`: an operator who
turned the access point off keeps it off. Holding it **up** is not instant: the
request waits for any association already in flight, for the reason the
deadline does (above), so on a device cycling through attempts the access point
appears when the current one ends. `GET /api/v1/wifi/status` reports the
standing request as `portal.force` (`auto`, `up` or `down`), which is the only
way to tell an application's decision from the policy's.

SSID `portal_ssid` (default `espOS-<id>`, the device id being the last 4 hex
of the base MAC, [net.md](net.md)), open unless
`portal_psk` (≥ 8 chars) is set. The AP is `192.168.4.1/24` with DHCP; a
tiny DNS responder answers every name with that address and the usual OS
probe URLs (`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`,
…) redirect to `/`, so phones pop their "sign in to network" sheet and land
on the setup page. The setup page scans, lets you pick a network and PUTs
`wifi.ssid0/psk0` through the normal API. Once connected the portal drops
(the phone loses the AP — expected).

Two log lines say how far a client got, and they need opposite fixes:

```
portal up: join "espOS-2be9" (open) and open http://192.168.4.1/
portal: lease 192.168.4.2 to ce:ee:ba:0d:3f:35
```

The lease line is the DHCP server answering. A client that associates
without one means the association worked and the address did not; no lease
line *and* a client that believes it is connected is the case to
investigate. Both together mean the network is fine and whatever is wrong is
above it — most often the setup page never opened, which on a display board
can simply be a blank screen.

## Provisioning without the portal

The store is plain NVS, so a factory image works: put the credentials in a
CSV **outside the repo** (patterns like `*.nvs.csv` are git-ignored),
generate the partition and flash it to the `nvs` offset:

```sh
printf 'key,type,encoding,value\nwifi,namespace,,\nssid0,data,string,MyBoat\npsk0,data,string,secret\n' > wifi.nvs.csv
python -m esp_idf_nvs_partition_gen generate wifi.nvs.csv wifi.nvs.bin 0x20000
esptool.py --port /dev/ttyACM0 write_flash 0x9000 wifi.nvs.bin
```

`config_version` need not be present — the store stamps it on first boot.
The size argument is the `nvs` partition's size, 128K (`0x20000`) in the
shipped tables; a project with its own partition table passes its own. A
board first flashed with espOS 0.18 or older keeps the 48K (`0xc000`) it was
flashed with: pass that there, or the image runs over the partitions after it.

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

`ssid/bssid/channel/rssi` appear from `obtaining_ip` on, `ip/netmask/gateway/
connected_s` only in `connected`, `backoff_ms` only in `backoff`, `portal.ip`
and `portal.clients` only while the portal is active. `portal.force` is always
present — it says what was *asked* for, which is just as meaningful when the
answer is no access point. `hostname` is
`net.hostname` as `espos_net` applied it at boot. The same document is
pushed as the `wifi` SSE event on every change (and once on connect). The
transport-neutral view — is there a route, on which interface, with which
address — is `GET /api/v1/net/status` ([net.md](net.md)).

## Co-processor link watchdog (esp_hosted boards)

On boards where the radio lives on a second chip — ESP32-P4 host with an
ESP32-C6 over SDIO, and similar — the host↔co-processor transport can
wedge while the WiFi state machine still reports `connected`. The host
log fills with

```
rpc_core: Timeout waiting for Resp for [0x126](Req_WifiStaGetApInfo)
```

and nothing else: the RPC channel is gone, so the host cannot even ask
whether it is associated. Upstream tracks this as
[esp-hosted-mcu#197](https://github.com/espressif/esp-hosted-mcu/issues/197)
and [#220](https://github.com/espressif/esp-hosted-mcu/issues/220).

esp_hosted raises `ESP_HOSTED_EVENT_TRANSPORT_FAILURE` for faults it can
detect itself (a dropped SDIO read, an all-ones `PKT_LEN`), and with
`CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE=y` — the default, which
espOS keeps — that reboots the device. **A silently wedged link raises no
event at all**, because there is nothing to detect, only an absence.

`espos_wifi_hosted_watchdog_start()` supplies the missing signal. It
enables the co-processor heartbeat (20 s) and watches for it. The
heartbeat travels over the same RPC channel that dies, so its absence
*is* the fault detector. After three missed beats the device restarts.

A co-processor init event (`ESP_HOSTED_EVENT_CP_INIT`) after the first
beat means the co-processor reset under a running host, and the device
restarts at once. One before the first beat is the boot handshake: the
host resets the co-processor during transport bring-up and that event can
reach the loop after the watchdog registers, so it is ignored. Restarting
on it boot-looped a P4 panel, and on the first boot of an OTA image any
restart before the confirm rolls the update back.

It starts automatically from `espos_wifi_start()`; there is nothing to
call. On native-radio and simulator builds it compiles to a stub
returning `ESP_ERR_NOT_SUPPORTED`. A non-OK return means wedges will not
be *detected* — not that WiFi is broken.

### Why a restart and not a transport re-init

Upstream's `host_hosted_events` example recovers with
`esp_hosted_deinit()` → `esp_hosted_init()` →
`esp_hosted_connect_to_slave()`, which repairs the link in seconds and
keeps config and UI state. **Do not do this here**, however obviously
better it looks than a reboot.

That pair **asserts rather than returning an error** when it cannot
re-allocate:

```text
assert failed: sdio_mempool_create sdio_drv.c:258 (buf_mp_g)
```

`sdio_mempool_destroy()` runs on deinit and `sdio_mempool_create()` on
init; if the pool cannot be re-allocated — fragmentation, or the old one
not fully released — the assert fires on whatever task called it, which
for this watchdog is `esp_timer`. Error handling on the return value
cannot help: the assert fires first, so the `if (err)` branch is
unreachable.

The transport is already broken when recovery runs, so the recovery path
must not have a failure mode of its own. `esp_restart()` always works.
A device that restarts 60 s after its radio link dies is strictly better
than one sitting unreachable until someone power-cycles it, which is the
behaviour this replaces — and that, not the in-place repair, was always
the valuable half.

## esp_hosted 2.x → 3.x key names

espOS pins `espressif/esp_hosted` at exactly `3.0.9`. 3.0 renamed most of the
host-side Kconfig surface and **removed** the 2.x spellings. IDF leaves
`KCONFIG_WARN_UNDEF_ASSIGN` off, so an assignment to a symbol that no
longer exists is dropped without a word: a project carrying its own 2.x
fragment gets the component's built-in SDIO defaults, the co-processor
never answers, and the only symptom is a P4 with no WiFi.
`espos_core/project_include.cmake` scans the defaults files for these keys
and fails the configure rather than letting that reach a device.

| esp_hosted 2.x | esp_hosted 3.x |
| --- | --- |
| `CONFIG_ESP_HOSTED_ENABLED` | `CONFIG_ESP_HOSTED` (role: `CONFIG_ESP_HOSTED_HOST`) |
| `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE` | `CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO` |
| `CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS` | `CONFIG_ESP_HOSTED_HOST_SDIO_BUS_WIDTH_4` |
| `CONFIG_ESP_HOSTED_SDIO_PIN_{CLK,CMD,D0..D3}` | `CONFIG_ESP_HOSTED_HOST_SDIO_PIN_{CLK,CMD,D0..D3}` |
| `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE` | `CONFIG_ESP_HOSTED_HOST_RESET_GPIO` |
| `CONFIG_ESP_HOSTED_{SDIO,SPI,SPI_HD,UART}_RESET_ACTIVE_{HIGH,LOW}` | *(removed, no replacement — see below)* |
| `CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ` | `CONFIG_ESP_HOSTED_HOST_SDIO_CLK_KHZ` |
| `CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE` | `CONFIG_ESP_HOSTED_HOST_SDIO_RX_STREAMING_MODE` |
| `CONFIG_ESP_HOSTED_SLAVE_RESET_ON_EVERY_HOST_BOOTUP` | `CONFIG_ESP_HOSTED_HOST_CP_RESET_STRATEGY_ALWAYS` |
| `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` | `CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM` |
| `CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE` | `CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE` |
| `CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID` + `..._BLUEDROID_HCI_VHCI` | `CONFIG_ESP_HOSTED_HOST_FEAT_BT` + `CONFIG_BT_BLUEDROID_ENABLED` ([ble.md](ble.md)) |
| *(no equivalent)* | `CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT` — opt-in in 3.x, required here |

Reset polarity is no longer configurable: 3.x hardcodes the pulse the
`ACTIVE_HIGH` setting produced — park high, 10 ms low, park high
(`eh_host_port_power.c`: *"CP reset is its EN pin: LOW asserts reset,
HIGH runs it"*). 2.x with `ACTIVE_HIGH=y` emitted the same three levels
on the same pin, so dropping the key changes nothing electrically on the
Waveshare boards — which is why espOS's P4 fragment simply loses the line.
A board that needed the **opposite** polarity has no knob any more, and
its co-processor will not come out of reset; that is a hardware fact, not
a configuration one, so the lint says so rather than suggesting a
replacement key.

A 3.x host is back-compatible with 0.0.6+/1.x/2.x co-processor firmware, so
moving to 3.x does not by itself require reflashing the C6 — verified on a
Waveshare P4 against a co-processor reporting 2.12.3: SDIO came up in
streaming mode, RPC defaulted to V2, and WiFi and BLE both worked. It does not
mean the co-processor is *current*: both devices here raise `coprocessorStale`
against a 3.0.9 host, and `esp_hosted` says a major mismatch is what causes RPC
timeouts. Treat the warning as a precondition worth removing before chasing
one, per [hardware.md](hardware.md#updating-the-c6-co-processor-firmware). One consequence to know about: a slave
that predates the `ESP_PRIV_FIRMWARE_VERSION` handshake TLV reports no version
at all, which is why the boot log says `Co-proc [0.0.0]` on a board whose link
is perfectly healthy — and why espOS reads the version out of the handshake
TLVs rather than through `esp_hosted_get_coprocessor_fwversion()`, which is an
RPC the same slave does not answer. `hardware.coprocessor` in
[`GET /api/v1/system/info`](rest-api.md) and the `coprocessorStale` health
condition report it; `0.0.0` there means "present, answering, silent about its
version", which is a different fact from the field being absent.

## mDNS

The responder moved to [`espos_net`](net.md#mdns) with the network seam,
API unchanged (`espos_mdns.h`, `espos_mdns_add_service()`,
`ESPOS_EVENT_MDNS_READY`): it follows whatever interface carries the default
route, and the knobs are `CONFIG_ESPOS_NET_MDNS` and
`CONFIG_ESPOS_NET_MDNS_MAX_SERVICES`. The `.local` name is `net.hostname`.

## Design notes

* The state machine (`wifi_sm.c`) is pure C over an injected port and runs
  unchanged on the host (`test/host/espos_wifi_test`, every transition
  above). The device port (`port_idf.c`) is esp_wifi/esp_netif glue; the
  host port (`port_sim.c`) scripts a fake driver so the REST/SSE layer is
  testable too.
* Driver calls are **never made under the state-machine lock**. The SM
  queues actions; `espos_wifi_dispatch()` runs them after unlocking. On
  ESP32-P4 the driver is a co-processor behind an RPC that itself delivers
  events into the SM — calling it under the lock deadlocked within a
  second of boot.
* One FreeRTOS timer serves connect/DHCP/backoff timeouts and the portal
  deadline (whichever is earlier), so there is nothing to keep in sync.
* ESP32-P4: WiFi is an ESP32-C6 co-processor over SDIO (`esp_hosted` +
  `esp_wifi_remote`, P4-only dependencies in `main/idf_component.yml`;
  pinout in `sdkconfig.d/espos.defaults.esp32p4`). Same `esp_wifi_*` API; the MAC is
  read from the driver, not eFuse.

  Four settings on that transport are load-bearing, all pinned in
  `sdkconfig.d/espos.defaults.esp32p4`:

  * **`CONFIG_WIFI_RMT_RX_BA_WIN=6`.** IDF defaults this to 6 but raises it
    to 16 as soon as a project enables PSRAM
    (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) — which any board with a display
    will. At 16 the SDIO Rx path overruns under sustained inbound TCP: the
    link wedges, `H_SDIO_DRV` spams *"task still writing Rx data to
    queue!"*, every RPC to the C6 times out, and WiFi stays dead until the
    host reboots — while the state machine still reports `CONNECTED`,
    because the disconnect event never crosses the jammed link
    (espressif/esp-hosted-mcu#184). Measured on a Waveshare 7B with
    repeated ~30 KB HTTP reads: wedged after 85 requests at 16, survived
    400 at 6. Note the knob is `WIFI_RMT_*` — with hosted WiFi the radio is
    remote, so the local `ESP_WIFI_RX_BA_WIN` does not reach it.
  * **`CONFIG_ESP_HOSTED_HOST_SDIO_RX_STREAMING_MODE=y` must stay
    on.** The C6 slave firmware is fixed in streaming mode and the host has
    to match, or the transport asserts at boot: *"SDIO mode mismatch: slave
    is in streaming mode, but host is in packet mode. Aborting."*
  * **`CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT=y`.** The co-processor
    heartbeat is the only evidence the host has that the link is alive, so
    the watchdog described above is built on it; esp_hosted 3.x made the
    feature opt-in and defaults it off, which leaves
    `esp_hosted_configure_heartbeat()` out of the build entirely. espOS does
    not quietly carry on without it: `espos_core`'s consumer lint fails the
    configure with the line to add, and `hosted_watchdog.c` compiles to its
    no-co-processor stubs behind a `#warning` for a project that bypasses the
    lint. What it must never do is arm the 60 s timer for a heartbeat that
    cannot arrive — that is a device rebooting every minute on healthy
    hardware.
  * **`CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM=y`.** The transport mempool
    is the transport's large DMA buffer pool; left in internal RAM
    (the default) it is the biggest `MALLOC_CAP_INTERNAL|DMA` consumer on
    the host, and sustained inbound TCP (measured with ~270 KB/s of
    ACK-paced MJPEG on a Waveshare 7B) drove internal free memory to
    ~12 KB, SDIO transfers stalled, the heartbeat stopped, and the link
    watchdog rebooted the host ~90 s into every streaming session. In
    PSRAM (which the P4's GDMA reaches through cache) the same workload
    left >200 KB internal free and the link never blinked. **Constraint:**
    only valid with 64-byte L2 cache lines — the 1600-byte transport
    stride is 64-aligned but not 128-aligned, so under
    `CONFIG_CACHE_L2_CACHE_LINE_128B` the SDIO driver rejects PSRAM
    buffers with `ESP_ERR_INVALID_ARG`
    ([esp-hosted-mcu#219](https://github.com/espressif/esp-hosted-mcu/issues/219)).
    Keep the mempool internal on 128-byte-line configurations. The option
    only exists with PSRAM, which the P4 defaults enable
    (`CONFIG_SPIRAM=y`); without PSRAM IDF drops it silently, and the board
    does not boot anyway ([hardware.md](hardware.md)).

  Since a wedged transport cannot be recovered from the host (there is no
  reconnect API, and the RPC that would carry one is exactly what times
  out), an application that must survive it unattended should reboot on
  its own liveness signal — real traffic, not `ESPOS_WIFI_ST_CONNECTED`,
  which keeps reporting success.

* The state machine posts nothing itself: on every status change
  `espos_wifi.c` queues a report for `espos_net` and delivers it from the
  drainer, outside the SM lock (`espos_net_report()` takes its own lock and
  runs subscriber callbacks). `espos_net` decides whether the default route
  changed and posts `NETWORK_UP`/`NETWORK_DOWN`; the port's `GOT_IP` handler
  only logs the "connected to … web UI" line.
* BLE provisioning is a separate optional component,
  [`espos_prov`](provisioning.md): it writes the same `wifi` keys this
  component reads and never touches the radio itself.
* Not yet: country code.
