# Network (`espos_net`)

The one place espOS answers "is the network up, and how". Every transport —
the WiFi station and wired Ethernet today, 802.15.4 next — reports its link into
`espos_net`; SignalK, OTA, mDNS and the health policy ask `espos_net`, never
a radio. Configuration is the `net` namespace (`hostname`); status is
`GET /api/v1/net/status` and the `net` SSE event.

## Why a seam

Until 0.7 `espos_sk`, `espos_ota` and the mDNS responder called
`espos_wifi_get_status()` to learn whether they had a network, took the
device id from the station's MAC and the hostname from `wifi.hostname`. That
tied three components that know nothing about radios to one particular
radio: an Ethernet gateway, a firmware on the 802.15.4-only H-series, or a
headless build without WiFi would have had to carry `espos_wifi` along just
to satisfy those calls. The seam is small — link up or down, the addresses,
an id, a name — so it became one component that every transport feeds and
everything else consumes.

## The contract (`espos_net.h`)

```c
typedef struct {
    bool up;  espos_net_if_t iface;                     /* the interface carrying the default route */
    char ip[16], netmask[16], gateway[16], ip6_ll[40];
    uint8_t mac[6]; char hostname[33]; int8_t rssi;     /* rssi 0 unless the route is the WiFi station */
    uint32_t up_count, up_since_ms;                     /* how often the route came up; ms it has been up (0 when down) */
} espos_net_status_t;

esp_err_t   espos_net_start(void);                      /* after espos_httpd_start(); owns hostname + mDNS */
esp_err_t   espos_net_get_status(espos_net_status_t *out);   /* never calls a driver */
bool        espos_net_is_up(void);
esp_err_t   espos_net_subscribe(espos_net_cb_t cb, void *arg);   /* up / down / route moved */
const char *espos_net_short_id(void);                   /* "1a2b": last two bytes of the base MAC */
uint32_t    espos_net_backoff_ms(uint32_t round, uint32_t cap_ms, uint32_t rnd);
/* transports report in: */
esp_err_t   espos_net_register_if(espos_net_if_t iface, void *esp_netif);
void        espos_net_report(espos_net_if_t iface, bool up, const char *ip, const char *netmask, const char *gateway, int8_t rssi);
```

`espos_net_if_t` is `ESPOS_NET_IF_WIFI_STA`, `ESPOS_NET_IF_ETH` or
`ESPOS_NET_IF_THREAD` (`ESPOS_NET_IF_NONE` when down). `mac` is the base MAC
from eFuse; `ip6_ll` the link-local IPv6 address of the interface carrying
the route, empty when there is none or the build has no IPv6.

`espos_net_get_status()` and `espos_net_is_up()` **never call a driver**:
a status snapshot must not depend on a radio answering. On the ESP32-P4 the
radio is a co-processor behind an RPC that stalls when the link wedges, and
a status label on a display task once froze the whole UI that way
([wifi.md](wifi.md), "Co-processor link watchdog"). The netif the addresses
come from is host-side lwIP on every target, the P4 included.

## Default-route rule

One interface carries the default route at a time. When several are up the
preference is static — **Ethernet over WiFi station over Thread** — with no
metric and no probing, so a consumer can predict the answer from this
sentence. A wired link is the one somebody ran a cable for; the station is
the fallback; a Thread border route is last because it is the slowest and
the least likely to reach a SignalK server directly.

Each `espos_net_report()` produces one of three edges, or none:

| Report | Edge | What consumers see |
|---|---|---|
| first interface up | `UP` | `ESPOS_EVENT_NETWORK_UP`, subscribers, `net` SSE |
| last interface down | `DOWN` | `ESPOS_EVENT_NETWORK_DOWN`, subscribers, `net` SSE |
| a preferred interface comes up, the preferred one goes away, or the route's own address changes | `CHANGED` | `NETWORK_DOWN` then `NETWORK_UP` — sockets bound to the old address are dead either way, so the "one DOWN per UP" rule holds and the SignalK stream re-dials |
| same data again, an RSSI refresh, a change on a standby interface | none | the `net` SSE event when anything in the status changed; no event, no callback |

`up_count` counts `UP` and `CHANGED` edges since boot; `up_since_ms` is the
time the current route has been up. The machine is pure C over an injected
clock (`espos_net_sm.h`, `src/net_sm.c`) and every row above is a case in
`test/host/espos_net_test`.

## Events, callbacks, threading

