# SignalK (`espos_sk`) — discovery, access token, delta stream, inbound

M3: find the server, get and keep a token. M4: stream published values as
deltas over a WebSocket, buffer them while offline, reconcile metadata,
publish device health. M7: subscribe to paths and families, receive values
and meta, send PUT requests and raw frames — what a display or controller
needs on top of a sensor. Plus one HTTP client for everything else an
application asks the server over REST (`espos_sk_http.h`).

## Discovery

`espos_sk` browses `_signalk-http._tcp` via mDNS every `sk.discover_s`
(default 60 s, immediately when WiFi comes up) and keeps up to
`ESPOS_SK_MAX_SERVERS` (12) servers with their TXT records (`self`, `roles`,
`swname`, `swvers`). Entries that
drop out of one query survive two intervals (mDNS is lossy).
`GET /api/v1/sk/servers` lists them; `sk_servers` SSE events fire after
every pass.

The responder the queries go through — and the device's own
`<hostname>.local`, `_http._tcp` and `_espos._tcp` records — is
`espos_wifi`'s ([wifi.md](wifi.md), "mDNS"); `espos_sk` only browses. A pass
waits for `espos_mdns_is_ready()` (the station reports connected a few
milliseconds before the responder's `ESPOS_EVENT_MDNS_READY` reaches it,
and an empty first pass would only be retried a whole interval later) and
returns nothing without a link. Built with `CONFIG_ESPOS_NET_MDNS=n` there
is no responder to browse with: discovery is off and `sk.server_host` must
be set.

Which server is used:

| `sk` config                          | choice                                             |
|--------------------------------------|----------------------------------------------------|
| `server_host` set                    | that host:port (manual, for networks without mDNS) |
| `server_self` set                    | the discovered server with that self URN           |
| neither                              | sticky: the server our token / pending request belongs to (its address first, then its self URN); else the discovered `master` with the lowest self URN; else any |

Discovered servers that turn out unreachable (wrong subnet, gone) are
skipped for five minutes so one dead entry cannot block the machine.

## Token state machine

`sk_token_sm.c` is pure C over an injected port (HTTP calls, storage,
timer, clock) and is unit-tested for every transition
(`test/host/espos_sk_test`). Verified against a real signalk-server 2.31
(the flow, status codes and body shapes below are what it actually
returns).

```
NO_SERVER ──server known──▶ evaluate:
   pending href for this server? ──▶ REQUESTED (resume polling)
   stored token for this self?  ──▶ VERIFYING ── 200 ▶ APPROVED
   else                          ──▶ IDLE: POST /signalk/v1/access/requests
                                            {clientId, description, permissions}
        202 {state:PENDING, href}   ▶ REQUESTED, href persisted
        404 (security disabled)     ▶ OPEN  (no token needed; re-POSTed every 60 s —
                                            GET /self is blind to security when allow_readonly is on)
        403 (device requests off)   ▶ DENIED
        400 "already requested"     ▶ ERROR, retry in 60 s
        unreachable / 5xx           ▶ ERROR, backoff 10 s → 5 min
REQUESTED: GET href every 5 s, ×1.5 up to 60 s
        state PENDING               ▶ keep polling
        COMPLETED + APPROVED + token▶ token persisted (keyed by self) ▶ VERIFYING
        COMPLETED + DENIED          ▶ DENIED (no auto retry; UI offers "request again")
        404 / 500 "not found"       ▶ server lost it ▶ IDLE (request again)
VERIFYING / APPROVED: GET /signalk/v1/api/self with Bearer
        200                         ▶ APPROVED (self URN learned/updated), re-check every check_s
        401 / 403                   ▶ token dropped ▶ IDLE (request again)
APPROVED + any other SK call reporting 401/403 (espos_sk_report_unauthorized) ▶ IDLE
```

Design points from the plan, all implemented:

* **`clientId` is a v4 UUID generated once** and stored in the `skstate`
  NVS namespace, never in the exported configuration; it survives config
  import/export and factory-reset only wipes it because the whole partition
  goes.
* **Tokens are keyed by the server's `self` URN.** A server that changes
  address keeps its token (discovery re-resolves the host by self); a
  reinstalled server (new self) gets a fresh request; a token learned for a
  manual host without mDNS has its self filled in from the first successful
  verify.
* **...and by the address it was issued at.** A self URN is not unique: a
  second server configured with the boat's MMSI (a test VM, a spare Pi)
  advertises the same one. Selection and the discovery list therefore anchor
  on the token's (or pending request's) host:port first -- as long as that
  address still answers with the same self, since DHCP can hand it to another
  server. A server with the same self at another address is a twin and is not
  elected until the anchored address has dropped out of the discovery list
  (two discovery intervals plus 5 s without an answer) and gone unheard for
  two discovery rounds in which anything answered; rounds with no answers at
  all (network down) do not count. After that it may be our server at a new
  address, so it is tried, but only as a stand-in (`server.by_self` in
  `GET /sk/status`): a 401/403 from it, or a 404 for the pending request,
  keeps the token and the request and waits (state `error`, with backoff)
  rather than clearing them and asking that server for access. When the
  anchored address answers again it is chosen again. If the server really
  moved and has also revoked the token or pruned the request, pin it
  (manual server, or "Use" in the server list): a pinned server's answer
  counts again. Otherwise restarting the
  real server long enough for two rounds handed the pairing to the twin for
  good: the token was refused there, cleared, and replaced by a request on the
  twin, which then became the anchor. Without the address anchor, mDNS answer order picked between the twins
  every round: a pending request polled on the other one came back 404 and
  was dropped, so the approval was never collected and the device kept
  asking. Among servers with the same self the one in use stays, then the
  first by address (string order) wins.
