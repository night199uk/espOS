# Troubleshooting

By symptom; each entry says why it happens and what fixes it, and links the
component page that owns the behaviour. Three places answer most questions
before any of this: the **monitor** (`idf.py monitor`), which narrates the
boot ([Concepts](concepts.md#espos_start-the-order-and-why)); the device's
**log ring** at `GET /api/v1/logs` and the Logs page of the web UI, which
keep the boot log without a cable ([below](#logs)); and `GET /api/v1/system/info`,
whose `reset_reason` and `last_reset` say why the device last restarted.

## Build

**`IDF_PATH is ESP-IDF vX.Y.Z; espOS builds with v6.0.0 up to (not including) v6.1.0`**
: The IDF version policy (`cmake/espos_version.cmake`). espOS is tested on
  the release named in `.idf-version`; any 6.0.x builds with one warning
  naming the tested one; another minor is refused, because it moves
  component APIs (esp_hosted, TWAI, the linux target) under the code. Fix:
  install a 6.0.x release and source its `export.sh`. To try anyway,
  `-DESPOS_ALLOW_IDF_MISMATCH=1` — and say so when reporting a problem.

**`.idf-version pins vA but the espOS submodule pins vB`**
: Your firmware carries its own `.idf-version` and it disagrees with the
  submodule's. Align them (espOS's pin is the one its CI tests) or delete the
  project's file to follow espOS.

**`'…/components' has no espos_config — if espOS is a submodule, run git submodule update --init`**
: The submodule directory is empty. `git submodule update --init` in the
  firmware project.

**`git describe here says v0.7.0-66-… which names an older tag`**
: Not a fault, and the build is fine — the line says which espOS you have
  (`base` is read from the submodule's `version.txt`, which is tracked and
  therefore always right). `git submodule update` never fetches new tags, so
  the submodule's idea of the newest release is as old as its last `--init`.
  `git -C espos fetch --tags` silences it. [Releasing](releasing.md#consumers).

**`the espOS pin is at tag vX but its version.txt says Y`**
: This one *is* a fault, and the two cannot be reconciled by fetching: a tag
  reachable from `HEAD` is a tag the checkout already has, so the pinned commit
  genuinely carries a `version.txt` that was never bumped to match its tag.
  A device would report whichever the build preferred. Check the pinned commit.

**The web page still behaves like an older release after an OTA update** (`ui` missing from `/system/info`, or `ui: "storage"`)
: Firmware before the page was built into the image served it from the
  LittleFS `storage` partition, which OTA never writes, so the page stayed
  the one flashed over USB. Update to a firmware whose `/system/info` reports
  `ui: "embedded"` and reload the page (Ctrl/Cmd-Shift-R). A project with its
  own `DIR` bundle is still served from storage; reflash it over USB with
  `idf.py flash`. [Web UI](ui.md).

**The device serves a placeholder page instead of the web UI** (`ui: "placeholder"` in `/system/info`; boot log `ui storage … has no index.html: serving the embedded page`)
: There is no bundle in the image or in storage. With
  `CONFIG_ESPOS_HTTPD_UI_EMBED` off, or a custom `DIR`, the page comes from
  the LittleFS `storage` partition, and it has none. Either the project never
  called `espos_project_ui_partition()` after `project()`, or a custom `DIR`
  was given and is missing (that is a CMake *warning*, not an error), or the
  image was written with `idf.py app-flash`, which skips `storage.bin` —
  `idf.py flash` writes it. A missing *default* bundle (`espos_httpd/ui-dist`)
  is a hard configure error: restore it from git. [Web UI](ui.md).

**`partitions.csv` not found after bumping espOS** (or the OTA slots vanished)
: The partition table moved to `components/espos_core/partitions/4mb.csv` and is now selected by
  the prologue's `PARTITIONS` argument. IDF applies defaults only to options
  a `sdkconfig` does not already set, so a stale `build/sdkconfig` still
  names the old path. Delete `build/sdkconfig` (or the build directory) once
  and reconfigure. The same applies after editing any `sdkconfig.d/*` file.
  [Development](development.md#building-a-firmware-on-espos).

**`Failed to resolve component 'espos_ble' required by component 'main'`** (likewise `espos_n2k`, `espos_voice`, `espos_audio`)
: Consumer projects build with `MINIMAL_BUILD`, and the optional espOS
  components are excluded outright unless named:
  `espos_project_prologue(… COMPONENTS espos_ble)`. The core set needs no
  listing — naming one of those is itself an error (`… is not an optional
  espOS component`). [Development](development.md#building-a-firmware-on-espos).

**`no secure_boot_signing_key.pem — generating a development RSA-3072 signing key`**
: Expected on a fresh checkout; the key is git-ignored. It matters the day
  you ship: see [OTA](#ota) below.

**The build stops fitting** (`size report`, `ota_0` over 90 %)
: The app slot is 1728 KB on the default 4 MB table. A Bluedroid firmware is
  larger than that on a native-radio target — the BLE gateway measures
  2.13 MB on an ESP32-C6 — so move to `components/espos_core/partitions/8mb.csv`
  ([Hardware](hardware.md#flash-size-and-partition-tables)). Two such slots
  plus storage do not fit a 4 MB chip at all, so a 4 MB board can carry that
  firmware only without OTA, in a single `factory` slot.

## WiFi

**No `espOS-xxxx` network appears**
: First check whether something else is carrying the network: **by default
  there is no access point while Ethernet (or any other transport) has the
  default route**, because the device is reachable over that instead.
  `GET /api/v1/net/status` names the interface, and `wifi.portal_online = true`
  raises the portal anyway. Otherwise: the portal comes up at once only when
  *no* network is configured or `wifi.sta_enabled` is off. With networks
  configured it appears after `wifi.portal_after_s` (90 s) of failed attempts,
  and never while connected or with `wifi.portal_enabled` off. An application
  can also hold it down — or up — with `espos_wifi_portal_open()`, which
  outranks all of the above except `portal_enabled`. A request to hold it *up*
  still waits for any association already in flight to finish, so on a device
  cycling through connection attempts the access point appears when the current
  attempt ends rather than immediately — bounded by the attempt, not by its
  success. The request is not persisted, so a reboot clears it, and
  `GET /api/v1/wifi/status` reports it as `portal.force` — `auto`, `up` or
  `down` — so "no access point and `force: down`" is an application holding it
  shut rather than any of the reasons above. A phone that does not pop its
  sign-in sheet still reaches the
  page at `http://192.168.4.1`. A `wifi.portal_psk` makes the portal a protected
  network. [WiFi → Portal](wifi.md#portal-softap-provisioning).

**The access point appears but a phone gets no address**
: Look for `portal: lease <ip> to <mac>` in the log. Without it the client
  associated and the DHCP server did not answer; with it the lease was handed
  out and the browser is the problem. A client that associates and is dropped a
  few seconds later with no lease line is the first case. Seen on a P4 whose
  station had been failing to associate for days: a reboot cleared it.

**It never connects; `reason: {code, text}` in `GET /api/v1/wifi/status`**
: `202`, `2`, `15` — wrong password (also seen when a password is under 8
  characters: that slot is skipped with a warning, WPA needs 8..63). `201` —
  network not in range or a hidden SSID. `204` — handshake timed out, weak
  signal. `200` — lost the beacon: out of range or the AP went away. `1001`
  — associated but no DHCP lease: the router. The full table is in
  [WiFi → State machine](wifi.md#state-machine). A password set on an
  open/WEP network is refused on purpose.

**`<hostname>.local` does not resolve**
: mDNS is off in the build (`CONFIG_ESPOS_NET_MDNS=n`), or the client has
  no mDNS (older Windows), or you are on another subnet — mDNS does not
  cross routers. The monitor's `connected to … as <ip>` line has the address.

**ESP32-P4: WiFi says `connected`, nothing works, log full of `rpc_core: Timeout waiting for Resp`**
: The SDIO link to the C6 co-processor wedged; the disconnect event cannot
  cross the jammed link, so the state machine keeps saying connected. Two
  configuration landmines cause it: a receive block-ack window of 16 (IDF
  raises `CONFIG_WIFI_RMT_RX_BA_WIN` from 6 as soon as PSRAM is enabled),
  and the transport mempool in PSRAM combined with 128-byte L2 cache lines
  (the SDIO driver rejects the buffers with `ESP_ERR_INVALID_ARG`). Both are
  pinned in `sdkconfig.d/espos.defaults.esp32p4`; do not override them.
  `SDIO mode mismatch … Aborting` at boot is the third: streaming mode must
  stay on. Recovery is a restart — the heartbeat watchdog does it after
  three missed 20 s beats — **never** `esp_hosted_deinit()`/`init()`, which
  asserts rather than failing. [WiFi → Co-processor link watchdog](wifi.md#co-processor-link-watchdog-esp_hosted-boards).

## Signal K

**`no server (waiting for discovery or manual host)`**
: Discovery browses `_signalk-http._tcp` over mDNS, which does not cross
  subnets or VLANs; the server also has to advertise (signalk-server does by
  default; `avahi-browse -rt _signalk-http._tcp` on a laptop shows what the
  device sees). On a network without mDNS set `sk.server_host` (and
  `sk.server_port`, default 80) on the device's Signal K page or with
  `PUT /api/v1/config {"sk": {"server_host": "192.168.1.10", "server_port": 3000}}`.
  Several servers on the boat and the wrong one chosen: `sk.server_self`
  pins one by its self URN, `sk.server_pin` keeps the current choice.
  [Signal K → Discovery](signalk.md#discovery).

**`access denied by the server — request again from the device's web UI when it is allowed`**
: Someone denied the request, or the server has device access requests
  disabled (the request is answered `403`). Nothing retries by itself, on
  purpose. Fix the server side (Security → Access Requests, or its
  settings), then **Request again** on the device's Signal K page
  (`POST /api/v1/sk/request`).

**Approved once, now `requesting` again; a new entry in Access Requests**
: The token was revoked — the device was deleted under Security → Devices,
  or the server was reinstalled (new self URN, so the stored token no longer
  applies). The device drops the token on the first `401` and requests
  again; approve it. If the log says `already requested` (`400`), the server
  still holds a pending request for this client: approve or deny that one.
  [Signal K → Token state machine](signalk.md#token-state-machine).

**`approved, streaming`, but no values on the server**
: `GET /api/v1/sk/status` → `ws`: `connected`, `sent`, `buffered`,
  `dropped`. Publishing needs the `readwrite` permission (`sk.permissions`);
  with read-only the server ignores deltas. `sk.ws_enabled` off keeps
  buffering without a socket. `dropped` rising means the offline ring is too
  small for the outage (`sk.buffer_msgs`, `sk.buffer_kb`).
  [Signal K → Delta stream](signalk.md#delta-stream-m4).

**A PUT answers `COMPLETED` with `statusCode 405 "PUT not supported for …"`**
: The server's honest answer: no plugin has registered a PUT handler for
  that path. Install or write one on the server side; the device did its
  part. [Signal K → Inbound](signalk.md#inbound-m7).

**The device restarts every ~5 minutes while the server is down**
: `skLinkStalled`: WiFi connected, the stream has worked once this boot,
  down for `sk.stall_s` (300 s). It exists for the P4's wedged-link case.
  If your server is routinely down longer, raise `sk.stall_s` or turn the
  watchdog off ([Health](health.md#the-watchdog-policy)).

## OTA

**`image rejected: bad signature or corrupt`**
: The image was signed with a key the device does not trust — most often
  the *development* key, which every fresh checkout generates anew: a device
  flashed from one clone rejects images built in another (or in CI). Keep
  one key, outside the repository, and flash each device once over USB with
  a build carrying it. Also rejected: an image whose `project_name` differs
  from the running app's, and a corrupt download. Verify a file with
  `espsecure verify-signature --version 2 --keyfile <key>.pem build/<app>.bin`.
  [OTA → Signing key](ota.md#signing-key).

**Swapped the key, rebuilt, the device still rejects the image**
: ESP-IDF's signing step depends on the unsigned binary alone, so a new key
  without a source change keeps the *old* signature. The prologue detects
  the change and forces a re-link; a project with its own CMake that copied
  the signing lines without that block deletes `build/.bin_timestamp` by
  hand. [OTA → Swapping the key](ota.md#swapping-the-key).

**`rolled_back: true` in `GET /api/v1/ota/status`**
: The new image failed to confirm itself — it panicked before WiFi came up,
  or never reached the network within `ota.confirm_tmo_s` (600 s) — and the
  bootloader booted the previous slot; `other_version` names the failed
  build. A panic leaves a core dump ([below](#core-dumps)).

**A newer build is on the server but `available` stays `null`**
: The manifest entry must match `target`, `channel` (`ota.channel`,
  default `stable`) and `app`, and be *strictly* newer than the running
  version (`1.0.0-beta.1 < 1.0.0`; a `-dirty` build compares as a string).
  `ota.last_error` names a manifest that failed to parse or exceeded 16 KiB.
  [OTA → Manifest format](ota.md#manifest-format-schema-1).

## Memory

**`lowMemory` notification**
: WARN below 40 KB total or 20 KB internal free; ALARM — fatal, restarts
  after three consecutive 10 s ticks — below 12 KB internal or an 8 KB
  largest free internal block. On a board with PSRAM the *internal* numbers
  are the ones that matter: task stacks, the radio and DMA come from
  internal RAM, and tens of megabytes of PSRAM hide its exhaustion. Watch
  `espos.<hostname>.internalFree` and `largestBlock` on the server; move
  large buffers to PSRAM (`heap_caps_malloc(…, MALLOC_CAP_SPIRAM)`); keep
  the P4's hosted mempool in PSRAM as the defaults do. Each open TLS
  connection costs about 20 KB (`CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT`).
  [Health → Built-in conditions](health.md#built-in-conditions).

## Restarts and watchdogs

**The device restarts and you do not know why**
: `GET /api/v1/system/info`. `last_reset` non-null: the health policy
  restarted it, and `health_key`/`message` say on which condition
  (`lowMemory`, `taskStalled`, `skLinkStalled`, or your own) with the heap
  low-water marks before it. `reset_reason: task_wdt`: a task registered
  with `espos_health_watch_task()` was silent for 30 s — there is a core
  dump. `panic`: core dump. `brownout`: the power supply. Loss of WiFi never
  restarts a device by design. [Health → The reset record](health.md#the-reset-record).

**ESP32-P4 restarts about 60 s after the network stalls**
: The co-processor heartbeat watchdog; the stall is the fault, the restart
  is the recovery ([WiFi](#wifi) above).

**Turning the restarts off**
: `espos_start_opts_t.health_watchdog = false`, or
  `CONFIG_ESPOS_CORE_HEALTH_WATCHDOG=n` for the whole firmware; conditions
  are still reported, strikes are never counted.

## Logs

Without a cable, the ring holds the last 16 KiB of console lines including
the boot:

```sh
H=http://espos-1a2b.local
curl -s "$H/api/v1/logs?limit=200" | python3 -c 'import json,sys; print(*json.load(sys.stdin)["lines"], sep="\n")'
curl -s -X PUT -H 'Content-Type: application/json' -d '{"level":"debug","tag":"espos_sk"}' "$H/api/v1/logs/level"
```

Poll on with `?after=<next - 1>` from the previous reply; the level change
is runtime-only. The web UI's Logs page does the same with filter, follow
and download. [REST API → Logs](rest-api.md#logs-m5).

## Core dumps

A panic writes a core dump to the `coredump` partition; the summary and the
image are served over REST, and the ELF of *that exact build* decodes it
(`app_elf_sha256` in the summary identifies the build):

```sh
H=http://espos-1a2b.local
curl -s "$H/api/v1/system/coredump"                       # task, pc, app_elf_sha256, …
curl -s -o coredump.bin "$H/api/v1/system/coredump/raw"
espcoredump.py --chip esp32c6 info_corefile -c coredump.bin -t raw build/minimal.elf
curl -s -X DELETE "$H/api/v1/system/coredump"
```

`espcoredump.py` is `$IDF_PATH/components/espcoredump/espcoredump.py`, on
`PATH` in an exported IDF shell. The web UI's Status page shows the
summary and offers the download and the erase. [REST API →
`/system/coredump`](rest-api.md#get-systemcoredump-m5-protected); the tutorial
[Logs and core dumps](tutorials/logs-and-core-dumps.md) walks through a
deliberate crash end to end.