| Signal | When | Runs on |
|---|---|---|
| `ESPOS_EVENT_NETWORK_UP` / `_DOWN` | route edges, as in the table above; `NETWORK_UP` carries `espos_event_network_t {ip, hostname}` | the default event loop task |
| `espos_net_subscribe()` callback | every edge (not RSSI refreshes), with a copy of the status | the task of the transport that reported — for WiFi the `espos_wifi` task, whose queued driver calls wait while it runs; no `espos_net` lock held; copy out and return, never block |
| `net` SSE event | every status change, RSSI refreshes included; a snapshot on connect | the reporting task, through `espos_httpd_sse_publish()` |
| `ESPOS_EVENT_MDNS_READY` | every `NETWORK_UP` once the responder runs | the default event loop task |

Events are notifications, not state: a late subscriber asks
`espos_net_get_status()` for the picture and uses the events for changes
from then on ([concepts.md](concepts.md), "Events").

## Hostname and device id

`net.hostname` (string, ≤ 32 characters, `[A-Za-z0-9-]`, restart required)
is the name the device answers to as `<hostname>.local`, sends in its DHCP
request, uses as the SignalK source label and shows in the server's
access-request list. Empty — the default — means `espos-<id>`.

The id is `espos_net_short_id()`: the last two bytes of the **base MAC**
read from eFuse (`esp_read_mac(ESP_MAC_BASE)`), so it is the same whether
the device speaks WiFi, Ethernet or Thread. Until 0.7 the WiFi station's MAC
was used, which on a native radio is the same number. On the ESP32-P4 it is
not: the station MAC belongs to the C6 co-processor, the base MAC to the P4
itself, so a P4 device without a configured hostname changes its default
name once on the upgrade (hostname, portal SSID, SignalK source label — the
server shows a new source). Set `net.hostname` to keep a name across it.

**Moved from `wifi.hostname`.** The key lived in the `wifi` namespace until
0.7. `espos_net_start()` moves a value found there into `net.hostname` once
(only if `net.hostname` is unset) and clears the old key; a value written to
`wifi.hostname` afterwards — by an old script — is moved the same way at the
next boot. The `wifi.hostname` descriptor stays for one release so the
store still knows the key; it is titled "moved" in the UI and reads back
empty once the device has booted on this version. Removed in 0.9.

## `GET /api/v1/net/status`

```json
{"up": true, "iface": "wifi_sta", "ip": "192.168.1.23", "netmask": "255.255.255.0", "gateway": "192.168.1.1",
 "ip6_ll": "fe80::3e71:bfff:fe12:1a2b", "mac": "3c:71:bf:12:1a:2b", "hostname": "espos-1a2b", "id": "1a2b",
 "rssi": -59, "up_count": 1, "up_s": 26}
```

`iface` ∈ `none wifi_sta eth thread`; `rssi` is `null` unless the route is
the WiFi station; `up_s` is seconds since the route came up (0 when down);
`ip6_ll` is `""` when there is none. The same document is pushed as the
`net` SSE event on connect and on every change. `GET /api/v1/wifi/status`
is unchanged and stays the place for WiFi-specific detail (state machine,
SSID, portal, reason codes).

## How a transport plugs in

`espos_wifi` is the template; an Ethernet driver does the same four things:

1. **Start after `espos_net_start()`.** It owns the hostname the DHCP request
   carries, so the order is enforced: `espos_wifi_start()` refuses to run
   before it (`ESP_ERR_INVALID_STATE`, one log line). `espos_start()` does
   `httpd → net → [wifi] → [sk] → [ota] → [ble]`.
2. **Register the netif** once it exists:
   `espos_net_register_if(ESPOS_NET_IF_ETH, netif)`. `espos_net` sets the
   hostname on it and reads its link-local address for the status.
3. **Report every change:** `espos_net_report(ESPOS_NET_IF_ETH, true, ip,
   netmask, gateway, 0)` on `IP_EVENT_ETH_GOT_IP`, `espos_net_report(..., false,
   NULL, NULL, NULL, 0)` on link down or lost IP. Identical reports are
   no-ops, so reporting from every status change is fine. Report from your
   own task — `espos_net_report()` runs the subscribers on it before it
   returns — and never under a lock a subscriber might want.
4. **Post no `NETWORK_UP`/`NETWORK_DOWN` yourself.** `espos_net` decides
   whether the *default route* changed; a second link coming up while the
   first carries the route is not a network event to anyone above the seam.

The station reports from `espos_wifi`'s own task, outside its state-machine
lock and after the machine has taken the link up, so both
`espos_wifi_get_status()` and `espos_net_get_status()` already say "up" when
a `NETWORK_UP` handler runs. Static addressing is a WiFi setting
(`wifi.ip_mode` and friends, [wifi.md](wifi.md), "Addressing"): the netif
still raises `GOT_IP` with a static address, so `espos_net` sees no
difference.

## Wired Ethernet (`espos_eth`)