* **A recorded token address is not re-homed by a verify**: a twin with
  security off answers 200 to any token, and following it would anchor the
  device there for good. A token stored by older firmware, which has no
  address, learns it from its first successful plaintext verify; over TLS
  (no verify leg) it keeps the self-only behaviour.
* **A pending `href` is persisted** with the server it belongs to; a reboot
  mid-approval resumes polling instead of creating a duplicate request.
* **Manual token paste**: `POST /api/v1/sk/token {"token": "…"}` → verified
  immediately.
* Secrets: the token never appears in any API response or SSE event; the
  store lives in the same (optionally encrypted) NVS partition as the config.

## Delta stream (M4)

`espos_sk_publish_number/string/bool/json(path, value)` is the whole app
API: thread-safe, never blocks, works before WiFi is up (but only after `espos_sk_start()` — earlier calls return `ESP_ERR_INVALID_STATE`). Values are for
`vessels.self`; the source label is `espos.<hostname>`.

**Another vessel.** `espos_sk_publish_number/string/bool/json_ctx(context,
path, value)` publish the same way for any context, which is how a dinghy
logger relays its sounder to the mothership's server as a boat of its own.
`espos_sk_vessel_context()` gives the device's own one,
`vessels.urn:mrn:signalk:uuid:<uuid>`, with the UUID generated on the first
`espos_sk_start()` and kept in NVS (namespace `skstate`, key `vessel_id`),
apart from the clientId: it survives reboots and OTA and is new only after
an NVS erase. The status document shows it as `vessel_context`. A batch
window holds values for up to four contexts and sends one delta per context,
since a delta carries only one. The `_ctx` calls refuse a NULL context
rather than read it as `vessels.self`.

These are for live data. signalk-server keeps a delta's timestamp
(`src/index.ts`), but its full model takes the newest delta it receives as
the vessel's current state without comparing timestamps
(`src/deltacache.ts`, `ingestDelta`), the staleness check flags old values
at once, and every plugin and client receives them as if they had just
happened. Replaying hours of recorded data this way walks the vessel back
through its old positions; recorded data goes to its consumer as a batch
upload instead. The offline ring below is the short version of the same
replay, and acceptable for it: it holds a bounded outage (32 KiB by
default), drains oldest first and ends on current values.

**A time of its own.** `espos_sk_publish_number/string/bool/json_ctx_at(context,
path, value, at_ms)` date a value with when it was measured, in unix
milliseconds, for a sample that waited on the device: one held through a
short dropout, or a reading the sensor timestamped itself. `at_ms` 0 is the
plain `_ctx` call. The update carries that timestamp whether or not
`sk.timestamps` is on or the device knows the time. A context's values in a
window go out as one delta with an update per timestamp, in the order each
was first published. A value that would join an update sent ahead of an
older value of its path closes the window instead, so each path's values
reach the server in the order they were published; values of different
paths may be reordered. A path alternating between dated and current values
therefore costs a message per change, which is one more reason to finish a
replay before current values resume. A time before 2000 is refused
(`ESP_ERR_INVALID_ARG`): it is uptime or unix seconds passed by mistake.
The caveats above still hold. History stores may not use the timestamp
either: signalk-questdb stamps rows with the time they arrive, on purpose,
and signalk-to-influxdb2 does too by default (`useSKTimestamp`), and skips
other vessels (`onlySelf`). Nothing acknowledges a delta, and the ring drops
its oldest messages when full, so a dated delta is no more durable than any
other: a backlog that must arrive complete goes as an upload.

**Backpressure instead of a drop.** A full ring drops its oldest message,
which is what an offline buffer should do. A caller forwarding a backlog it
cannot afford to lose can call `espos_sk_set_no_drop(true)` instead: a publish
that would close the window against a full ring returns `ESP_ERR_NO_MEM` and
queues nothing, so the caller waits for the stream task to drain and retries
rather than losing the head of its backlog. Only a window-closing publish is
refused -- while the window is open, publishes return `ESP_OK` and the values
stay pending, so `ESP_OK` is not a promise the value reached the ring. A window
too large for the ring goes out with the ordinary drop behaviour instead of
being refused forever. Turn it off again for live values, where dropping the
oldest is the right trade. It is backpressure, not durability: a send failure
requeued with the ring full still drops the oldest message, and the ordering
caveats above hold, so a replay should end before current values resume and a
backlog that must arrive complete goes as an upload.

Pipeline (`sk_delta.c`, pure C, unit-tested; `sk_ws.c` = the transport
task):

1. **Batching window** (`sk.batch_ms`, default 100 ms): everything
   published inside one window becomes one delta message per context,
   carrying that context's paths (a device that publishes only for
   `vessels.self` sends one message per window); a path published twice
   in a window for the same context keeps the last value. Numbers use
   the shortest round-trip representation.
2. **Ring buffer** while the stream is down (`sk.buffer_msgs` /
   `sk.buffer_kb`, default 128 messages / 32 KiB): oldest messages are
   dropped first and counted (`ws.dropped`). Windows keep closing while
   offline, so a path's history survives, not just its latest value.
3. **Drain** after (re)connect at `sk.drain_per_s` (default 20/s) so the
   server is not swamped by a backlog; new values queue behind the backlog
   so ordering per path is preserved.

