# espOS REST API — v1

Base path: `/api/v1`. All request and response bodies are JSON
(`Content-Type: application/json`) unless stated otherwise. The UI (M5) is
developed against this document, so **changing anything here is a
cross-component decision** — raise it before editing.

Status of each endpoint: **M1** = implemented now; later milestones are listed
so the shape is agreed early and marked *planned*. Each endpoint is also marked
**public** or **protected** — whether [authentication](#authentication) applies
once an API key is set.

## Conventions

* Errors are always
  ```json
  {"error": "<machine_code>", "message": "<human text>"}
  ```
  with an appropriate 4xx/5xx status — including the server's own errors
  (`404 not_found`, `405 method_not_allowed`, `400 bad_request`, `408
  timeout`, `411 length_required`, `413 too_large`, `414 uri_too_long`, `431
  headers_too_large`, `500 internal`). Validation errors on `PUT /config`
  add a `path` (see below).
* While a reboot or factory reset is pending (≈500 ms) writes are refused
  with `503 restarting`.
* **State-changing requests (`PUT`, `POST`) must send `Content-Type:
  application/json`**, otherwise `415 unsupported_media_type`. This is the
  CSRF guard: a browser cannot send that header cross-origin without a CORS
  preflight, which the device never grants. `curl -X POST -H
  'Content-Type: application/json' …` for the system endpoints. A request
  authenticated by the login *cookie* must in addition come from the device's
  own origin (see Authentication).
* Responses that must not be cached carry `Cache-Control: no-store`.
* Secrets (descriptor `secret: true`) are never returned. They read back as
  the sentinel `"********"` when set and `""` when unset; writing the
  sentinel is a no-op, so an exported document can be imported unchanged.
* Blobs are base64 strings (RFC 4648, padded).
* **Authentication** is the section below. With `httpd.api_key` unset the
  API is open to anyone on the network (the default — treat it like the
  SensESP config UI); once a key is set, every endpoint marked *protected*
  needs `Authorization: Bearer <key>` or the login cookie, else `401`.

## Authentication

Everything is **protected** unless marked **public**. Protection is off until
`httpd.api_key` is set (so an update keeps an existing device open); from then
on a protected request must carry one of

* `Authorization: Bearer <key>` — machine clients: the designer, a fleet
  plugin, scripts. Stateless.
* the `espos_sid` cookie from `POST /auth/login` — browsers. `fetch()` and
  `EventSource` send it by themselves; `HttpOnly; SameSite=Strict; Path=/`.

or arrive on the device's own setup-portal network **while that network is
exempt**, which it is only when no key is configured or a recovery window is
open ([security.md](security.md)). Otherwise:

| Status | `error` | When |
|---|---|---|
| `401` | `unauthorized` | no or invalid credential, a stale cookie included; carries `WWW-Authenticate: Bearer realm="espOS"` |
| `403` | `forbidden` | a cookie-authenticated `PUT`/`POST`/`DELETE` whose `Origin` (else `Referer`) host is not the `Host`, or that has neither — Bearer requests skip this |
| `403` | `auth_unconfigured` | the build has `CONFIG_ESPOS_HTTPD_AUTH_REQUIRED=y` and no key is set yet; set one from the portal, which is exempt exactly while that is true |
| `429` | `too_many_attempts` | five wrong keys within 60 s: every key check (login and Bearer, the right key too) answers this for 30 s, `Retry-After` says how long; live cookies keep working |

The check runs before the handler, for every endpoint registered through
`espos_httpd_register()` — an application's own included; an endpoint that
must stay open registers with `espos_httpd_register_ex(uri, ESPOS_HTTPD_PUBLIC)`.
Changing or clearing the key drops every session. Over plain http the key
crosses the network in clear, like the SignalK token does.

### `POST /auth/login` — public

Body `{"key": "<httpd.api_key>"}` (JSON content type required) → `204` with

```
Set-Cookie: espos_sid=<32 hex>; HttpOnly; SameSite=Strict; Path=/; Max-Age=<httpd.session_ttl_s>
```

`401 unauthorized` for a wrong key (counted), `429` while throttled, `409
auth_open` when no key is configured, `400 validation` for a bad body.
Sessions live in RAM: `CONFIG_ESPOS_HTTPD_MAX_SESSIONS` (4) at a time — one
more evicts the session idle longest — and a reboot ends them all.

### `POST /auth/logout` — public

Drops the session the cookie names, if any → `204` with a cookie of
`Max-Age=0`. JSON content type required.

### `GET /auth/status` — public

```json
{"required": true, "configured": true, "authenticated": true, "method": "cookie", "recovery_s": 0}
```

`required`: protected endpoints need a credential (a key is set, or the build
requires one); `configured`: a key is set; `authenticated`: *this* request
carried a valid one, or came from the portal while the portal was exempt;
`method` ∈ `none bearer cookie portal`; `recovery_s`: seconds left of a portal
recovery window, `0` when none is open (docs/security.md). The web UI decides
from this whether to show its login page. A wrong Bearer on this endpoint
counts toward the throttle like any other key check.

## Configuration

### `GET /config` — M1 · protected

Effective configuration (stored values, else compiled-in defaults), one
object per NVS namespace:

```json
{
  "app":   {"label": "espOS device", "enabled": true, "interval_ms": 1000, ...},
  "httpd": {"port": 80}
}
```

Query: `?ns=<namespace>` limits the response to that namespace
(`404 unknown_namespace` if it does not exist or is longer than a namespace
can be; `414 uri_too_long` for a query string ≥ 128 bytes).

### `PUT /config` — M1 · protected

Body: same shape as `GET`, **partial documents allowed**. Semantics:

| Value in body                    | Effect                                    |
|----------------------------------|-------------------------------------------|
| key present with a value         | validated, then written                   |
| key present with `null`          | reset to the compiled-in default          |
| key absent                       | untouched                                 |
| secret key set to `"********"`   | untouched                                 |
| unknown namespace or key         | `400`, nothing written                    |

The whole document is validated first; on any validation error **nothing is
written**. (A storage failure while applying an already-validated document
is reported as `500 write_failed`; keys before the failing one may have been
written.)

Success `200`:
```json
{"changed": ["app.interval_ms", "httpd.port"], "restart_required": true}
```
`changed` lists keys whose *effective* value changed (writing the current
value is not a change). `restart_required` is true if any changed key is
flagged `restart_required` in its descriptor.

Failure `400`:
```json
{"error": "validation", "path": "app.interval_ms", "message": "out of range [100,60000]"}
```
`path` is `"<ns>.<key>"`, `"<ns>"`, or `""` for document-level errors
(`"malformed JSON"`, `"expected object of namespaces"`).

Other statuses: `413 too_large` (body over `CONFIG_ESPOS_HTTPD_MAX_BODY`,
default 16 KiB), `408 timeout` (body did not arrive), `503 restarting`,
`500 write_failed`. Trailing non-whitespace after the JSON document is
`400 validation` (`"malformed JSON"`).

### `GET /config/schema` — M1 · protected

JSON Schema (draft 2020-12) of the whole configuration document, generated
at build time from the config descriptors. `Content-Type:
application/schema+json`. Sent with an `ETag`; a request with a matching
`If-None-Match` gets `304`. Vendor extensions used by the UI:

| Extension                   | On          | Meaning                                  |
|-----------------------------|-------------|------------------------------------------|
| `x-espos-version`           | namespace   | descriptor schema version                |
| `x-espos-secret: true`      | key         | write-only secret (also `writeOnly`, `format: password`) |
| `x-espos-restartRequired`   | key         | takes effect after reboot                |
| `x-espos-unit`              | key         | display unit                             |
| `x-espos-type: "blob"`      | key         | base64 bytes; `x-espos-maxBytes` is the decoded limit |

## System

### `GET /system/ping` — public

```json
{"app": "espos", "version": "0.7.0-3-gabc1234", "auth": true}
```
Liveness for a fleet page or a discovery tool: which firmware, and whether it
wants a key (`auth` is `/auth/status`'s `required`). Nothing mDNS does not
already advertise.

### `GET /system/info` — M1 · protected

```json
{
  "app": "espos", "version": "0.1.0-3-gabc1234", "idf_version": "v6.0.2",
  "chip": "esp32c6", "chip_revision": 1, "cores": 1,
  "uptime_s": 42, "free_heap": 210000, "min_free_heap": 190000,
  "free_internal": 148000, "min_internal_free": 121000,
  "reset_reason": "software", "config_storage_reset": false,
  "schema_etag": "6acfba355e183b19", "ui_storage": true,
  "hardware": {
    "mac": "60:55:f9:00:1a:2b", "cpu_mhz": 160, "flash_bytes": 8388608,
    "ram_internal_bytes": 524288, "ram_psram_bytes": 0,
    "features": ["wifi", "ble", "802.15.4", "embedded-flash"],
    "board": "Espressif ESP32-C6-DevKitC-1"
  },
  "time": {"synced": true, "source": "sntp", "now": 1788775933456},
  "last_reset": {
    "reason": "software", "health_key": "skLinkStalled",
    "message": "stream down for over 300 s while WiFi reports connected",
    "min_free_heap_before": 148216, "min_internal_before": 31720,
    "largest_block_before": 25600, "uptime_before_s": 86742,
    "at": "2026-09-07T04:12:31Z"
  }
}
```

On a host whose radio is a separate chip — an ESP32-P4 with an ESP32-C6 over
SDIO — `hardware` also carries the co-processor. A C6 like the one above has its
own radio and never does. A different device from the one above, and from the
7B in the `espos_start()` snippet below; all three strings are real boards on
one bench, so do not read `board` as derived from the chip:

```json
"hardware": {
  "mac": "30:ed:a0:e3:2b:e9", "cpu_mhz": 360, "flash_bytes": 16777216,
  "board": "Waveshare ESP32-P4-WIFI6-Touch-LCD-X 7in",
  "coprocessor": {"version": "0.0.0", "host_version": "3.0.9",
                  "target": "esp32c6", "stale": true}
}
```

`host_version` is what this build's `esp_hosted` expects to talk to, `target`
the co-processor's chip, and `stale` whether the host is newer. Where each one
comes from matters:

| field | source |
| --- | --- |
| `version`, `target` | the PRIV TLVs the co-processor sends during transport bring-up, which `esp_hosted` has already parsed and kept |
| `host_version` | this build's own `ESP_HOSTED_VERSION_*`, a compile-time constant |
| `stale` | `esp_hosted`'s verdict on the two (`eh_host_mcu_transport_verify_fw_compat()`) |

What none of them is: `esp_hosted_get_coprocessor_fwversion()`, an RPC to the
other chip. That is the whole reason the object can be trusted — a co-processor
old or broken enough to be worth reporting is exactly the one whose RPC does
not answer, so a query-based field would be absent precisely when it matters.
Taking esp_hosted's verdict rather than comparing the two strings here is what
keeps `stale` from disagreeing with the warning esp_hosted logs: a patch-level
difference is not stale, and a host *older* than its co-processor is a
different problem and not this flag's.
`stale: true` is also raised as the `coprocessorStale` health condition
([hardware.md](hardware.md#updating-the-c6-co-processor-firmware)).

The accessors that read those TLVs are not on the include path `esp_hosted`
declares — they arrive transitively, which upstream could tighten
([esp-hosted-mcu#251](https://github.com/espressif/esp-hosted-mcu/issues/251)) —
so `espos_wifi` guards the include and keeps the RPC as a fallback. **The
fallback fills the same four fields but not with the same meaning**, and a
build that takes it is worth knowing about:

| | TLV path (what espOS ships) | RPC fallback |
| --- | --- | --- |
| source | `EH_PRIV_FIRMWARE_VER` / chip-id TLVs from the handshake, already in host RAM | `esp_hosted_get_coprocessor_fwversion()` + `esp_hosted_get_cp_info()` |
| when | from the first co-processor heartbeat, once | retried up to 5× then every 5 min until one succeeds |
| `stale` | `esp_hosted`'s verdict, patch differences excluded | espOS comparing major.minor itself |
| if the co-processor will not answer | reports `0.0.0`, which is the fact | the whole `coprocessor` object stays **absent** |
| `target` | from the chip-id TLV | from a second RPC, so it can be missing while the rest is present |

Both are honest; the TLV path is the one that still answers when the
co-processor is the problem.

`hardware` answers "which board is this", which matters once there is more than
one on the bench. Most of it is read from the chip; `cpu_mhz` is what the build
asked for and `board` is what the firmware declared, so neither is a live
measurement:

| field | |
|---|---|
| `mac` | the base MAC — the identity `espos_net` derives the short id and default hostname from |
| `cpu_mhz` | what the build asked for (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`), not a live reading |
| `flash_bytes` | the flash chip's size |
| `coprocessor` | **only on a host whose radio is a separate chip**, and only from the first co-processor heartbeat onwards (so: within about 20 s of boot, or never if `CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT` is off). Absence does not distinguish those cases from a board with no co-processor at all. A co-processor whose firmware never announced a version reads `"0.0.0"` — present, answering, silent about itself. See below |
| `ram_internal_bytes`, `ram_psram_bytes` | **totals**, not free — `free_heap` above is the live number. `ram_psram_bytes: 0` means no PSRAM, which is what separates two boards with the same chip. |
| `features` | `wifi`, `ble`, `bt-classic`, `802.15.4`, `embedded-flash`, `embedded-psram`, from `esp_chip_info()`'s bitmask |
| `board` | **only if the firmware said so** (`espos_start_opts_t.board`); absent otherwise |

`board` cannot be discovered: ESP-IDF knows the chip, not what it is soldered
to, the MAC's OUI is Espressif's rather than the board vendor's, and the
`USER_DATA` efuse a vendor could burn an identifier into is blank on every
board seen here. The firmware is the only thing that knows, and it usually
already does — a Kconfig `choice` selecting the board has a prompt string that
is exactly this. Pass it to `espos_start()`:

```c
espos_start(&(espos_start_opts_t){
    .app_name = "cockpit",
    .board    = "Waveshare ESP32-P4-WIFI6-Touch-LCD-7B",
});
```

Deliberately absent: display size and touch (espOS has no display concept — a
firmware with a panel knows its own geometry), and radio *versions* like
"BLE 5.0" or "WiFi 6" — the feature bits say whether, not which, and a version
would be a hardcoded datasheet table espOS cannot verify.

The whole object is absent on a device built before it existed, so a client
should treat it and every member as optional.

`min_free_heap` and `min_internal_free` are low-water marks since boot; `free_heap` and
`free_internal` are the live figures beside them. The whole-heap pair asks
`MALLOC_CAP_DEFAULT` — what a plain `malloc()` may be given — and the internal pair
`MALLOC_CAP_INTERNAL`. Whether PSRAM is part of the whole-heap pair is a build
question, not a hardware one: it joins the `malloc()` heap only when
`CONFIG_SPIRAM_USE_MALLOC` says so, and `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` then
decides how much still comes from internal RAM.

Compare within a pair, never across them. ESP-IDF's capability bits are independent
flags rather than a hierarchy, and `heap_caps_get_minimum_free_size()` **sums each
matching region's own minimum**, so the two low-water marks add up different,
overlapping sets of regions, each at whatever moment that region happened to be at its
emptiest. Neither is reliably the larger.

`min_internal_free` is what the `memoryTrough` health condition is raised from
([health.md](health.md)), so it is the figure to read after seeing that warning: the
trough is over by the time anyone asks, and nothing live records that it happened.

Compare it with **`free_internal`**, not with `free_heap`. The two internal figures are
the same measurement — live and low-water — so a comfortable `free_internal` beside a
tiny `min_internal_free` is a board that dipped and recovered, while both low is a board
simply short of internal RAM. `free_heap` cannot stand in for that on a PSRAM build: it
is `MALLOC_CAP_DEFAULT`, so it counts PSRAM too, and megabytes free there say nothing
about whether internal RAM ever came back. `free_internal` and `min_internal_free` are
the same measurements `espos_health` compares, not similar ones.

`ui_storage` (M5) is true when the LittleFS UI partition is mounted.
`config_storage_reset` is true when the NVS partition had to be erased at
boot (corrupt/incompatible) and every value is a default.
`reset_reason` ∈ `poweron external software panic int_wdt task_wdt wdt
deepsleep brownout sdio usb jtag efuse power_glitch cpu_lockup unknown`.

`time` is the short form of `GET /time` below and is always present: `synced`,
`source` ∈ `none rtc sk manual sntp`, and `now` in unix milliseconds (`0` when
the device has not been told the time). A firmware built without `espos_time`
reports `{"synced": false, "source": "none", "now": 0}` — the honest answer,
so a client never has to guess whether the component is there.

`last_reset` is the record the health watchdog ([health.md](health.md)) left
when it restarted the device: `reason` (the reset reason, `software` for a
watchdog restart), `health_key` and `message` of the condition that struck
out, the low-water marks of total heap (`min_free_heap_before`) and internal
RAM (`min_internal_before`), the largest free internal block at the time
(`largest_block_before`), how long that boot had run (`uptime_before_s`) and
the wall-clock time (`at`, ISO 8601 UTC; `null` when the clock was never set).
`null` when the last reset was not the watchdog's — power-on, a panic (see
`/system/coredump`), an OTA reboot, `POST /system/reboot`. It stays for the
whole boot and is gone after the next reset, whatever its cause.

### `GET /system/coprocessor/update`, `POST /system/coprocessor/update` · protected

Only on a host whose radio is a separate chip (an ESP32-P4 with an ESP32-C6):
a build without one has no such route, so a 404 means there is nothing to
update. The POST installs the co-processor image this firmware was built for
over the existing link and restarts the device to apply it
([hardware.md](hardware.md#updating-the-c6-co-processor-firmware)):

```json
{}                                                           → 202 {"status":"updating"}
{"url": "http://signalk.local:3000/fw/espos-coprocessor-esp32c6-3.0.9-33154c51f397.bin"}
```

With no `url` the image comes from the URL this firmware was built with. Either
way the device accepts only the image whose SHA-256 it was built with
(`coproc_image.h`), checked before the first byte is written to the
co-processor; any other file ends in `failed`. Redirects are followed; the
server must send a `Content-Length`. 409 `busy` while a co-processor update
already runs, 409 `ota_busy` while a firmware update, check or rollback is in
progress (an update or rollback ends in a restart too), and 409 `unconfirmed`
while the running firmware is still pending verification after an OTA: the
restart that applies the co-processor image would roll it back. 400
`validation` for a body that is not a JSON object, or a `url` that is not a
string, not http(s), longer than 255 characters, or https on a build without a
certificate bundle.

The GET is the progress and the image on offer:

```json
{"state": "writing", "done": 524288, "total": 1427808,
 "image": {"version": "3.0.9", "sha256": "33154c51…", "url": "https://…"}}
```

`state` is `idle`, `downloading`, `verifying`, `writing`, `restarting` or
`failed` (with `error`). Compare `image.version` with
`hardware.coprocessor.version` in `GET /system/info` to decide whether to offer
the update. `restarting` is the last state a client sees: the co-processor
restarts itself into the new image and the device follows it, so the next
answer comes from a fresh boot. A co-processor that takes the image but will
not activate it reads `failed` instead and the device still restarts, since
it cannot tell whether the co-processor switched anyway;
`hardware.coprocessor.version` after the restart says which image runs.

### `POST /system/reboot` — M1 · protected

`202 {"status": "rebooting"}` — the device restarts ~500 ms after
responding.

### `GET /system/coredump` — M5 · protected

Summary of the core dump saved by the last panic, `404 not_found` when
there is none:

```json
{"present": true, "size": 18848, "valid": true, "task": "httpd", "pc": "0x4003530c",
 "app_elf_sha256": "adef2c527", "version": 1179908,
 "mcause": 7, "mtval": "0x00000010", "ra": "0x40035308", "sp": "0x4ff45580", "stackdump_bytes": 448}
```
RISC-V targets carry `mcause/mtval/ra/sp/stackdump_bytes`; Xtensa targets
`exc_cause`, `exc_vaddr`, `backtrace` (array of PCs) and
`backtrace_corrupted`. `app_elf_sha256` identifies the build that crashed
(compare with `idf.py` output / `esp_app_desc`).

`GET /system/coredump/raw` streams the image as `application/octet-stream`
for `espcoredump.py --chip <target> info_corefile -c coredump.bin -t raw
build/espos.elf` (needs the ELF of exactly that build). `DELETE
/system/coredump` erases it (`{"status": "erased"}`).

`POST /system/crash` (only with `CONFIG_ESPOS_HTTPD_DEBUG_CRASH=y`, off by
default) panics on purpose to test the path.

### `POST /system/factory-reset` — M1 · protected

Arms the reboot (further writes get `503`), erases the whole NVS partition,
responds `202 {"status": "factory_reset", "rebooting": true}`, then reboots.
WiFi credentials and the SignalK token live in NVS too, so this returns the
device to provisioning.

## Logs — M5

### `GET /logs` · protected

The in-RAM log ring (`CONFIG_ESPOS_LOG_RING_SIZE`, 16 KiB), paged by
sequence number: `?after=<seq>` returns lines with a higher sequence
(default: from the oldest kept), `?limit=<n>` (default 200, max 1000).

```json
{"first": 1, "next": 101, "dropped": 0, "size": 16384, "used": 5911, "gap": false, "from": 1,
 "lines": ["I (500) espos_config: ready: 4 namespace(s), schema 8851b3f24a88fe45", "…"]}
```
`lines[i]` has sequence `from + i`; poll on with `after = next - 1`.
`gap` is true when `after` was older than the ring still holds (lines were
overwritten in between); `first`/`dropped` say how many. Lines are the
console lines without colour codes, truncated at
`CONFIG_ESPOS_LOG_LINE_MAX` (256). Streamed in chunks, so a full ring never
has to fit in RAM twice.

### `PUT /logs/level` · protected

`{"level": "debug", "tag": "espos_sk"}` (`tag` optional, default `*`;
`level` ∈ `none error warn info debug verbose`) → `200 {"tag","level"}`;
`400 validation` otherwise. Runtime only (`esp_log_level_set`), not
persisted.

## Static UI — M5 — public

Everything that is not `/api/…` is served from the LittleFS `storage`
partition (the gzipped Vite bundle, see [ui.md](ui.md)): `<path>.gz` is
preferred and sent with `Content-Encoding: gzip` (`curl --compressed`),
`/assets/*` (content-hashed) with `Cache-Control: public, max-age=31536000,
immutable`, everything else `no-cache`. An extension-less path that does
not exist falls back to `/index.html` (SPA routes such as `/wifi`); a
missing file with an extension is `404 {"error": "not_found"}`, as is
anything unknown under `/api/`. When the partition has no `index.html` the
placeholder page embedded in the firmware is served instead
(`ui_storage: false` in `/system/info`).


## Network

### `GET /net/status`

The transport-neutral view of the network ([net.md](net.md)): whether a
default route exists, on which interface, with which addresses, and the
device's identity on it.

```json
{"up": true, "iface": "wifi_sta", "ip": "192.168.1.23", "netmask": "255.255.255.0", "gateway": "192.168.1.1",
 "ip6_ll": "fe80::3e71:bfff:fe12:1a2b", "mac": "3c:71:bf:12:1a:2b", "hostname": "espos-1a2b", "id": "1a2b",
 "rssi": -59, "up_count": 1, "up_s": 26}
```

`iface` ∈ `none wifi_sta eth thread` (`none` while down, with the address
strings empty); `mac` is the base MAC and `id` its last two bytes, the
device id every default name derives from; `rssi` is `null` unless the
route is the WiFi station; `up_count` counts how often the route came up or
moved since boot; `up_s` is seconds since it came up (0 when down); `ip6_ll`
is `""` when the interface has no link-local IPv6 address. `/wifi/status`
below is unchanged and keeps the WiFi-specific detail.

## Health

### `GET /health` — protected

What the device currently thinks is wrong.

```json
{
  "worst": "alarm",
  "fatal": "lowMemory",
  "conditions": [
    {"key": "lowMemory", "state": "alarm", "message": "internal RAM below 12 KB", "reboot_on_alarm": true},
    {"key": "memoryTrough", "state": "warn", "message": "internal RAM low-water mark below 10 KB since boot", "reboot_on_alarm": false},
    {"key": "netDown", "state": "normal", "message": "", "reboot_on_alarm": false}
  ]
}
```

`worst` is the worst state of any condition — what a single status LED wants.
`fatal` names the condition the policy would restart for, or `null`; it is the one
thing a reader cannot derive from the list, because it depends on the flags a
condition was raised with rather than on its state. `reboot_on_alarm` reports that
flag per condition, rather than the raw `ESPOS_HEALTH_F_*` bitmask a client would
then have to keep in step with the firmware.

Conditions are listed oldest first, including those currently `normal`: a key keeps
its slot for the life of the boot once raised, so the list is the history of what
*has* gone wrong as much as what is wrong now. `truncated` appears with a count if
there are more conditions than the response can carry (32).

### `POST /health/test` — protected

Raise a synthetic condition, so a hardware test can exercise the buzzer, LED and
notification path a real fault would reach — on a boxed-up board, without causing
the fault ([espOS #137](https://github.com/signalk-espOS/espOS/issues/137)).

```json
{"key": "test.buzzer", "state": "alarm", "message": "G4 drill", "ttl_s": 45}
```

Returns the same body as `GET /health`. Clear it with `{"key": "...", "state": "normal"}`.

| field | |
|---|---|
| `key` | required; **must start with `test.`**, name something after it, under 24 bytes |
| `state` | required; `normal` clears, `warn` or `alarm` raises |
| `message` | optional string, under 96 bytes |
| `ttl_s` | optional, 1–300 (fractions allowed), default **45**; a backstop, not the normal way to clear |

A field that is present but of the wrong type is a `400`, not silently ignored, and
so is an over-long `key` or `message` — the firmware refuses those rather than
truncating them.

The ttl is cleared by the policy tick, so a drill ends within one
`CONFIG_ESPOS_HEALTH_POLICY_TICK_S` (default 10 s) of falling due rather than at the
second: expect `ttl_s: 45` to clear somewhere in 45–55 s. `GET /health` drives expiry
too, so a script polling it sees the clear sooner. To end a drill at a known moment,
clear it explicitly.

Three properties make this safe enough to ship enabled rather than hide behind a
build flag, since the API key is the real gate either way:

* **It cannot reboot the device.** Every built-in alarm carries
  `ESPOS_HEALTH_F_REBOOT_ON_ALARM`, so triggering one to watch a buzzer ends the
  observation. A synthetic condition is always reported with no flags, so it is
  structurally incapable of arming that path — note `fatal` stays `null` while
  `worst` reads `alarm`.
* **It cannot impersonate a real condition.** The reserved `test.` prefix means a
  drill can neither be mistaken for a genuine fault nor clobber a consumer's own
  condition key.
* **At most one drill is active at a time.** Raising a second clears the first, so a
  script that loops cannot leave a trail of fake alarms on a live boat.

The default `ttl_s` is 45 rather than 30 because a *repeating* alarm has to be
watched for several cycles to be believed: an espOS consumer measured its own
morse-code buzzer loop at 10–11 s per pass, so 30 s catches two cycles and not
comfortably three. Out-of-range values are rejected rather than clamped — silently
substituting a different timeout is how a test ends up proving the wrong thing.

A drill is a real report, so it fans out to every sink, **including the SignalK
notification path** — expect it on a chartplotter, and clear it when done.

`507 Insufficient Storage` means the condition table is full
(`CONFIG_ESPOS_HEALTH_MAX_CONDITIONS`, default 12). Reuse a key you have already
used this boot rather than inventing one per run: a key keeps its slot.

## Time

### `GET /time` — protected

What the device believes the time is and where it learned it ([time.md](time.md)).

```json
{"synced": true, "source": "sntp", "now": 1788775933456,
 "iso": "2026-09-07T10:12:13.456Z", "tz": "UTC0",
 "sntp": {"enabled": true, "running": true, "from_dhcp": true,
          "servers": ["pool.ntp.org"]}}
```

`source` ∈ `none rtc sk manual sntp`, ranked in that order: a lower-ranked
source never overrides a higher one, so nothing walks the clock back once NTP
has answered. `now` is unix milliseconds and `iso` the same instant as ISO 8601
UTC; both are `0` and `""` when nothing has set the clock — never a
plausible-looking 1970, so a missing time reads as missing.

`synced` is false either when no source has spoken or when the only one that
did was an RTC value carried through a deep sleep that has since gone stale
(`CONFIG_ESPOS_TIME_RTC_STALE_H`); in the stale case `now` is still the
device's best guess and `source` still says `rtc`.

`sntp.running` is true once polling started, which happens on the first
`NETWORK_UP` rather than at boot. `sntp.servers` lists the configured ones; a
server supplied by DHCP is not among them.

### `PUT /time` — protected

```json
{"now": 1788775933456}
```

Sets the clock as `source: "manual"`, and replies with the `GET /time`
document. `tz` may be sent instead of or alongside `now` to set the POSIX
timezone (`time.tz`) — a display concern only; nothing espOS publishes is ever
in local time.

`409 outranked` when a higher-ranked source already has the clock — in
practice, when SNTP has synced. `400 validation` when `now` is not a positive
unix-millisecond value or `tz` is too long.

## WiFi

### `GET /wifi/status` — M2 · protected

The status document described in [wifi.md](wifi.md): `state` ∈
`disabled unconfigured connecting obtaining_ip connected backoff`,
`reason: {code, text}`, link/IP details, `backoff_ms` while backing off,
counters, `portal: {active, ssid, force, ip?, clients?}`.

`portal.force` ∈ `auto up down` is what `espos_wifi_portal_open()` last asked
for, so `active: false` with `force: "down"` is an application holding the
access point shut rather than the policy declining to raise it — two states
that otherwise look identical and need different answers
([wifi.md](wifi.md#portal-softap-provisioning)). `auto` is the default and
means the policy decides.

### `POST /wifi/scan` — M2 · protected

Starts an asynchronous scan. `202 {"status": "scanning"}`; `409 busy` if
the driver cannot scan right now. Requires the JSON content type.

### `GET /wifi/scan` — M2 · protected

```json
{"scanning": false, "age_s": 3,
 "results": [{"ssid": "Boat", "bssid": "aa:bb:cc:dd:ee:ff", "rssi": -52, "channel": 6, "auth": "wpa2/wpa3"}]}
```
`age_s` is `null` before the first scan. `auth` ∈ `open wep wpa wpa2
wpa/wpa2 wpa2-enterprise wpa3 wpa2/wpa3 wapi owe other`. Results are cached;
a `wifi_scan` SSE event carries the same document when a scan finishes.

### Captive-portal probes — M2 — public on the portal network

`/generate_204`, `/gen_204`, `/hotspot-detect.html`,
`/library/test/success.html`, `/connecttest.txt`, `/ncsi.txt`, `/redirect`,
`/canonical.html`, `/success.txt` answer `302 → http://192.168.4.1/`. They
only matter to a phone that just joined the portal, and that network is
exempt from authentication; on the station side they are ordinary protected
endpoints.

## BLE provisioning

### `GET /prov` — protected

Present only when the build has `espos_prov` ([provisioning.md](provisioning.md)).

```json
{"active": true, "got_credentials": false,
 "service_name": "ESPOS_ca6a", "pop": "7K4M9QRT2WXY",
 "scheme": "espos-ble-prov-1"}
```

`pop` is the proof of possession a phone must present, served here because a
device-generated one is useless if nobody can read it. Reachable only over
the network, which a device being provisioned does not have yet — it is for
a device already on WiFi advertising for re-provisioning, and for the setup
portal, which serves it over its own access point.

## Events

### `GET /events` — M2 · protected

`text/event-stream` (chunked, `retry: 3000` first, a `: ping` comment every
15 s). Protected like the rest: a browser's `EventSource` sends the login
cookie by itself, a script sends the Bearer header. Events:

| event       | data                                | when                                   |
|-------------|-------------------------------------|----------------------------------------|
| `net`       | the `/net/status` document           | on connect (snapshot) and every change: route up/down/moved, RSSI refresh |
| `wifi`      | the `/wifi/status` document          | on connect (snapshot) and every change |
| `wifi_scan` | the `/wifi/scan` document            | scan finished                          |
| `config`    | `{"ns": "...", "key": "..."}`         | a key's effective value changed        |

| `sk`        | the `/sk/status` document            | on connect and every token/server change |
| `sk_servers`| the `/sk/servers` document           | on connect and after each discovery pass |
| `sk_ws`     | the `ws` object of `/sk/status`      | stream connect/disconnect, error, drops |
| `sk_tls`    | the `/sk/tls` document               | on connect and whenever the pinned certificate changes |
| `logs`      | `{"next": <seq>}`                     | at most every 500 ms when new log lines arrived; fetch `/logs?after=` |
| `ota`       | the `/ota/status` document           | on connect, state changes, every ~32 KiB of download |
| `ble`       | the `/ble/status` document           | on connect (snapshot)                  |

At most `CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS` (3) streams; when full the
oldest stream is evicted (clients reconnect via `retry`).

The "on connect" snapshots in the table above are one registered callback per
publishing component, capped by `CONFIG_ESPOS_HTTPD_SSE_MAX_CONNECT_CBS` (8).
A firmware that adds its own publishers registers after espOS's, so it is the
consumer's events that go missing if the cap is too low -- and the symptom is
quiet, because the endpoint still answers and later changes are still
published; only the snapshot a fresh client gets is absent. Registering past
the cap is refused with `ESP_ERR_NO_MEM` and logged by `espos_sse`.

## SignalK

### `GET /sk/status` — M3 · protected

```json
{
  "token": {"state": "approved", "has_token": true, "busy": false,
            "approved_s": 120, "next_action_s": 40, "last_check_s": 20,
            "last_http_status": 200, "last_error": "",
            "counts": {"requests": 1, "approved": 1, "denied": 0, "unauthorized": 0,
                       "cert_errors": 0}},
  "server": {"host": "192.168.1.10", "port": 80, "self": "urn:mrn:signalk:uuid:…",
             "source": "discovered", "scheme": "http", "by_self": false, "name": "boat",
             "swname": "signalk-server", "swvers": "2.31.1"},
  "client_id": "…uuid…", "description": "espOS espos-1a2b", "permissions": "readwrite",
  "discovery": {"enabled": true, "count": 2, "last_s": 12},
  "ws": {"enabled": true, "connected": true, "connected_s": 300, "reconnects": 1,
         "sent": 1234, "send_errors": 0, "pending": 0,
         "buffered": 0, "buffered_bytes": 0, "dropped": 0, "last_error": "",
         "meta": {"declared": 8, "reconciled": 8},
         "in": {"subs": 2, "frames": 590, "received": 586},
         "put": {"pending": 0, "ok": 3, "failed": 1}}
}
```
`token.state` ∈ `no_server requesting pending verifying approved denied
open error cert_error`; `pending_href`/`pending_s` while pending;
`server.source` ∈ `discovered manual pinned none`; `server.scheme` ∈ `http
https` — what is actually in use, which under `sk.scheme = auto` is the only
place to read it. `server.by_self` is true while the server stands in for the
one the token or pending request belongs to, found by its self URN while that
address is silent; refusals from it do not cost the token (signalk.md). The token itself is never returned.

`cert_error` means the server's TLS certificate is not the one this device
trusts. The token is kept (it is the transport that is wrong, not the
credential), the retry is a flat 60 s rather than the exponential backoff an
unreachable server gets, and the stream stays down. `GET /sk/tls` says what
differed; `DELETE /sk/tls` accepts the new certificate. See
[SignalK → TLS](signalk.md#tls-https-wss).

`ws` (M4) is the delta stream: `pending` = values in the open batching
window, `buffered`/`buffered_bytes` = messages held in the offline ring,
`dropped` = messages the ring had to discard (oldest first),
`next_retry_s` while disconnected, `meta` = declared / reconciled counts.
`in` (M7): active subscriptions, text frames read, value/meta items
delivered; `put` (M7): requests in flight, answered OK, failed/timed out.

### `GET /sk/servers` — M3 · protected

`{"servers": [{"host","port","self","name","roles","swname","swvers","scheme","seen_s","selected"}], "last_s": 12}`

`scheme` is `https` for a server that advertised itself as
`_signalk-https._tcp` rather than `_signalk-http._tcp`, which is how
signalk-server says its `ssl` setting is on.

### `GET /sk/tls` — S1 · protected

Present only in a build with `CONFIG_ESPOS_SK_TLS` (the default).

```json
{
  "trust": "tofu",
  "pinned": {"kind": "leaf", "cn": "boat.local", "san": "192.168.1.10,boat.local",
             "fingerprint": "9f2c…", "since": 1789000000},
  "last_error": "",
  "presented": {"cn": "boat.local", "fingerprint": "9f2c…"}
}
```

`trust` ∈ `tofu ca bundle` (the `sk.tls_trust` setting). `pinned` is `null`
until something is anchored; `kind` ∈ `ca leaf` — `ca` binds the issuing CA
*and* the `san` set, so a renewal by the same CA for the same names is
accepted, while `leaf` is the certificate itself and a renewal needs a
`DELETE`. `fingerprint` is the SHA-256 of whichever certificate is anchored,
hex. `since` is unix seconds, `0` when the device had no clock at the time.
`presented` is what the last handshake showed (`null` before the first), and
`last_error` says how it differed — the pair is what makes a `cert_error`
actionable.

### `DELETE /sk/tls` — S1 · protected

Forget the anchor and retry at once: the deliberate "yes, that certificate
really was replaced" a pinned device needs after a renewal it could not follow
on its own. `202 {"status":"reset"}`.

### `PUT /sk/tls/ca` — S1 · protected

Body `{"pem": "-----BEGIN CERTIFICATE-----…"}` → `200
{"status":"stored","trust":"ca"}`. Validates the PEM before storing it (`400
validation` for anything that is not a certificate, or one over
`CONFIG_ESPOS_SK_TLS_CA_MAX`), writes it to `sk.ca_pem` and switches
`sk.tls_trust` to `ca`. Equivalent to setting both config keys, with the
parse check.

### `POST /sk/discover`, `POST /sk/request`, `POST /sk/forget` — M3 · protected

`202` with a status word; JSON content type required. `request` re-requests
access (from `denied`/`error`/`open`); `forget` drops the token (a pending
request keeps being polled).

### `POST /sk/token` — M3 · protected

Body `{"token": "<jwt>"}` → `202 {"status": "verifying"}`; the token is
verified against `/signalk/v1/api/self` and kept if it works. `400
validation` for a bad body.

### `POST /sk/publish` — M4 · protected

Publish a value for a `vessels.self` path — the C publish API over HTTP,
for scripts and the setup page:

```json
{"path": "espos.demo.count", "value": 42,
 "meta": {"units": "1", "description": "demo"}, "period_ms": 1000}
```
`meta`/`period_ms` are optional and go through `espos_sk_declare_meta()`
(same rules: non-standard paths only, `period_ms` adds `timeout`). `202
{"status":"queued"}`; `400 validation` for a bad body or an over-long
path/value; `503 not_ready` before the SignalK component is up. Queued
does not mean delivered: the value goes into the current batching window
and, if the stream is down, into the offline ring (see `ws` in
`/sk/status`).

### `POST /sk/put`, `GET /sk/put` — M7 · protected

`{"path": "navigation.anchor.maxRadius", "value": 30}` → `202
{"status":"sent"}` (`503 not_connected` without a stream, `429 busy` with
8 in flight, `400 validation`). The server answers asynchronously; `GET
/sk/put` returns the last answer `{"request_id","state","status_code",
"message"}` or `null`.

SSE events: `sk` (the status document, on connect and on change),
`sk_servers` (the servers document after each discovery pass), `sk_ws`
(the `ws` object on every stream state change), `sk_tls` (the `/sk/tls`
document when the pinned certificate changes).

## OTA — M6

### `GET /ota/status` · protected

```json
{"state": "idle", "last_error": "",
 "running": {"version": "0.6.1", "project": "espos", "target": "esp32p4", "slot": "ota_1",
             "image_state": "valid", "pending_verify": false, "confirmed": true,
             "other_slot": "ota_0", "other_version": "0.6.0", "rolled_back": false,
             "built": "Aug 18 2026 14:20:25", "idf": "v6.0.2", "key_fp": "b3381b48b9cc9941"},
 "manifest": {"url": "http://…/manifest.json", "channel": "stable", "auto_check": true,
              "auto_install": false, "last_check_s": 120, "next_check_s": 86280},
 "progress": {"received": 0, "total": 0},
 "available": {"version": "0.6.2", "url": "http://…/espos-esp32p4-0.6.2.bin", "size": 0,
               "sha256": "", "notes": "…", "newer": true}}
```
`manifest.url` is what the last check actually fetched, which is not always
what is configured: with `ota.manifest_src = "signalk"` the URL is derived
from the connected server, and reporting the (empty) configured value would
tell an operator the device looks nowhere. Before the first check it falls
back to the configured `manifest_url`.

`state` ∈ `idle checking available downloading verifying ready failed`
(`ready` = installed, rebooting in ~1.5 s). `image_state` ∈ `valid
pending_verify new invalid aborted undefined`; `pending_verify` is true
while a fresh image has not confirmed itself; `rolled_back` when the other
slot holds an image that failed. `key_fp` names the key the running image
is signed with, which is the key it accepts updates from: the first 16 hex
characters of the Secure Boot V2 public-key digest of its first signature
block (what `espsecure signature-info-v2` prints), `null` when the image
carries no signature block or the OTA task has not read it yet. It
predicts acceptance for espOS's signed-apps-without-hardware-Secure-Boot
setup, where the running image's first signature block is the trusted key;
with hardware Secure Boot the eFuse digests decide instead
([ota.md](ota.md#signing-key)). `available` is `null` until a manifest
check found something. `last_check_s`/`next_check_s` are `null` before the
first check / when auto-check is off.

### `POST /ota/check`, `POST /ota`, `POST /ota/confirm`, `POST /ota/rollback` · protected

JSON content type required; `202 {"status": …}`. `POST /ota` with
`{"url": "http(s)://…"}` installs that image, with `{}` the *available*
build (`404 not_found` if none). `409 busy` while a check or install is
running; `409 coprocessor_busy` for an install or rollback while a radio
co-processor update runs (`/system/coprocessor/update`), so a client that
adopts a running install on `busy` can tell that no install is coming;
`400 validation` for a non-http(s) URL. Progress and the outcome
arrive via `GET /ota/status` and the `ota` SSE event.

## BLE gateway

Present only when `espos_ble` is in the build and Bluetooth is enabled in
sdkconfig. The component bridges BLE devices to signalk-server's BLE provider
API; it decodes nothing itself, so there is no device or sensor model here —
what a device *is* remains a server-side concern.

### `GET /ble/status` · protected

```json
{
  "enabled": true, "scanning": true, "mac": "D8:85:AC:FA:2C:46",
  "scan_hits": 2766, "adv_received": 2766, "adv_posted": 1840,
  "adv_dropped": 0, "adv_pending": 126,
  "post_success": 46, "post_fail": 0,
  "ws_connected": true,
  "gatt_sessions": 0, "gatt_max": 3
}
```

* `scan_hits` counts advertisements seen by the radio, `adv_received` those
  handed to the gateway; a gap between them means the intake callback is being
  starved.
* `adv_pending` is the ring buffer's depth and `adv_dropped` the running total
  shed once it is full (oldest first). Sustained growth in `adv_dropped` means
  the POST interval or the buffer is too small for the local radio traffic —
  it is a real count, not an estimate.
* `post_fail` counts POSTs the server refused or that never completed; a 401
  or 403 is reported to `espos_sk`, which owns the token, rather than
  triggering a second access request from here.
* `gatt_max` is the concurrent-session ceiling (3, Bluedroid's own limit) and
  is what the gateway advertises to the server in its `hello`.

There is no `POST /ble/...`: scanning is driven by config
(`PUT /config` with the `ble` namespace) and GATT sessions are opened by the
server over the control WebSocket, never through this API.

## Data flow

Present only when the firmware builds `espos_flow` and calls
`espos::flow::api_register(&graph)` (or `nullptr` for the counters alone).

### `GET /flow` · protected

```json
{
  "running": true,
  "loop": { "posts": 1842, "dropped": 0, "timers_fired": 3680,
            "timers_live": 2, "queue_peak": 3,
            "edges_used": 4, "edges_max": 96 },
  "nodes": [ { "id": "light" }, { "id": "cal", "title": "Calibration" },
             { "id": "sk" } ]
}
```

Replaces SensESP's status page, which was assembled from `StatusPageItem`
objects a firmware registered by hand: the graph already knows its own shape,
so a node added to it appears here without being told to.

`nodes` is in adoption order — the order `make<T>()` was called, which is the
order the firmware's own source reads in. It is `null`, not `[]`, when no
graph was registered: a firmware may drive the loop from C alone, and an
empty array would mean a graph that adopted nothing, which is a wiring bug.

The `loop` half is what matters in service and neither value raises an alarm
on its own:

* **`dropped`** climbing means something posts faster than the loop consumes.
  The value is lost, not queued.
* **`edges_used`** approaching `edges_max` means the next `connect_to()` will
  fail. The pool is static (`CONFIG_ESPOS_FLOW_MAX_EDGES`).
* **`queue_peak`** is the high-water mark, so it answers "how close did this
  ever come" rather than "how deep is it now".

## NMEA 2000

Present only when the build has `espos_n2k` and the firmware handed it the
receiver with `espos_n2k_api_register(&receiver)` ([n2k.md](n2k.md)).

### `GET /n2k` · protected

```json
{"present": true, "running": true, "ever_received": true, "idle_s": 0,
 "frames": 89559, "dropped": 0, "errors": 59, "bus_off": 0,
 "last_error": {"flags": 8, "arb_lost": false, "bit_err": false, "form_err": false,
                "stuff_err": true, "ack_err": false}}
```

`present: false`, and nothing else, means the component is in the build but
the application never registered a receiver. `idle_s` is seconds since the
last frame and `null` until one has arrived. `last_error` is the TWAI error
word of the most recent bus error, raw in `flags` and decoded alongside.
The flags are symptoms, not diagnoses: `ack_err` alone (no node acknowledged
the frame) is the usual sign that nothing else is listening, and repeated
`stuff_err` or `form_err` point at a bitrate or wiring mismatch, but one
flag is not proof of either. This is the route to read on a silent bus;
[n2k.md](n2k.md) has the table of what the counters mean.

## Power

Present only when the build has `espos_power` and `espos_power_start()` has
registered the route ([power.md](power.md)); the registered route answers
`503 not_started` if the component is not running, which only happens when
start failed after registering it.

### `GET /power` · protected

```json
{"mode": "cycle", "decision": "stay", "why": "stream", "timer_wake": true, "wake_count": 12,
 "last_awake_ms": 4210, "interval_s": 300, "uptime_ms": 2600, "deadline_ms": 30000, "holds": 0}
```

`mode` ∈ `off cycle`; `decision` ∈ `sleep stay` is what the policy decided
last and `why` ∈ `off unconfirmed ota window network stream publishing hold
done deadline` names the rule that won. `timer_wake` says whether this boot
was the cycle's timer wake; `wake_count` and `last_awake_ms` come from RTC
memory and survive deep sleep; `deadline_ms` is this wake's time budget and
`holds` the application's open `espos_power_hold()` count.

## Planned (shape reserved, not implemented)

Nothing — M1–M7 and the authentication are implemented. Future additions go
here first.