The second transport, and the checklist above applied: the chip's internal
EMAC and an RMII PHY with DHCP, the netif registered as `ESPOS_NET_IF_ETH`,
`IP_EVENT_ETH_GOT_IP` reported up and link loss or `IP_EVENT_ETH_LOST_IP`
reported down. It starts beside the WiFi station, not instead of it, and the
static preference above puts the route on the cable whenever it has an
address. Only Ethernet events from its own driver and netif count, so an app
that installs a second Ethernet driver of its own does not move this one's
route.

There is little to configure. The EMAC's pins are IDF's
`ETH_ESP32_EMAC_DEFAULT_CONFIG`, which on the ESP32-P4 is exactly the Waveshare
ESP32-P4-WIFI6-POE-ETH wiring (MDC 31, MDIO 52, RMII clock in on GPIO 50). What
varies per board is under menu "espOS Ethernet":

| Kconfig | ESP32-P4 default | |
|---|---|---|
| `CONFIG_ESPOS_ETH_PHY_ADDR` | `1` | the PHY's MDIO address; `-1` scans |
| `CONFIG_ESPOS_ETH_PHY_RST_GPIO` | `51` | the PHY's active-low reset; `-1` for none |

The PHY is driven by IDF's generic IEEE 802.3 driver. IDF 6 moved the named
PHY drivers (IP101, LAN87xx, ...) to the component registry; the generic one
reads link, speed and duplex from the standard registers, which is all a
transport needs, and adds no dependency.

`espos_start()` starts it when the component is in the build and
`CONFIG_ESPOS_ETH` is on, and **does not fail if it cannot**: a PHY that does
not answer is logged and the device carries on, rather than turning
`ESP_ERROR_CHECK(espos_start())` into a reboot loop on a device that could
have come up on WiFi. An unplugged cable is not an error at all. A wrong PHY
address or reset GPIO fails the same quiet way, which is why the install error
names both settings.

mDNS follows the cable without help: the netif is IDF's default Ethernet netif
(`ETH_DEF`), which the responder picks up when `CONFIG_MDNS_PREDEF_NETIF_ETH`
is on, its default.

On a chip without an internal EMAC (ESP32-C3, C6, S3) the component compiles to
stubs: `espos_eth_start()` returns `ESP_ERR_NOT_SUPPORTED`.

## mDNS

`espos_net` runs the device's mDNS responder (`espos_mdns.h`, `src/mdns.c`;
it moved here from `espos_wifi` with the API unchanged). It answers for
**`<hostname>.local`** — `net.hostname`, default `espos-<id>` — with that
name as the instance name, and advertises two services on `httpd.port`:

| Service        | TXT                                                                                    | For                                         |
|----------------|----------------------------------------------------------------------------------------|---------------------------------------------|
| `_http._tcp`   | `path=/`                                                                               | browsers, "open the device" in any mDNS app |
| `_espos._tcp`  | `v=<app version>` `app=<app name>` `espos=<espOS version>` `target=<chip>` `id=<short id>` `api=/api/v1` `auth=0` | finding every espOS device with one query and knowing what it is before fetching anything |

`v` and `app` are IDF's `PROJECT_VER` / `PROJECT_NAME` (the values
`esp_app_desc_t` carries, `git describe` for the version in a tagged
checkout), compiled in by the component's `CMakeLists.txt`; `espos` is
espOS's own `version.txt` (the manifest version in a registry-installed
copy). `id` is `espos_net_short_id()`. `auth=0` is a fixed value: the advertisement
predates `httpd.api_key` and does not reflect whether a key is set. Ask
`GET /api/v1/system/ping`, whose `auth` field is live.

`espos_net_start()` brings the responder up before any interface has an
address. That is deliberate: the responder accepts records without a link
and announces them itself on `GOT_IP`, and doing the work there rather than
in a `NETWORK_UP` handler keeps the event loop free (`mdns_hostname_set()`
waits for the responder task; `mdns_service_add()` takes a lock that task
holds while it parses packets — neither belongs in a handler). The link
state is tracked separately: **`ESPOS_EVENT_MDNS_READY`** is posted on every
`NETWORK_UP` once the responder runs, and `espos_mdns_is_ready()` answers
true exactly while a query or an announcement can reach the network (false
again on `NETWORK_DOWN`). SignalK discovery starts browsing on that signal.

### Registering a service

```c
#include "espos_mdns.h"

static const char *const txt[] = { "schema=1", "widgets=label,value,toggle", "api=/layout,/hello" };
ESP_ERROR_CHECK(espos_mdns_add_service("_signalk-player", "_tcp", 8081, txt, 3));
/* ... */
espos_mdns_remove_service("_signalk-player", "_tcp");
```