The WebSocket task (`espos_skws`) runs when `sk.ws_enabled`, WiFi is up, a
server is selected and the token state allows streaming (approved, or the
server has security off). It connects to
`ws://<host>:<port>/signalk/v1/stream?subscribe=none` with
`Authorization: Bearer <token>`, consumes the hello, then sends deltas as
text frames. A `401` on connect calls `espos_sk_report_unauthorized()` (the
token machine re-verifies / re-requests); any other failure backs off with
the shared WiFi backoff curve (`ws.next_retry_s`). Config changes to the
stream keys are picked up live; `ws_enabled=false` closes the socket and
keeps buffering.

**Meta reconciliation.** `espos_sk_declare_meta(path, meta_json,
period_ms)` records metadata for a NON-standard path (spec paths belong to
the server). On every (re)connect the task `GET`s
`/signalk/v1/api/vessels/self/<path>/meta`; if the server has none it
`PUT`s ours, otherwise the server's copy — possibly edited by the user —
wins. `period_ms > 0` adds `timeout` (2.5× the period, in seconds), the one
field the device really owns. `ws.meta.declared/reconciled` show progress.

**Device health.** Every `sk.health_s` (default 10 s, 0 = off) the task
publishes `espos.<hostname>.{uptime,freeHeap,minFreeHeap,internalFree,largestBlock,rssi,
wifiReconnects,skReconnects,resetReason}` with declared meta, so a
dashboard sees the device without any app code.

Wire facts that cost time (signalk-server 2.31): client text frames must
be sent with the FIN bit (`WS_TRANSPORT_OPCODES_TEXT |
WS_TRANSPORT_OPCODES_FIN`) or the server closes the socket after the
first frame; `subscribe=none` still delivers the hello; meta `GET` is
`404` when unset and `PUT` takes `{"value": {…}}`.

## Inbound (M7)

```c
int h = espos_sk_subscribe("navigation.*", 1000, on_update, NULL);   /* family */
espos_sk_subscribe("environment.mode", 0, on_update, NULL);           /* exact */
espos_sk_unsubscribe(h);
espos_sk_put("navigation.anchor.maxRadius", "30", on_put_done, NULL);
espos_sk_send_raw("{\"context\":\"vessels.self\",\"updates\":[…]}");
```

* **Subscriptions** are exact paths or families (`prefix.*`, `prefix*`,
  `*`). The stream is opened with `subscribe=none&sendMeta=all`; after the
  hello (and after every reconnect) one `{"context":"vessels.self",
  "subscribe":[{"path","period","format":"delta","policy":"instant",
  "minPeriod"}]}` frame carries every subscription; new ones while
  connected go out incrementally, `espos_sk_unsubscribe` sends
  `unsubscribe` when nothing else covers the pattern. Up to
  `ESPOS_SK_MAX_SUBS` (48).
* **Delivery**: `sk_parse.c` (pure C, cJSON) turns each frame into items —
  `path`, `value_json` (verbatim JSON text: numbers, strings, objects,
  `null`), `timestamp`, `$source`/`source.label`, `context` — plus meta
  items (`meta_json` set, value NULL) when the server sends `meta`. The
  callback runs on the stream task; copy what you need and return (a
  display marshals to its UI thread — never block, never call an
  `espos_sk_*` function that could wait on the stream). Frames are
  reassembled up to `CONFIG_ESPOS_SK_RX_FRAME_MAX` (16 KiB); larger ones
  are dropped with a log line.
* **PUT (outbound)**: `{"context":"vessels.self","requestId":<uuid4>,"put":
  {"path","value"}}`; the response (`state` COMPLETED/FAILED, `statusCode`,
  `message`) is matched by requestId and handed to the callback; no answer
  in 10 s → `"TIMEOUT"`. Up to 8 in flight; `ESP_ERR_INVALID_STATE` when
  the stream is down (nothing is queued across reconnects — a control
  action must not fire minutes later). A real server without a handler
  answers `COMPLETED` with `statusCode 405 "PUT not supported for …"`.
* **Raw frames** (`espos_sk_send_raw`) go out ahead of buffered deltas —
  e.g. an inbound `notifications.*` delta with `state:"normal"` to
  acknowledge an alarm.
* Status: `ws.in {subs, frames, received}`, `ws.put {pending, ok,
  failed}`; REST `POST /api/v1/sk/put {"path","value"}` (202; last
  answer under `GET /api/v1/sk/put`) for scripts.
