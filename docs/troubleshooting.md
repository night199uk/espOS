# Troubleshooting

Find your symptom below. Each entry says why it happens, what fixes it, and
which page explains more.

Three places answer most questions before you get here:

* the **serial monitor** (`idf.py monitor`), where the device says what it
  is doing as it boots ([Getting started](getting-started.md#2-read-the-monitor)
  shows the lines, [Concepts](concepts.md#what-espos_start-does-in-order)
  the start order);
* the **device log** at `GET /api/v1/logs` or on the Logs page of the web
  UI, which keeps the boot log so you don't need a cable ([below](#logs));
* `GET /api/v1/system/info`, where `reset_reason` and `last_reset` say why
  the device last restarted.

## Build

**`IDF_PATH is ESP-IDF vX.Y.Z; espOS builds with v6.0.0 up to (not including) v6.1.0`**
:   Your ESP-IDF is outside the range espOS supports
    (`cmake/espos_version.cmake`). espOS is tested on the release in
    `.idf-version`. Any other 6.0.x builds with a warning. Another minor
    version is refused, because those move APIs that espOS uses (esp_hosted,
    TWAI, the linux target). Install a 6.0.x release and source its
    `export.sh`. To try anyway, add `-DESPOS_ALLOW_IDF_MISMATCH=1`, and say so
    when you report a problem.

**`.idf-version pins vA but the espOS submodule pins vB`**
:   Your firmware has its own `.idf-version` and it disagrees with the one in
    the espOS submodule. Make them match (espOS's is the one its CI tests), or
    delete your project's file to follow espOS.

**`'…/components' has no espos_config — if espOS is a submodule, run git submodule update --init`**
:   The submodule folder is empty. Run `git submodule update --init` in your
    firmware project.

**`git describe here says v0.7.0-66-… which names an older tag`**
:   This is a note, not an error, and the build is fine. `git submodule update`
    never fetches new tags, so the submodule doesn't know about releases made
    since you first cloned it. The version itself comes from the submodule's
    `version.txt`, which is always right. `git -C espos fetch --tags` makes the
    note go away. [Releasing](releasing.md#consumers).

**`the espOS pin is at tag vX but its version.txt says Y`**
:   This one is a real fault, and fetching won't fix it: a tag that the pinned
    commit can reach is one the checkout already has. The pinned commit has a
    `version.txt` that was never bumped to match its tag, so a device would
    report one version or the other depending on the build. Check which commit
    the submodule points at.

**The web page still looks like an older release after an update** (`ui` missing from `/system/info`, or `ui: "storage"`)
:   Older firmware served the web page from the `storage` partition, and an
    over-the-air update never writes that partition. So the page stayed the one
    flashed over USB. Update to a firmware whose `/system/info` says
    `ui: "embedded"`, then reload the page with Ctrl/Cmd-Shift-R. A project
    with its own page bundle (`DIR`) still serves it from storage; flash it over
    USB with `idf.py flash`. [Web UI](ui.md).

**The device shows a placeholder instead of the web UI** (`ui: "placeholder"` in `/system/info`; boot log `ui storage … has no index.html: serving the embedded page`)
:   There is no page bundle in the image or in storage. With
    `CONFIG_ESPOS_HTTPD_UI_EMBED` off, or a custom `DIR`, the page comes from
    the `storage` partition, and that partition is empty. One of three things
    happened:

    * the project never called `espos_project_ui_partition()` after
      `project()`;
    * a custom `DIR` is missing, which CMake only warns about;
    * the image was written with `idf.py app-flash`, which skips
      `storage.bin` (`idf.py flash` writes it).

    A missing default bundle (`espos_httpd/ui-dist`) stops the configure step;
    restore it from git. [Web UI](ui.md).

**`partitions.csv` not found after updating espOS** (or the OTA slots are gone)
:   The partition table moved to `components/espos_core/partitions/4mb.csv`,
    and you now pick it with the prologue's `PARTITIONS` argument. ESP-IDF only
    applies defaults to options your `sdkconfig` doesn't already set, so an old
    `build/sdkconfig` still names the old path. Delete `build/sdkconfig` (or the
    whole build folder) once and configure again. Do the same after editing any
    `sdkconfig.d/*` file. [Development](development.md#building-a-firmware-on-espos).

**`Failed to resolve component 'espos_ble' required by component 'main'`** (or `espos_n2k`, `espos_voice`, `espos_audio`)
:   Firmware projects build with `MINIMAL_BUILD`, which leaves out the optional
    espOS components unless you name them:
    `espos_project_prologue(… COMPONENTS espos_ble)`. The core components need
    no naming, and naming one is an error (`… is not an optional espOS
    component`). [Development](development.md#building-a-firmware-on-espos).

**`no secure_boot_signing_key.pem — generating a development RSA-3072 signing key`**
:   Normal on a fresh checkout, because the key is not in git. It matters once
    devices go on a boat: see [OTA](#ota) below.

**The firmware no longer fits** (size report, `ota_0` over 90 %)
:   The app slot is 1728 KB on the default 4 MB table. A firmware with the
    Bluetooth stack is bigger than that on chips with their own radio (the BLE
    gateway is 2.13 MB on an ESP32-C6), so move to
    `components/espos_core/partitions/8mb.csv`
    ([Hardware](hardware.md#flash-size-and-partition-tables)). Two slots that
    size don't fit on a 4 MB chip at all. On a 4 MB board such a firmware can
    only run without updates, from a single `factory` slot.

## WiFi

**No `espOS-xxxx` network appears**
:   Check these:

    * **Is the application holding it?** Firmware can force it up or down with
      `espos_wifi_portal_open()`, which wins over everything below except
      `portal_enabled`. `GET /api/v1/wifi/status` shows this as `portal.force`
      (`auto`, `up` or `down`), so no setup network and `force: down` means the
      application is keeping it shut. A forced-up setup network appears when
      the current connection attempt ends, whether it worked or not. A reboot
      clears the setting.
    * **Is another link up?** While Ethernet (or any other link) carries the
      network, there is no setup network, because you can reach the device that
      way. `GET /api/v1/net/status` names the link. `wifi.portal_online = true`
      turns the setup network on anyway.
    * **Are networks configured?** The setup network comes up at once only when
      no network is configured or `wifi.sta_enabled` is off. With networks
      configured it comes up after `wifi.portal_after_s` (90 s) of failed
      attempts. It never comes up while connected, or when
      `wifi.portal_enabled` is off.

    If a phone doesn't open the sign-in page by itself, browse to
    `http://192.168.4.1`. Setting `wifi.portal_psk` puts a password on the
    setup network. [WiFi → Portal](wifi.md#portal-softap-provisioning).

**The setup network appears but a phone gets no address**
:   Look for `portal: lease <ip> to <mac>` in the log. If it is missing, the
    phone joined and the device's DHCP server didn't answer. If it is there,
    the phone got its address and the browser is the problem. A phone that
    joins and is dropped a few seconds later with no lease line is the first
    case. We have seen this on a P4 whose WiFi had been failing to connect for
    days, and a reboot cleared it.

**It never connects; `reason: {code, text}` in `GET /api/v1/wifi/status`**
:   | Code | Meaning |
    |---|---|
    | `202`, `2`, `15` | Wrong password. Also seen with a password under 8 characters: WPA needs 8 to 63, so that network is skipped with a warning. |
    | `201` | The network is out of range, or hidden. |
    | `204` | The handshake timed out: weak signal. |
    | `200` | The beacon was lost: out of range, or the access point went away. |
    | `1001` | Connected, but no address from DHCP: check the router. |

    The full table is in [WiFi → State machine](wifi.md#state-machine). The
    device refuses a password for an open or WEP network on purpose.

**`<hostname>.local` doesn't resolve**
:   mDNS is off in the build (`CONFIG_ESPOS_NET_MDNS=n`), the computer has no
    mDNS (older Windows), or it is on another subnet, which mDNS doesn't
    cross. The monitor's `connected to … as <ip>` line has the address.

**ESP32-P4: WiFi says `connected` but nothing works, and the log is full of `rpc_core: Timeout waiting for Resp`**
:   The SDIO link to the C6 radio chip has jammed. The disconnect message
    can't cross a jammed link, so the device still thinks it is connected.
    Two settings cause this, and both are fixed in
    `sdkconfig.d/espos.defaults.esp32p4`, so don't override them:

    * a receive block-ack window of 16 (ESP-IDF raises
      `CONFIG_WIFI_RMT_RX_BA_WIN` from 6 as soon as PSRAM is on);
    * the transport memory pool in PSRAM together with 128-byte cache lines,
      which makes the SDIO driver reject the buffers with `ESP_ERR_INVALID_ARG`.

    A third cause shows at boot as `SDIO mode mismatch … Aborting`: streaming
    mode has to stay on. The fix is a restart, and the link watchdog does it
    after three missed 20 s heartbeats (the co-processor heartbeat watchdog). Never call `esp_hosted_deinit()` and
    `init()` to recover; that crashes instead of failing.
    [WiFi → Co-processor link watchdog](wifi.md#co-processor-link-watchdog-esp_hosted-boards).

## Signal K

**`no server (waiting for discovery or manual host)`**
:   The device looks for `_signalk-http._tcp` over mDNS, which doesn't cross
    subnets or VLANs. The server also has to announce itself; signalk-server
    does by default. On a laptop, `avahi-browse -rt _signalk-http._tcp` shows
    what the device sees. On a network without mDNS, set `sk.server_host` and
    `sk.server_port` (default 80) on the device's Signal K page, or with
    `PUT /api/v1/config {"sk": {"server_host": "192.168.1.10", "server_port": 3000}}`.
    If the boat has several servers and the device picked the wrong one,
    `sk.server_self` chooses one by its self URN, and `sk.server_pin` keeps the
    current one. [Signal K → Discovery](signalk.md#discovery).

**`access denied by the server — request again from the device's web UI when it is allowed`**
:   Someone denied the request, or the server has device access requests
    turned off (it answers `403`). The device doesn't retry by itself, on
    purpose. Fix it on the server (Security → Access Requests, or its
    settings), then press **Request access** on the device's Signal K page
    (`POST /api/v1/sk/request`).

**It was approved, and now it is `requesting` again, with a new entry in Access Requests**
:   The token was revoked. Either the device was deleted under Security →
    Devices, or the server was reinstalled and has a new self URN, so the old
    token no longer counts. The device drops the token on the first `401` and
    asks again; approve it. If the log says `already requested` (`400`), the
    server still has an open request from this device: approve or deny that
    one. [Signal K → Token state machine](signalk.md#token-state-machine).

**`approved, streaming`, but the server shows no values**
:   Check `GET /api/v1/sk/status`: `ws` has `connected`, `sent`, `buffered`
    and `dropped`.

    * Sending data needs **read/write** access (`sk.permissions`). With
      read-only access the server ignores what the device sends.
    * With `sk.ws_enabled` off the device keeps buffering but never opens the
      connection.
    * A rising `dropped` count means the offline buffer is too small for the
      outage: raise `sk.buffer_msgs` or `sk.buffer_kb`.

    [Signal K → Delta stream](signalk.md#delta-stream-m4).

**`Failed to create socket` or `httpd_accept_conn: error in accept (23)` in the log**
:   The device has run out of sockets (23 is "too many open files"). The web
    server, the delta stream and every call to the server share one table of
    `CONFIG_LWIP_MAX_SOCKETS`. At IDF's 10 the table runs out as soon as a
    browser holds a few connections to the web UI, and the device's own calls
    to the server fail until the browser lets go. espOS needs 11 +
    `CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS` +
    `CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT`, which is 16 at the defaults.
    Compare that with the value in the build's `sdkconfig`:

    * below it, the firmware was built with an espOS that did not check it
      (the build now stops and names the number): update espOS, or raise
      the value in the firmware's `sdkconfig.defaults`;
    * at or above it, the firmware opens connections of its own on top of
      espOS's: raise it in the firmware's `sdkconfig.defaults`.

**A PUT comes back `COMPLETED` with `statusCode 405 "PUT not supported for …"`**
:   That is the server's real answer: no plugin on the server handles PUTs for
    that path. Install or write one on the server; the device did its part.
    [Signal K → Inbound](signalk.md#inbound-m7).

**The device restarts every 5 minutes or so while the server is down**
:   That is the `skLinkStalled` check. It fires when WiFi is connected, the
    stream worked once since boot, and it has been down for `sk.stall_s`
    (300 s). It exists for the P4's jammed-link case above. If your server is
    often down for longer, raise `sk.stall_s` or turn the watchdog off
    ([Health](health.md#the-watchdog-policy)).

**The device restarted after half an hour without WiFi**
:   That is the `wifiStalled` check: `wifi.stall_s` (1800 s) of trying without
    associating once. It restarts once per outage, so a device whose access
    point stays off does not loop. Set `wifi.stall_s` to 0 to turn it off
    ([Health](health.md#why-wifistalled-restarts-once)).

## OTA

**`image rejected: bad signature or corrupt`**
:   The image was signed with a key the device doesn't trust. Most often it is
    the development key, which every fresh checkout makes anew, so a device
    flashed from one clone rejects images built in another clone or in CI.
    Keep one key outside the repository, and flash each device once over USB
    with a build signed by it. The device also rejects an image whose
    `project_name` differs from the running app's, and a corrupt download. To
    check a file:
    `espsecure verify-signature --version 2 --keyfile <key>.pem build/<app>.bin`.
    [OTA → Signing key](ota.md#signing-key).

**I changed the key and rebuilt, and the device still rejects the image**
:   ESP-IDF only re-signs when the unsigned binary changes, so a new key with
    no source change keeps the old signature. The espOS prologue notices the
    new key and forces a re-link. A project with its own CMake that copied the
    signing lines without that part has to delete `build/.bin_timestamp` by
    hand. [OTA → Swapping the key](ota.md#swapping-the-key).

**`rolled_back: true` in `GET /api/v1/ota/status`**
:   The new image didn't confirm itself. It crashed before WiFi came up, or it
    didn't reach the network within `ota.confirm_tmo_s` (600 s), so the
    bootloader went back to the previous image. `other_version` names the
    failed build. A crash leaves a core dump ([below](#core-dumps)).

**A newer build is on the server but `available` stays `null`**
:   The manifest entry has to match the device's `target`, `channel`
    (`ota.channel`, default `stable`) and `app`, and be strictly newer than the
    running version. `1.0.0-beta.1` counts as older than `1.0.0`, and a
    `-dirty` build is compared as plain text. `ota.last_error` says when a
    manifest didn't parse or was over 16 KiB.
    [OTA → Manifest format](ota.md#manifest-format-schema-1).

## Memory

**A `lowMemory` notification**
:   It warns below 40 KB free in total or 20 KB of internal RAM. It becomes an
    alarm below 12 KB of internal RAM, or when the largest free internal block
    is under 8 KB, and after three alarms in a row (10 s apart) the device
    restarts.

    On a board with PSRAM, watch the internal numbers. Task stacks, the radio
    and DMA all need internal RAM, and megabytes of free PSRAM hide the
    problem. The device publishes `espos.<hostname>.internalFree` and
    `largestBlock` to the server. Move large buffers to PSRAM with
    `heap_caps_malloc(…, MALLOC_CAP_SPIRAM)`, and keep the P4's transport pool
    in PSRAM as the defaults do. Each open TLS connection costs about 20 KB
    (`CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT`).
    [Health → Built-in conditions](health.md#built-in-conditions).

## Restarts and watchdogs

**The device restarted and you don't know why**
:   Look at `GET /api/v1/system/info`:

    * `last_reset` is set: the health watchdog restarted it. `health_key` and
      `message` name the reason (`lowMemory`, `taskStalled`, `skLinkStalled`,
      or one of your own), with the lowest free memory before it.
    * `reset_reason: task_wdt`: a task watched with `espos_health_watch_task()`
      was silent for 30 s. There is a core dump.
    * `panic`: a crash. There is a core dump.
    * `brownout`: the power supply dipped.

    Losing WiFi never restarts a device; that is deliberate.
    [Health → The reset record](health.md#the-reset-record).

**An ESP32-P4 restarts about 60 s after the network stops working**
:   That is the co-processor heartbeat watchdog. The jammed link is the fault and the
    restart is the cure ([WiFi](#wifi) above).

**Turning the restarts off**
:   Set `espos_start_opts_t.health_watchdog = false`, or
    `CONFIG_ESPOS_CORE_HEALTH_WATCHDOG=n` for the whole firmware. Problems are
    still reported; the device just doesn't restart for them.

## Logs

The device keeps the last 16 KiB of log lines, including the boot, so you
can read them without a cable:

```sh
H=http://espos-1a2b.local
curl -s "$H/api/v1/logs?limit=200" | python3 -c 'import json,sys; print(*json.load(sys.stdin)["lines"], sep="\n")'
curl -s -X PUT -H 'Content-Type: application/json' -d '{"level":"debug","tag":"espos_sk"}' "$H/api/v1/logs/level"
```

To keep reading, pass `?after=<next - 1>` from the previous answer. The log
level change lasts until the next restart. The Logs page in the web UI does
the same, with a filter, follow and download.
[REST API → Logs](rest-api.md#logs-m5).

## Core dumps

A crash writes a core dump to the `coredump` partition. You can fetch a
summary and the dump over the network, and decode it with the ELF of that
exact build (`app_elf_sha256` in the summary tells you which):

```sh
H=http://espos-1a2b.local
curl -s "$H/api/v1/system/coredump"                       # task, pc, app_elf_sha256, …
curl -s -o coredump.bin "$H/api/v1/system/coredump/raw"
espcoredump.py --chip esp32c6 info_corefile -c coredump.bin -t raw build/minimal.elf
curl -s -X DELETE "$H/api/v1/system/coredump"
```

`espcoredump.py` is in `$IDF_PATH/components/espcoredump/` and on your
`PATH` in an exported ESP-IDF shell. The Status page of the web UI shows the
summary and has buttons to download and erase it.
[REST API → `/system/coredump`](rest-api.md#get-systemcoredump-m5-protected).
The tutorial [Logs and core dumps](tutorials/logs-and-core-dumps.md) walks
through a deliberate crash from start to finish.