* Callable **any time**, from any application task or `app_main()` — before
  `espos_start()`, before the network, before the responder exists. The
  entry waits in a table of `CONFIG_ESPOS_NET_MDNS_MAX_SERVICES` (default 6)
  slots and is registered when the responder comes up; afterwards it is
  registered at once. There is no readiness to wait for and nothing to retry.
* Adding a `(type, proto)` that is already in the table replaces its port
  and TXT: the old record is withdrawn, the new one announced.
* Limits (refused, never truncated): type < 32 chars starting with `_`,
  proto `_tcp` or `_udp`, up to 8 TXT items totalling 256 bytes with their
  separators. `"k"` without `=` is a flag item (empty value).
* Returns `ESP_ERR_INVALID_ARG` / `ESP_ERR_INVALID_SIZE` for a malformed
  request, `ESP_ERR_NO_MEM` when the table is full, and the responder's own
  error when it refuses the record (its ceiling is `CONFIG_MDNS_MAX_SERVICES`,
  default 10: the two built-ins, these slots and anything a component adds
  with `mdns_service_add()` directly all count) — then the entry is
  dropped, not queued. `ESP_ERR_NOT_SUPPORTED` when built without the
  responder.
* Threading: `espos_mdns_start()`, `espos_mdns_add_service()` and
  `espos_mdns_remove_service()` run on the caller's task and may block for a
  few milliseconds on the responder — not from an `ESPOS_EVENT` handler or a
  URI handler. `espos_mdns_is_ready()` only takes the table mutex.

What this replaces in consumers: a 2-second retry loop around
`mdns_service_add()` that watched for `ESP_ERR_INVALID_STATE` until espOS
happened to have started the responder (the P4 cockpit's
`mdns_announce.cpp`) becomes the one call above. `espos_n2k` keeps its own
`mdns_service_add()` in the candump server: that component deliberately has
no espOS dependencies, and the responder is the same one either way.

### Configuration

* `CONFIG_ESPOS_NET_MDNS` (default y) — the responder and the whole API.
  Off saves the responder task and its sockets (~4 KB) and makes the device
  reachable by address only; SignalK discovery cannot run without it and
  `sk.server_host` must be set. Not available on the linux target
  (`espressif/mdns` does not exist there), where `espos_mdns_*` compile to
  stubs returning `ESP_ERR_NOT_SUPPORTED` / `false`. `CONFIG_ESPOS_WIFI_MDNS`,
  the 0.7 name, still exists as a derived read-only symbol so files that
  spell it keep meaning something; setting it does nothing.
* `CONFIG_ESPOS_NET_MDNS_MAX_SERVICES` (default 6, 1..16) — application
  service slots.
* `net.hostname` (restart required) — the `.local` name.

## Builds without WiFi

`espos_core` requires `espos_wifi` on every chip with a radio, or — the
ESP32-P4 — a co-processor; the 802.15.4-only H-series (esp32h2, esp32h21,
esp32h4) has neither, `esp_wifi` does not exist there and `espos_wifi`
cannot build. `espos_project_prologue()` excludes it on those targets and
records the decision as the `ESPOS_WIFI` build property, which
`espos_core`'s `CMakeLists.txt` reads in early expansion (before sdkconfig
exists, so a Kconfig symbol could not carry it); a project that bypasses the
prologue gets the same rule from the target name. Everything above the seam
builds and starts: `espos_start()` runs `httpd → net → [sk] → [ota]`, logs
that no transport is built, and the device has a network the moment some
transport reports one.

`CONFIG_ESPOS_WIFI` (espOS core, default y, offered on chips with WiFi) is
the runtime switch for a firmware that *links* `espos_wifi` but carries its
network elsewhere — an Ethernet gateway on a WiFi chip: off, `espos_start()`
skips `espos_wifi_start()` and the station, its portal and the `/wifi`
endpoints never come up; nothing else changes.

## Design notes

* Only `espos_net_report()` and `espos_net_register_if()` are for
  transports; everything else is for consumers. The header carries no IDF
  type (the netif is `void *`), so a binding generated from it does not drag
  `esp_netif.h` along.
* The WiFi wrappers `espos_wifi_short_id()` and `espos_wifi_backoff_ms()`
  stay for one release (removed in 0.9); the former returns
  `espos_net_short_id()`, the latter is the same curve kept standalone so
  the WiFi state machine still builds on its own in its host test.
  `espos_wifi_get_status()` is not deprecated — it is the WiFi-specific
  view — but it is no longer the answer to "is the network up".
* On the linux target the port has no netif and a fixed base MAC
  `02:00:00:00:1a:2b`, the address the simulated station always had, so the
  host device keeps its id `1a2b` and the REST harness its expectations.