* The example app subscribes to `app.watch_path` and logs each update.
* **PUT (inbound)** — the server operating *this* device — is a separate
  mechanism with its own handler table; see
  [Inbound PUT (control)](#inbound-put-control).

Verified 2026-08-18 against signalk-server 2.31 on the ESP32-P4: 586
updates in ~40 s of `navigation.*` from N2K sources, satellitesInView
objects of several KiB reassembled, PUT round trip (405 from a server
without handlers).

## Inbound PUT (control)

Everything above is this device *asking* the server for something. This
section is the other direction: **the server asking this device to change
something** — a switch operated from a phone, a setpoint moved from a
plotter. Without it a device can only ever report.

```c
static esp_err_t set_bilge(const char *path, const char *value_json, void *arg)
{
    if (strcmp(value_json, "true") == 0) { gpio_set_level(RELAY, 1); return ESP_OK; }
    if (strcmp(value_json, "false") == 0) { gpio_set_level(RELAY, 0); return ESP_OK; }
    return ESP_ERR_INVALID_ARG;                 /* answered COMPLETED 400 */
}

espos_sk_put_handler_register("electrical.switches.bilge.state", set_bilge, NULL);
espos_sk_publish_bool("electrical.switches.bilge.state", false);   /* REQUIRED, see below */
```

The handler's return value becomes the answer:

| return | answer |
|---|---|
| `ESP_OK` | `COMPLETED` 200 |
| `ESP_ERR_INVALID_ARG` | `COMPLETED` 400 |
| anything else | `COMPLETED` 502 |
| `ESPOS_SK_PUT_PENDING` | `PENDING` 202, and you call `espos_sk_put_respond()` later |
| *no handler for the path* | `COMPLETED` 405 |

### Three things that are easy to get wrong

**The device must publish the path first.** signalk-server routes a PUT to a
device by the `(path, $source)` pairs it has *seen that connection publish*
(`processUpdates` in `src/interfaces/ws.ts`). A path this device has never
published does not exist as a PUT target, and the request is answered 405 by
the server without ever reaching the device. Publish the current state once at
boot, and again on every change.

**`put` arrives as an ARRAY.** The server writes
`{"requestId","context","put":[{"path","value"}]}` — an array, even for one
path. That is not the shape a client sends outbound (`espos_sk_put` writes a
single object), and a parser that only understands the object form silently
sees no requests at all. espOS accepts both.

**The reply state must be `COMPLETED` or `PENDING`.** signalk-server's
`isWsRequestReply()` accepts a `state` of exactly `COMPLETED`, `PENDING` or
`null` and **silently ignores** anything else. A reply with
`"state":"FAILED"` — the obvious spelling for a failure — is dropped without
a word, and the client then waits out the server's full 60-second timeout. A
failure is `COMPLETED` with a 4xx/5xx `statusCode`.

espOS answers **every** request, including one whose path has no handler and
one whose items are unusable. Silence is the worst answer: it costs the
client 60 seconds and tells it nothing.

### Flushing before sleep

```c
espos_sk_flush(2000);      /* send what is buffered, then deep-sleep */
```

Deltas are batched (`sk.batch_ms`), so a value published a millisecond before
`esp_deep_sleep_start()` is still sitting in the buffer when the radio goes
down. `espos_sk_flush()` closes the batch and waits — `ESP_OK` when nothing is
left, `ESP_ERR_TIMEOUT` if the deadline passed, `ESP_ERR_INVALID_STATE` when
the stream is down and nothing *can* drain. Never call it from the stream task
or a subscription callback.

The message the stream task is writing at that moment counts as pending until
the write returns, so `ESP_OK` means everything was handed to the socket. It
does not mean the server has acknowledged it: on a live link the TCP stack
sends within milliseconds, and code about to cut the radio should allow it
that moment.

## The graph nodes (`espos_sk_flow`)

`espos_sk_flow` is the Signal K end of the [data-flow graph](flow.md) — the
same calls as above, as nodes.

```cmake
idf_component_register(SRCS main.cpp PRIV_REQUIRES espos_core espos_flow espos_sk_flow)
```

| Node | Direction |
|---|---|
| `sk::Output<T>(path[, Meta])` | publish |
| `sk::Listener<T>(path)` | receive a value from the server |
| `sk::PutHandler<T>(path)` | let the server change something here |
| `sk::PutRequest<T>(path)` | ask the server to change something |
| `sk::Notify(key, message)` | raise/clear a device condition |
| `sk::NetRssi`, `sk::IpAddress` | what the network says about itself |

### Metadata is impossible to get wrong

**Never send metadata for a path in the Signal K specification.** The server
already knows that `navigation.speedOverGround` is metres per second; a device
that declares it anyway can only get it wrong, and then every dashboard on the
boat is wrong.

So `Output` has **no units argument**. A units string cannot be passed without
constructing a `Meta`, and constructing a `Meta` is the statement "this path
is mine, nobody else knows what it means" — which is exactly when metadata is
correct:

```cpp
sk::Output<float> sog("navigation.speedOverGround");             // spec: no meta, ever
sk::Output<float> pv("sensors.solar.0.voltage", sk::Meta{"V"});  // ours: meta declared
```

The rule is not documented and hoped for; it is unspeakable.

`Output<std::optional<T>>` publishes JSON `null` when disengaged — "this
sensor has nothing right now", which is different from zero and different
from stale.

### Receiving

`Listener` and `PutHandler` receive on the **stream task** and neither emits
there: both post into a `Mailbox`, so the emit happens on the flow task like
every other node. That is why you wire from `.out()`:

```cpp
auto& depth = g.make<sk::Listener<float>>("environment.depth.belowTransducer");
depth.out() >> shallow_alarm;
```

### A switch a phone can operate

```cpp
auto& req   = g.make<sk::PutHandler<bool>>("electrical.switches.bilge.state");
auto& relay = g.make<espos::sensors::GpioOutput>("relay", 22);
auto& state = g.make<sk::Output<bool>>("electrical.switches.bilge.state");

req.out() >> relay >> state;     // PUT -> pin -> publish what the pin did
```

Publishing at the end of the chain is what registers this device as the
path's source, without which the server has nowhere to route the PUT. It also
publishes what the pin *actually did* rather than what was asked for — they
differ when the pin failed to open, which is exactly when a dashboard must not
lie.

`PutHandler` answers 200 as soon as the value is accepted into the mailbox,
not once the chain has run: the alternative is to block the stream task until
the flow task finishes, and one slow consumer would then stall every other
frame on the connection.

## HTTP requests to the server

`espos_sk_http.h` is the one way an application talks HTTP to the selected
server. Four hand-rolled copies of "GET a SignalK REST node" in one firmware
had two of them rebooting the device; this is the version that does not.

```c
#include "espos_sk_http.h"

espos_sk_http_resp_t r;
if (espos_sk_http_get("/signalk/v1/applicationData/global/my-app/1/layout.json", NULL, &r) == ESP_OK
    && r.status == 200 && !r.truncated) {
    apply_layout(r.body, r.len);              /* NUL-terminated, malloc'ed */
}
espos_sk_http_resp_free(&r);

char *value = NULL;                          /* GET …/vessels/self/navigation/position → "value" member */
if (espos_sk_get_value("navigation.position", &value) == ESP_OK) { /* {"latitude":…,"longitude":…} */ }
free(value);
char *meta = NULL;                           /* GET …/navigation/speedOverGround/meta */
if (espos_sk_get_meta("navigation.speedOverGround", &meta) == ESP_OK) { /* {"units":"m/s",…} */ }
free(meta);

espos_sk_http_opts_t o = { .timeout_ms = 3000, .max_body = 512 };
espos_sk_http_post("/plugins/my-plugin/api/thing", "{\"on\":true}", &o, &r);   /* PUT, DELETE likewise */
espos_sk_http_resp_free(&r);

char url[ESPOS_SK_URL_MAX];
espos_sk_url("/signalk/v1/api", url, sizeof(url));       /* http(s)://host:port/signalk/v1/api */
espos_sk_ws_url("/signalk/v1/stream", url, sizeof(url)); /* ws(s)://… */
```

* **Reply**: `status` (0 when nothing arrived), `body` (malloc'ed,
  NUL-terminated, `""` for an empty reply), `len`, `truncated`. The call
  returns `ESP_OK` whenever a reply arrived — a 404 or 500 is a successful
  call; check `status`. `ESP_ERR_INVALID_STATE` means no server is selected,
  `ESP_ERR_TIMEOUT` that no connection slot came free, anything else is the
  transport error `esp_http_client` reported. Always
  `espos_sk_http_resp_free()`.
* **Options** (`NULL` or zeroed = defaults): 6 s timeout, 16 KiB body cap,
  `Authorization: Bearer` from the current token, 401/403 reported,
  `Accept: application/json`. `no_auth` drops the header, `no_report_unauthorized`
  keeps a 401 from touching the token machine, `accept` overrides the header
  (`""` = none).
* **Body cap**: the body is collected in `HTTP_EVENT_ON_DATA` and stops at
  `max_body`; beyond it `truncated` is set and the rest is drained and
  discarded. Never parse a truncated body — treat it as "the reply was too
  big" (`espos_sk_get_value/meta` return `ESP_ERR_INVALID_SIZE`). The buffer
  grows with the reply, so the cap costs nothing for small documents.
* **Token**: snapshotted per call from `espos_sk_get_token()`, sent as a
  header — never in a query string, where it would end up in every proxy and
  server log. No token, no header, which is what a server running without
  security expects. A 401/403 while a token was sent calls
  `espos_sk_report_unauthorized()`: the token machine re-verifies and, if the
  server really has dropped the device, requests access again.
* **Scheme**: from the selected server's `tls` flag, `http`/`ws` or
  `https`/`wss` — decided per server by `sk.scheme` and, under `auto`, by what
  the server advertised or a probe found (below). Certificates go through the
  same trust store as the delta stream.
  `espos_sk_url()` / `espos_sk_ws_url()` build the URL for code that opens its
  own connection (the BLE gateway's control socket does).
* **Concurrency**: `CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT` (default 2, range
  1–8) bounds requests in flight — the token machine's and the meta
  reconciliation's own calls included. Each open request is a socket plus,
  over TLS, ~20 KB of RAM; a display fetching one value per widget on a layout
  change would otherwise open dozens at once. A caller over the limit waits
  up to its own `timeout_ms` for a slot.
* **Threading**: blocking, on the caller's task, for up to `timeout_ms`
  waiting for a slot plus `timeout_ms` on the wire, with ~2 KiB of its stack.
  Call from an application task. Never from the SK stream task (the
  `espos_sk_subscribe`/`espos_sk_put` callbacks), an `ESPOS_EVENT` handler, a
  Bluetooth stack callback or an HTTP URI handler.

### The two crash patterns it avoids

Both were reproduced on the ESP32-P4 against signalk-server, both are inside
`esp_http_client`, and both are why the helper insists on one particular
shape — a **fresh client per call** and **`esp_http_client_perform()` only**:

1. `esp_http_client_open()` → `fetch_headers()` → `read()` leaves the client's
   `cache_data_in_fetch_hdr` flag set. When the body arrives in the same TCP
   segment as the headers — which is every small SignalK reply — the next
   step hits `assert(orig_raw_data == raw_data)` in `http_on_body` and the
   device reboots. Only `perform()` clears the flag.
2. Reusing one handle across `perform()` calls (`set_url()` per path, one
   connection for a batch) desyncs the same two pointers and trips the same
   assert mid-batch (seen on `navigation.anchor.*` paths).

`perform()` with a new handle per request enters neither path; the body is
delivered through the event handler, which is also where the size cap lives.
`sk_http.c` has used this shape for the token legs since M3; the meta
reconciliation and the public API now share that single implementation.

## TLS (https / wss)

**On by default** (`CONFIG_ESPOS_SK_TLS=y`) and, with `sk.scheme` at its
default `auto`, used whenever the server says it speaks it. What changed: the
device now has somewhere to put a certificate no public root signed, which is
what every boat server has.

### Which scheme (`sk.scheme`)

`auto` | `http` | `https`, default `auto`. `http` and `https` force one. What
`auto` does depends on how the server was found:

* **discovered** — signalk-server advertises `_signalk-https._tcp` instead of
  `_signalk-http._tcp` when its `ssl` setting is on
  (`src/interfaces/rest.js`), so the server has already told us. The device
  browses both types; no probe, no round trip.
* **manual host** — nothing has been advertised, so one probe: `GET
  http://host:port/signalk` with redirects switched off. A `30x` to an
  `https://` Location means TLS, and the port is taken from the Location if it
  names one (an SSL-enabled server often listens elsewhere). Nothing answering
  on the plain port is tried once over https before concluding the host is
  down. **The probe carries no token** — it is aimed at an address that has not
  been established as our server yet, and handing the credential to whatever
  answers is exactly what must not happen. A manual host that discovery saw
  within the carry-over window (picked from the server list, say) takes the
  scheme it advertised instead, with no probe.

  No answer on either scheme is not an answer. The host may be down, still
  booting, or not reachable yet, and on a boat the server usually comes up
  after its devices. The device uses plain http as a guess, does not remember
  it, and asks again when the network comes up and after every attempt that
  runs on the guess. For the same reason a manual host is not selected, or
  probed, before there is a network at all.

An answer is a field on the chosen server (`espos_sk_server_t::tls`), cached
per `(host, port)` and re-decided only when the selection changes. It is *not*
`restart_required` any more: `sk_ws.c` destroys and rebuilds its transport pair
when the scheme changes, so a server that gains or loses TLS is followed live.

`GET /sk/status` reports what is actually in use as `server.scheme`, which
under `auto` is the only place to read it.

### Which certificates are trusted (`sk.tls_trust`)

`tofu` | `ca` | `bundle`, default `tofu`. **There is no accept-anything mode.**

* **`tofu`** — trust on first use, the way ssh does it. The first connection
  that works pins what the server presented; every later one must match.

  Two shapes of anchor, because certificates get renewed:

  * **CA anchor** — the highest `CA:TRUE` certificate in the chain, *plus* the
    normalised set of the leaf's dNSName/IP SANs. A renewal signed by the same
    CA for the same names is accepted with nobody pressing anything, which is
    what makes a 90-day certificate survivable on a device in a locker.
    Binding the SAN set as well as the CA matters: a private CA that signs one
    host would otherwise vouch for every other name it ever signs.
  * **Leaf anchor** — the SHA-256 of the leaf itself, used when the chain has
    no CA or the leaf carries no SAN. signalk-server's own generated
    self-signed certificate lands here, and a renewal then needs one
    deliberate "trust the new certificate".

* **`ca`** — the same machinery with the anchor supplied instead of captured:
  put the issuing CA in `sk.ca_pem` (a blob; base64 over the REST API) or
  `PUT /api/v1/sk/tls/ca {"pem": "-----BEGIN CERTIFICATE-----…"}`. Setting it
  anchors the device immediately, so a fleet that pre-seeds the key connects on
  the first try with nothing to capture. Invalid PEM is a `400`, not a device
  that quietly stops connecting.

* **`bundle`** — the public Mozilla roots and nothing pinned: the pre-S1
  behaviour, for a server with a real certificate from a real CA.

The common-name check is switched off outside `bundle` mode. It would add
nothing — the certificate has already been matched by fingerprint and SAN set —
and it fails on a boat server reached by IP, which is most of them.

### Stash, then commit

A verify callback runs *during* a handshake that has not finished. Pinning
there would let a machine-in-the-middle answering a first connect plant its own
certificate as the anchor and be trusted from then on. So the callback only
fills a capture slot in RAM, and `espos_sk_tls_commit()` writes it to NVS after
the connection has proved itself — an HTTP status from the far end, or a
WebSocket `101`. Every handshake starts by discarding whatever the last one
left.

The commit runs on whichever task made the request. When that task's stack is
in PSRAM (the BLE gateway's can be), the NVS write goes to a short-lived task
with an internal stack instead, and so does a retried load of the anchor after
a failed one at boot: flash access switches the cache, and PSRAM with it, off,
and a task whose stack vanishes mid-access faults. A save that fails is retried
on the next commit.

The anchor lives in the `skstate` NVS namespace next to the token
(`tls_kind`, `tls_fp`, `tls_ca`, `tls_san`, `tls_cn`, `tls_self`, `tls_at`):
device state, so it does not travel with a configuration export. `sk.ca_pem`
is configuration and does.

### When it does not match: `cert_error`

The token machine gains a state. `token.state == "cert_error"` means the
transport was refused, not the credential:

* **the token is kept.** Dropping it would mean a fresh approval in the
  server's admin UI after every renewal.
* **retry is a flat 60 s**, not the exponential ladder an unreachable server
  gets. The fix arrives from outside — the server renews, or somebody presses
  the button — and should be noticed within a minute rather than an hour.
* **the stream stays down** (`espos_sk_stream_allowed()` is false). Falling
  back to plaintext would send the token to whoever answered.
* `espos_health` carries it as `skCertificate` (ALARM, non-fatal — a reboot
  would not fetch a new certificate), which also publishes it as a SignalK
  notification.

`GET /api/v1/sk/tls` shows the pinned identity next to the presented one, which
is what makes it actionable: an operator who can compare the two fingerprints
knows whether this is their own renewal. `DELETE /api/v1/sk/tls` forgets the
anchor and retries at once — the SignalK page's "Trust the new certificate"
button.

### The plaintext 401 rule

Over TLS nothing on the path can inject an answer, so a `401` on a request that
carried our token is the server's and clears it, as before.

Over **plaintext** it is not. A captive portal, a proxy, a router's own login
page — all of them answer `401`, and throwing the token away over one costs a
trip to the server's admin UI to approve the device again. So the first
unauthorised answer on a plaintext connection buys a second opinion instead:
keep the token, ask again in 5 s, and only clear if that is refused too. Any
`200` resets the count. (SensESP's `should_clear_token_on_status`, same
reasoning.)

### Heap

A TLS handshake wants around 20 KB of *contiguous internal* RAM, and that is
the pool an ESP32 with a WiFi stack is short of — total free heap is the wrong
number on a PSRAM board, where tens of megabytes hide the few kilobytes that
matter.

* **One handshake at a time, device-wide.** Two at once is where the pool runs
  out; it also gives the esp-tls attach hook, which takes no user pointer, a
  well-defined capture slot. A caller waits
  `CONFIG_ESPOS_SK_TLS_HANDSHAKE_TIMEOUT_MS` (10 s) for it, then retries on its
  own backoff.
* **Pre-flight.** Below `CONFIG_ESPOS_SK_TLS_MIN_FREE_BLOCK_KB` (24 KB) of
  largest free internal block the handshake is deferred and `tlsMemory` is
  raised as a WARN. What that avoids is not a clean out-of-memory error:
  mbedTLS failing mid-handshake leaves a half-built session and a socket
  behind, and on a reconnect loop it starves the WiFi stack of the same pool.
* **The verify leg is skipped on TLS servers.** The WebSocket upgrade carries
  the same token and rejects it just as plainly, so the leg would only buy a
  second handshake per reconnect.
* `sdkconfig.d/espos.defaults` turns on mbedTLS's dynamic buffers
  (`MBEDTLS_DYNAMIC_BUFFER`, `DYNAMIC_FREE_PEER_CERT`,
  `DYNAMIC_FREE_CONFIG_DATA`) and drops the server side
  (`MBEDTLS_TLS_CLIENT_ONLY`); `SSL_IN_CONTENT_LEN` stays at 16384, which a
  SignalK subscription burst needs.

### Cost

Flash, measured on esp32c6 as `CONFIG_ESPOS_SK_TLS=y` minus `=n` on the same
tree:

| build shape | y | n | cost |
|---|---|---|---|
| with `espos_ota` (the reference app) | 1 485 308 | 1 475 280 | **~10 KB** |
| without it (the `tls_server` example) | 1 437 772 | 1 358 004 | **~78 KB** |

A firmware that already has `espos_ota` links mbedTLS and the certificate
bundle for the https image source, so all it pays here is the trust store, the
transports and the verify path. Without it, most of the 78 KB is the Mozilla
bundle — which the trust store still needs, as the fallback for `bundle` mode.

RAM: ~20 KB while a connection is open. Note that the mbedTLS dynamic-buffer
settings above cost about 17 KB of flash in *both* columns, because they are in
`sdkconfig.d/espos.defaults` and apply to the OTA client too; they are what
gives the RAM back between handshakes.

### Testing it against a real server

Turn SSL on in signalk-server (Server → Settings → SSL, or `"ssl": true` in
`settings.json`) and restart it. It generates a self-signed certificate and
advertises `_signalk-https._tcp`.

1. The device's SignalK page should show the server with scheme `https` and,
   after the first connect, a pinned **certificate** (leaf anchor — a
   generated self-signed certificate has no CA and often no SAN).
2. Delete signalk-server's certificate and restart it so it generates a new
   one. The device goes to `cert_error` within a minute, keeps its token, and
   the page shows the pinned fingerprint next to the presented one.
3. Press **Trust the new certificate**. It reconnects within a few seconds.
4. For the CA path, issue the server's certificate from a CA of your own and
   put that CA in `sk.ca_pem`; re-issuing the leaf for the same names should
   not interrupt anything, and re-issuing it for a different name should give
   `cert_error` with "names different hosts".

## Task stacks

Two tasks, two Kconfig options, each with measured numbers behind its default.
The client task is at its historical size; the stream task was raised after the
second measurement below.

* **`CONFIG_ESPOS_SK_TASK_STACK`** (default 12288, range 6144–16384) — the client
  task: discovery, the token state machine and its HTTP legs (access request,
  poll, verify), plus the https probe.
* **`CONFIG_ESPOS_SK_WS_TASK_STACK`** (default 10240, range 8192–16384) — the
  stream task: frame reassembly, delta parsing, inbound PUT dispatch, the
  notification sink — and metadata reconciliation, which issues an HTTP GET and
  PUT per path (`reconcile_meta()` is called from `ws_task()`, not from the
  client task).

Measured on an ESP32-C5 running the BLE gateway against a live server —
discovery, an approved token, meta for the published paths — with
`CONFIG_FREERTOS_USE_TRACE_FACILITY=y` and `uxTaskGetStackHighWaterMark()`:

| task | allocated | peak use | never touched |
|---|---|---|---|
| `espos_sk` | 12288 | 1488 | 10800 (88 %) |
| `espos_skws` | 8192 | 5272 | 2920 (36 %) |

A second run on the same kind of board, 112 minutes with the gateway's control
WebSocket on, REST requests every 10 minutes and an OTA attempt every 30
(still plain HTTP/ws):

| task | allocated | peak use | never touched |
|---|---|---|---|
| `espos_sk` | 12288 | 4280 | 8008 (65 %) |
| `espos_skws` | 8192 | 6904 | 1288 (16 %) |

The stream task's peak grew by 1.6 KB with nothing but a busier server, which
left 1.3 KB between it and a stack-protection reboot loop -- before any TLS. Its
default is now 10240. The client task's peak nearly tripled too, which is the
argument above for not lowering its default, made by the data.

**Neither default was lowered on the strength of the headroom either run left,
and the reason is worth stating.** The server in that run was plain HTTP on port 80, so no TLS handshake
completed on either task — and mbedTLS is the deepest call path both of them
have. A 12 % high-water reading taken with the deepest path never taken does not
show the stack is oversized; it shows the measurement was incomplete.

So the numbers above are a floor, not a budget, and what to measure before
lowering either default depends on the build:

* **`CONFIG_ESPOS_SK_TLS=y`** (the default) — take the reading against a `wss`
  server, where the client task's token legs and the stream task's
  `espos_sk_http_get_meta()` / `put_meta()` calls actually go through mbedTLS.
* **`CONFIG_ESPOS_SK_TLS=n`** — `sk_tls.c` is compiled out and the device can only
  speak http/ws, so no wss reading is possible. Measure the busiest workload that
  build does support instead: a reconnect that replays a full subscription burst
  and reconciles meta for every published path.

The floors are 6144 for the client task and 8192 for the stream task, the
smallest round sizes above what each was measured using (4280 B and 6904 B). An
option whose range lets a build fault on the first large frame is a trap rather
than a choice. A stack-protection fault here is a reboot
loop, not a degraded mode.

## API

* `GET /api/v1/sk/status` — token/server/discovery status plus the `ws`
  stream object (see rest-api.md).
* `POST /api/v1/sk/put {"path","value"}` / `GET /api/v1/sk/put` — PUT over
  the stream and the last answer.
* `POST /api/v1/sk/publish {"path","value"[,"meta","period_ms"]}` — publish
  over HTTP.
* SSE `sk_ws` — the `ws` object on every stream change.
* `GET /api/v1/sk/servers` — discovered servers, `selected` flag.
* `POST /api/v1/sk/discover` — run a discovery pass now.
* `POST /api/v1/sk/request` — request again (from denied/error/open).
* `POST /api/v1/sk/token {"token"}` — manual token.
* `POST /api/v1/sk/forget` — drop the token and start over. A request that
  is still pending is kept and polled on (the server holds it anyway and
  refuses duplicates).
* `GET /api/v1/sk/tls` — the pinned certificate identity next to the one the
  server last presented, and why they did not match if they did not.
* `DELETE /api/v1/sk/tls` — forget the pinned certificate and retry now.
* `PUT /api/v1/sk/tls/ca {"pem"}` — supply the issuing CA (switches
  `sk.tls_trust` to `ca`).
* SSE `sk_tls` — the `/sk/tls` document, on connect and whenever the anchor
  changes.
* SSE `sk`, `sk_servers`.

## Testing

* `test/host/espos_sk_test`: 32 Unity cases — token state machine, store,
  delta batching / ring / drain, frame parser and path patterns.
* `test/host/espos_httpd_test` `SkTests`: the real HTTP client and
  WebSocket against a Python mock of the signalk-server security API and
  stream endpoint (approve, deny, revoke, forget, security off, manual
  token, manual host, deltas + meta reconciliation, offline buffering with
  ordered drain; the HTTP helper through a harness probe: 200 with body and
  Bearer header, 404 as a reply, oversize body → `truncated`, value/meta/URL
  lookups, PUT through the same core, 401 → token machine re-requests);
  `SkInboundTests`: subscribe frames, value/meta delivery,
  exact vs family dispatch, PUT round trip / failure / timeout, raw frames,
  unsubscribe + resubscribe after reconnect, 9 KiB frame reassembly.
* Against a real signalk-server on the host: `node bin/signalk-server -c
  <fresh config dir>` from a checkout, `POST /skServer/enableSecurity
  {"userId","password","type":"admin"}`, restart, then approve with
  `PUT /skServer/security/access/requests/<clientId>/approved` using the
  admin cookie from `POST /signalk/v1/auth/login`; revoke with
  `DELETE /skServer/security/devices/<clientId>`.

## Notifications

`espos_sk_notify(key, state, message)` raises or clears a SignalK notification
under `notifications.espos.<label>.<key>`:

```c
espos_sk_notify("wakeService", ESPOS_SK_ALERT_WARN, "wake service unreachable");
espos_sk_notify("wakeService", ESPOS_SK_ALERT_NORMAL, "");   /* cleared */
```

For conditions the device knows about and an operator would want to see: memory
pressure, an overheating chip, a service the firmware depends on having gone
away. Without them these surface as a device that has quietly stopped doing its
job, which looks identical to a hardware fault and is the expensive kind of
problem to diagnose.

* **Level-triggered and idempotent.** Re-raising the same state and message
  sends nothing, so a caller may poll and re-raise on every tick. The first
  raise after boot always goes out, even if it is `NORMAL`, because the server
  may still hold an alert from before a restart.
* `key` is a short stable identifier (`lowMemory`, `wakeService`) -- it becomes
  part of the path, and the path is what a rule or dashboard keys on. The
  `message` is the human half and may change freely.
* `method` is `["visual"]` for warn/alarm and `[]` on clear. What to do about
  it is the server's decision, not the device's.
* Up to `CONFIG_ESPOS_HEALTH_MAX_CONDITIONS` distinct keys (default 8, range
  1-32): the notification is a sink on `espos_health`, so its condition table
  is the cap and one key too many gets `ESP_ERR_NO_MEM`. Deltas are buffered
  like any other while offline. Oversized keys or messages are rejected with
  `ESP_ERR_INVALID_SIZE` rather than truncated -- a clipped key would never
  match on the next call and would leak a slot.

espOS raises `lowMemory` itself, from `espos_health`'s watchdog policy rather
than the SignalK tick, so it exists without SignalK; thresholds and the
restart rule are in [health.md](health.md). Internal RAM is checked separately
because it is the scarce pool on targets with PSRAM -- tens of megabytes free
overall can hide an internal-RAM exhaustion that will take the radio down.

