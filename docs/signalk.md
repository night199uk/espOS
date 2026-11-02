# Signal K

`espos_sk` is how a device talks to the boat's Signal K server. It finds the
server on the network, asks it for access and keeps the token it gets, sends
the device's values as deltas, and receives values and PUT requests back.
It also has one HTTP client for anything else an application wants to ask
the server (`espos_sk_http.h`), and it speaks TLS when the server does.

Requests to the server, the delta stream and OTA downloads from it carry the
User-Agent `espOS/<espOS version> <app>/<app version>`
(`espos_net_user_agent()`), so
the server can tell an espOS device from other ESP-IDF firmware. IDF's own
default, `ESP32 Websocket Client`, is also what SensESP sends, and a server
that works around SensESP's bugs would otherwise apply that workaround to
espOS too.

The page follows that order. If you only want to publish a value, read
[Sending values](#delta-stream-m4). If something doesn't connect, the
[token state machine](#token-state-machine) and
[Troubleshooting](troubleshooting.md) are the places to look.

## Discovery

Every `sk.discover_s` (60 s by default, and at once when the network comes
up), the device looks for `_signalk-http._tcp` and `_signalk-https._tcp`
over mDNS. It remembers up to 12 servers (`ESPOS_SK_MAX_SERVERS`) with their
TXT records (`self`, `roles`, `swname`, `swvers`). mDNS loses answers now and
then, so a server stays on the list for two intervals plus 5 s after it last
answered, which covers two missed rounds.
`GET /api/v1/sk/servers` lists them, and an `sk_servers` SSE event goes out
after every round.

The mDNS responder belongs to `espos_net` ([Net](net.md#mdns)), which also
uses it for the device's own `<hostname>.local`, `_http._tcp` and
`_espos._tcp` records; `espos_sk` only asks it questions. A round waits for
`espos_mdns_is_ready()`, because the link reports connected a few
milliseconds before the responder posts `ESPOS_EVENT_MDNS_READY`, and an empty first round would only be repeated a whole interval
later. Without a network a round finds nothing. In a build with
`CONFIG_ESPOS_NET_MDNS=n` there is no responder: the device has no `.local`
name, discovery is off, and you must set `sk.server_host`.

Which server the device uses:

| `sk` settings | Server |
|---|---|
| `server_host` set | that host and port, for networks without mDNS |
| `server_self` set | the discovered server with that `self` URN |
| neither | the server the device already has a token or a pending request from (by its address first, then its `self` URN); otherwise the discovered `master` with the lowest `self` URN; otherwise any |

A discovered server that turns out to be unreachable (wrong subnet, switched
off) is skipped for five minutes, so one dead entry can't hold everything up.

## Token state machine

To send data to a server with security on, the device needs an access token,
and someone has to approve it in the server's admin page. `sk_token_sm.c`
handles that (`components/espos_sk/src/token_sm.c`). It is plain C that does its HTTP calls, storage and timers
through a small interface, and `test/host/espos_sk_test` tests every
transition. The status codes and replies below are what signalk-server 2.31
actually sends.

```
NO_SERVER ──server known──▶ evaluate:
   pending href for this server? ──▶ REQUESTED (resume polling)
   stored token for this self?  ──▶ VERIFYING ── 200 ▶ APPROVED
   else                          ──▶ IDLE: POST /signalk/v1/access/requests
                                            {clientId, description, permissions}
        202 {state:PENDING, href}   ▶ REQUESTED, href persisted
        404 (security disabled)     ▶ OPEN  (no token needed; re-POSTed every 60 s,
                                            GET /self is blind to security when allow_readonly is on)
        403 (device requests off)   ▶ DENIED
        400 "already requested"     ▶ ERROR, retry 60 s doubling to 10 min
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

What is worth knowing about it:

* **The `clientId` is a random UUID made once.** It lives in the `skstate`
  NVS namespace, not in the settings, so a settings export or import doesn't
  carry it. Only erasing the whole NVS partition, as a factory reset does,
  makes a new one.
* **A token belongs to the server's `self` URN.** A server that changes its
  address keeps the token, because discovery finds it again by its `self`. A
  reinstalled server has a new `self` and gets a new request. A token for a
  manually entered host without mDNS learns its `self` from the first
  successful check.
* **It also belongs to the address it was issued at.** A `self` URN is not
  unique: a second server set up with the boat's MMSI, such as a test VM or a
  spare Pi, advertises the same one. So the device holds on to the address of
  its token (or pending request), as long as that address still answers with
  the same `self`; DHCP can give the address to another server.
  * A server with the same `self` at another address is a twin. The device
    only tries it once the known address has dropped out of the discovery
    list (two discovery intervals plus 5 s without an answer) and has gone
    unheard for two rounds in which some server did answer. Rounds where
    nothing answered, because the network is down, don't count.
  * Even then the twin is only a stand-in (`server.by_self` in
    `GET /sk/status`). A 401 or 403 from it, or a 404 for the pending request,
    doesn't clear the token or the request. The device shows `error` and
    waits with backoff instead. When the known address answers again, the
    device goes back to it.
  * If your server really moved and also revoked the token or dropped the
    request, pin it: enter it as a manual server, or press "Use" in the
    server list. A pinned server's answer counts again.

  Before this, restarting the real server for two rounds handed the pairing
  to the twin for good: the twin refused the token, the device cleared it,
  asked the twin for access, and from then on held on to the twin. And
  before that, mDNS answer order picked between the twins every round, so a
  pending request was polled on the wrong one, came back 404 and was dropped,
  and the device never collected its approval. Among servers with the same
  `self`, the one in use stays, then the first by address (string order).
* **A successful check doesn't move the token to a new address.** A twin
  with security off answers 200 to any token, and following it would tie the
  device to the twin for good. A token saved by older firmware has no address
  yet and learns it from its first successful check over plain http. Over TLS
  there is no separate check, so such a token keeps matching by `self` only.
* **A pending request survives a reboot.** Its `href` is saved with the
  server it belongs to, so a device restarted while waiting for approval goes
  on polling instead of asking again.
* **You can paste a token by hand**: `POST /api/v1/sk/token {"token": "…"}`.
  The device checks it at once.
* **The token never leaves the device.** No API response or SSE event
  contains it, and it is stored in the same NVS partition as the settings,
  encrypted if that partition is ([Security](security.md)).

## Sending values {#delta-stream-m4}

`espos_sk_publish_number/string/bool/json(path, value)` is all an
application needs. You can call it from any task; it never blocks. It works
before the network is up, but not before `espos_sk_start()`, which
`espos_start()` calls; earlier calls return `ESP_ERR_INVALID_STATE`. Values
go out for `vessels.self` with the source label `espos.<hostname>`.

**Another vessel.** `espos_sk_publish_number/string/bool/json_ctx(context,
path, value)` publish the same way for any context. That is how a logger in
a dinghy sends its depth sounder to the mothership's server as a boat of its
own. `espos_sk_vessel_context()` returns the device's own context,
`vessels.urn:mrn:signalk:uuid:<uuid>`. The UUID is made on the first
`espos_sk_start()` and kept in NVS (namespace `skstate`, key `vessel_id`),
separately from the `clientId`. It survives reboots and updates, and only
changes after an NVS erase. The status document shows it as
`vessel_context`. One batch can hold values for up to four contexts and
sends one delta per context, since a delta has room for only one. The `_ctx`
calls refuse a NULL context rather than treat it as `vessels.self`.

**These calls are for live data.** signalk-server keeps a delta's timestamp
(`src/index.ts`), but it takes the newest delta it receives as the vessel's
current state without looking at timestamps (`ingestDelta` in
`src/deltacache.ts`). Its staleness check flags old values at once, and
every plugin and client gets them as if they had just happened. Sending
hours of recorded data this way walks the vessel back through its old
positions. Recorded data goes to whoever needs it as an upload instead. The
offline buffer below is a short replay of the same kind, and that is
acceptable: it holds a limited outage (32 KiB by default), sends oldest
first, and ends on current values.

**A time of its own.** `espos_sk_publish_number/string/bool/json_ctx_at(context,
path, value, at_ms)` stamp a value with when it was measured, in unix
milliseconds. That is for a sample that had to wait on the device, through a
short dropout, or a reading the sensor timestamped itself. `at_ms` 0 is the
same as the plain `_ctx` call. The value carries that timestamp whether or
not `sk.timestamps` is on and whether or not the device knows the time.

* Within one batch, a context's values go out as one delta with an update
  per timestamp, in the order each was first published.
* A value that would join an update sent ahead of an older value of the same
  path closes the batch instead. So each path's values reach the server in
  the order they were published; values of different paths may be
  reordered. A path that switches between dated and current values costs a
  message per change, which is one more reason to finish a replay before
  current values start again.
* A time before 2000 is refused (`ESP_ERR_INVALID_ARG`); it is uptime, or
  unix seconds passed by mistake.

The warning above still applies. History stores may ignore the timestamp
too: signalk-questdb stamps rows with the time they arrive, on purpose, and
signalk-to-influxdb2 does the same by default (`useSKTimestamp`) and skips
other vessels (`onlySelf`). Nothing acknowledges a delta, and the buffer
drops its oldest messages when it is full, so a dated delta is no safer than
any other. A backlog that must arrive complete goes as an upload.

**Waiting instead of dropping.** When the buffer is full it drops its oldest
message, which is right for an offline buffer. Code forwarding a backlog it
can't afford to lose can call `espos_sk_set_no_drop(true)` instead. Then a
publish that would close the batch while the buffer is full returns
`ESP_ERR_NO_MEM` and queues nothing, so the caller waits for the stream to
drain and tries again rather than losing the start of its backlog.

* Only a publish that closes the batch is refused. While the batch is open,
  publishes return `ESP_OK` and the values wait, so `ESP_OK` doesn't mean the
  value is in the buffer yet.
* A batch too big for the buffer goes out with the normal dropping rule
  rather than being refused forever.
* Turn it off again for live values, where dropping the oldest is the right
  trade.
* It makes the caller wait; it doesn't make delivery certain. A failed send
  put back into a full buffer still drops the oldest message, and the
  ordering rules above still hold.

How a value gets to the server (`sk_delta.c` is plain C and unit-tested;
`sk_ws.c` is the task that sends):

1. **Batching** (`sk.batch_ms`, 100 ms by default). Everything published
   within one batch becomes one delta per context. A device that only
   publishes for `vessels.self` sends one message per batch. A path published
   twice in a batch keeps the last value. Numbers are written in the shortest
   form that reads back exactly. A batch closes early once it holds
   `CONFIG_ESPOS_SK_PENDING_MAX` values (32 by default, 4 to 32). Each slot
   takes about 370 bytes, so lower it on a board short of memory.
2. **Buffering** while the stream is down (`sk.buffer_msgs` / `sk.buffer_kb`,
   128 messages or 32 KiB by default). When it is full the oldest messages go
   first, counted in `ws.dropped`. Batches keep closing while offline, so a
   path's history is kept, not only its latest value.
3. **Draining** after a reconnect at `sk.drain_per_s` (20 per second by
   default), so the backlog doesn't swamp the server. New values queue behind
   it, so each path stays in order.

The stream task (`espos_skws`) runs when `sk.ws_enabled` is on, the network
is up, a server is chosen, and the token allows streaming (approved, or the
server has security off). It connects to
`ws://<host>:<port>/signalk/v1/stream?subscribe=none` with
`Authorization: Bearer <token>`, reads the hello, and then sends deltas as
text frames. A `401` on connect calls `espos_sk_report_unauthorized()`, so
the token machine checks the token again or asks again. Any other failure
waits and retries on the same backoff curve as WiFi (`ws.next_retry_s`).
Changes to the stream settings apply without a restart; `ws_enabled=false`
closes the connection and keeps buffering.

**Metadata.** `espos_sk_declare_meta(path, meta_json, period_ms)` records
metadata for a path that is not in the Signal K specification; spec paths
belong to the server ([Metadata is impossible to get wrong](#metadata-is-impossible-to-get-wrong)).
On every connect the client task asks
`GET /signalk/v1/api/vessels/self/<path>/meta`. If the server has none, the
device `PUT`s its own. If the server has some, perhaps edited by the user,
the server's wins. A `period_ms` above 0 adds `timeout` (2.5 times the
period, in seconds), the one field only the device can know.
`ws.meta.declared` and `ws.meta.reconciled` show how far it got.
A reply the device cannot read (too large, cut off, not JSON) counts as
"the server has some", never as "none", so a big user-edited meta object is
not overwritten. A call that gets no answer or a server error ends the
round, and so does a refused token; paths still open are tried again after a backoff, starting
past the path that ended the round so one failing path holds back no other (1 s,
doubling up to 5 minutes, back to the start on every new connection and
every changed declaration). Values may reach the server a moment before
their metadata on a first connect. These are blocking HTTP calls, so they run on the
client task with the token machine's and not on the stream task.

**Device health.** Every `sk.health_s` (10 s by default, 0 turns it off) the
stream task publishes
`espos.<hostname>.{uptime,freeHeap,minFreeHeap,internalFree,largestBlock,rssi,wifiReconnects,skReconnects,resetReason}`
with metadata, so a dashboard can watch the device without any application
code.

Two details of signalk-server 2.31 that cost time to find: client text
frames must have the FIN bit set (`WS_TRANSPORT_OPCODES_TEXT |
WS_TRANSPORT_OPCODES_FIN`), or the server closes the connection after the
first frame. And `subscribe=none` still sends the hello. A meta `GET` answers
`404` when there is none, and the meta `PUT` takes `{"value": {…}}`.

## Receiving values and sending PUTs {#inbound-m7}

A display or a controller needs more than a sensor does: it reads values from
the server and asks it to change things.

```c
int h = espos_sk_subscribe("navigation.*", 1000, on_update, NULL);   /* family */
espos_sk_subscribe("environment.mode", 0, on_update, NULL);           /* exact */
espos_sk_unsubscribe(h);
espos_sk_put("navigation.anchor.maxRadius", "30", on_put_done, NULL);
espos_sk_send_raw("{\"context\":\"vessels.self\",\"updates\":[…]}");
```

* **Subscriptions** are exact paths or families (`prefix.*`, `prefix*`, `*`),
  up to 48 (`ESPOS_SK_MAX_SUBS`). The stream opens with
  `subscribe=none&sendMeta=all`. After the hello, and after every reconnect,
  one frame carries all subscriptions:
  `{"context":"vessels.self","subscribe":[{"path","period","format":"delta","policy":"instant","minPeriod"}]}`.
  A new subscription while connected goes out on its own, and
  `espos_sk_unsubscribe` sends `unsubscribe` once nothing else covers the
  pattern.
* **What your callback gets.** `sk_parse.c` (plain C, cJSON) turns each frame
  into items: `path`, `value_json` (the JSON text as it came: a number,
  string, object or `null`), `timestamp`, the source (`$source` or
  `source.label`) and `context`. When the server sends metadata, there are
  also meta items, with `meta_json` set and no value. Frames are put back
  together up to `CONFIG_ESPOS_SK_RX_FRAME_MAX` (16 KiB); a larger one is
  dropped with a log line.
* **The callback runs on the stream task.** Copy what you need and return. A
  display hands the work to its UI task. Never block there, and never call an
  `espos_sk_*` function that could wait on the stream
  ([Which task calls you](concepts.md#which-task-calls-you)).
* **Sending a PUT.** `espos_sk_put` sends
  `{"context":"vessels.self","requestId":<uuid4>,"put":{"path","value"}}`. The
  answer (`state` COMPLETED or FAILED, `statusCode`, `message`) is matched by
  `requestId` and passed to your callback; without an answer in 10 s the
  callback gets `"TIMEOUT"`. Up to 8 can be waiting at once. While the stream
  is down the call returns `ESP_ERR_INVALID_STATE`, and nothing is kept for
  later: a control action must not fire minutes after someone pressed the
  button. A real server with no handler for the path answers `COMPLETED`
  with `statusCode` 405 "PUT not supported for …".
* **Raw frames** (`espos_sk_send_raw`) go out ahead of buffered deltas. One
  use is a `notifications.*` delta with `state: "normal"` that acknowledges
  an alarm.
* **Status**: `ws.in {subs, frames, received}` and
  `ws.put {pending, ok, failed}`. For scripts there is
  `POST /api/v1/sk/put {"path","value"}` (answers 202; the last answer is at
  `GET /api/v1/sk/put`).
* The example app subscribes to `app.watch_path` and logs each update.
* The server sending a PUT to this device is a separate mechanism with its
  own handlers, described next.

Tested on 2026-08-18 against signalk-server 2.31 on an ESP32-P4: 586 updates
of `navigation.*` from N2K sources in about 40 s, `satellitesInView` objects
of several KiB put back together, and a PUT round trip (405 from a server
without handlers).

## Letting the server change something {#inbound-put-control}

Everything above is the device asking the server. This is the other way
round: the server asking the device to change something, such as a switch
operated from a phone or a setpoint moved on a plotter. Without it a device
can only report.

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

What your handler returns becomes the answer:

| Handler returns | Answer |
|---|---|
| `ESP_OK` | `COMPLETED` 200 |
| `ESP_ERR_INVALID_ARG` | `COMPLETED` 400 |
| anything else | `COMPLETED` 502 |
| `ESPOS_SK_PUT_PENDING` | `PENDING` 202; you call `espos_sk_put_respond()` later |
| *(no handler for the path)* | `COMPLETED` 405 |

### Three things that are easy to get wrong

**The device must publish the path first.** signalk-server sends a PUT to a
device only for a path it has seen that connection publish, under that
source (`processUpdates` in `src/interfaces/ws.ts`). For a path the device
has never published, the server answers 405 itself and the request never
reaches the device. Publish the current state once at boot and again on
every change.

**`put` arrives as an array.** The server sends
`{"requestId","context","put":[{"path","value"}]}`, an array even for one
path. That is not the shape a client sends (`espos_sk_put` writes a single
object), and a parser that only knows the object form sees no requests at
all. espOS accepts both.

**The answer's state must be `COMPLETED` or `PENDING`.** signalk-server's
`isWsRequestReply()` accepts exactly `COMPLETED`, `PENDING` or `null` and
quietly ignores anything else. An answer with `"state":"FAILED"`, the obvious
spelling for a failure, is dropped, and the client waits out the server's
full 60 s timeout. A failure is `COMPLETED` with a 4xx or 5xx `statusCode`.

espOS answers every request, including one for a path without a handler and
one it can't make sense of. No answer is the worst answer: it costs the
client 60 s and tells it nothing.

### Flushing before sleep

```c
espos_sk_flush(2000);      /* send what is buffered, then deep-sleep */
```

Values are batched (`sk.batch_ms`), so a value published a millisecond
before `esp_deep_sleep_start()` is still waiting when the radio goes off.
`espos_sk_flush()` closes the batch and waits. It returns `ESP_OK` when
nothing is left, `ESP_ERR_TIMEOUT` when the deadline passed, and
`ESP_ERR_INVALID_STATE` when the stream is down and nothing can be sent.
Never call it from the stream task or a subscription callback.

The message being written at that moment counts as pending until the write
returns, so `ESP_OK` means everything was handed to the network stack. It
doesn't mean the server has it. On a working link the stack sends within
milliseconds, and code about to switch the radio off should give it that
moment.

## The graph nodes (`espos_sk_flow`)

`espos_sk_flow` is the Signal K end of the [data-flow graph](flow.md): the
same calls as above, as nodes.

```cmake
idf_component_register(SRCS main.cpp PRIV_REQUIRES espos_core espos_flow espos_sk_flow)
```

| Node | What it does |
|---|---|
| `sk::Output<T>(path[, Meta])` | publishes |
| `sk::Listener<T>(path)` | receives a value from the server |
| `sk::PutHandler<T>(path)` | lets the server change something here |
| `sk::PutRequest<T>(path)` | asks the server to change something |
| `sk::Notify(key, message)` | raises or clears a device condition |
| `sk::NetRssi`, `sk::IpAddress` | report what the network says about itself |

### Metadata is impossible to get wrong

**Never send metadata for a path in the Signal K specification.** The server
already knows that `navigation.speedOverGround` is in metres per second. A
device that declares it anyway can only get it wrong, and then every
dashboard on the boat is wrong.

So `Output` has no units argument. The only way to pass units is to build a
`Meta`, and building one says "this path is mine, nobody else knows what it
means", which is exactly when metadata is right:

```cpp
sk::Output<float> sog("navigation.speedOverGround");             // spec: no meta, ever
sk::Output<float> pv("sensors.solar.0.voltage", sk::Meta{"V"});  // ours: meta declared
```

The rule isn't something you have to remember; the code doesn't let you
break it.

`Output<std::optional<T>>` publishes JSON `null` when it has no value. That
means "this sensor has nothing right now", which is different from zero and
from an old value.

### Receiving

`Listener` and `PutHandler` receive on the stream task but don't send
anything on from there. Both post into a `Mailbox`, so the value continues on
the flow task like every other node's. That is why you connect from
`.out()`:

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

Publishing at the end of the chain is what makes this device the path's
source; without it the server has nowhere to send the PUT. It also publishes
what the pin actually did rather than what was asked for. The two differ
when the pin failed to switch, which is exactly when a dashboard must not
show the wrong thing.

`PutHandler` answers 200 as soon as the value is in the mailbox, not after
the chain has run. Otherwise it would have to hold the stream task until the
flow task finished, and one slow node would hold up every other frame on the
connection.

## HTTP requests to the server

`espos_sk_http.h` is the one way an application should talk HTTP to the
chosen server. One firmware once had four hand-written copies of "GET a
Signal K REST value", and two of them made the device reboot; this one
doesn't.

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

* **The reply** has `status` (0 when nothing came back), `body` (allocated,
  NUL-terminated, `""` for an empty reply), `len` and `truncated`. The call
  returns `ESP_OK` whenever a reply arrived, so a 404 or 500 counts as
  success; check `status`. `ESP_ERR_INVALID_STATE` means no server is
  chosen, `ESP_ERR_TIMEOUT` that no connection slot came free, and anything
  else is the error `esp_http_client` reported. Always call
  `espos_sk_http_resp_free()`.
* **The options** (`NULL` or all zero for the defaults): a 6 s timeout, a
  16 KiB limit on the body, `Authorization: Bearer` with the current token,
  401 and 403 reported to the token machine, and `Accept: application/json`.
  `no_auth` leaves out the token, `no_report_unauthorized` keeps a 401 away
  from the token machine, and `accept` replaces the header (`""` for none).
* **The body limit.** The body is collected as it arrives and stops at
  `max_body`. Past that, `truncated` is set and the rest is read and thrown
  away. Never parse a truncated body; treat it as "the reply was too big"
  (`espos_sk_get_value/meta` return `ESP_ERR_INVALID_SIZE`). The buffer grows
  with the reply, so the limit costs nothing for small replies.
* **The token** is read at each call and sent as a header, never in the
  URL, where it would end up in every proxy and server log. No token, no
  header, which is what a server without security expects. A 401 or 403 on a
  request that carried a token calls `espos_sk_report_unauthorized()`: the
  token machine checks the token again and, if the server really dropped the
  device, asks for access again.
* **http or https** follows the chosen server, as decided by `sk.scheme`
  ([below](#which-scheme-skscheme)). Certificates are checked against the same
  trust store as the stream. `espos_sk_url()` and `espos_sk_ws_url()` build
  the URL for code that opens its own connection, as the BLE gateway does for
  its control socket.
* **How many at once.** `CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT` (2 by default,
  1 to 8) limits the requests open at the same time, the token machine's and
  the metadata calls included. Each open request is a socket and, over TLS,
  about 20 KB of RAM; a display fetching one value per widget on a layout
  change would otherwise open dozens. A caller over the limit waits up to its
  own `timeout_ms` for a slot.
* **Which task.** The call blocks the calling task for up to `timeout_ms`
  waiting for a slot plus `timeout_ms` on the network, and uses about 2 KiB
  of its stack. Call it from a task of your own. Never call it from the
  stream task (the `espos_sk_subscribe` and `espos_sk_put` callbacks), an
  `ESPOS_EVENT` handler, a Bluetooth callback or a web request handler.

### The two crash patterns it avoids

Both crashes were reproduced on an ESP32-P4 against signalk-server, both are
inside `esp_http_client`, and both are why the helper uses **a new client for
every request and only `esp_http_client_perform()`**:

1. `esp_http_client_open()`, then `fetch_headers()`, then `read()` leaves the
   client's `cache_data_in_fetch_hdr` flag set. When the body arrives in the
   same TCP segment as the headers, which is every small Signal K reply, the
   next step hits `assert(orig_raw_data == raw_data)` in `http_on_body` and
   the device reboots. Only `perform()` clears the flag.
2. Reusing one client for several `perform()` calls (`set_url()` for each
   path, one connection for a batch) gets the same two pointers out of step
   and trips the same assert partway through (seen on `navigation.anchor.*`
   paths).

`perform()` with a new client per request avoids both, and delivers the body
through the event handler, which is also where the size limit lives. The
token machine has worked this way from the start; the metadata calls and the
public API now share the same code.

## TLS (https / wss) {#tls-https-wss}

TLS is built in by default (`CONFIG_ESPOS_SK_TLS=y`), and with `sk.scheme`
at its default `auto` the device uses it whenever the server offers it. A
boat's server rarely has a certificate from a public authority, so the
device keeps its own record of which certificate to trust.

### Which scheme (`sk.scheme`)

`auto`, `http` or `https`; `auto` by default. `http` and `https` force one.
What `auto` does depends on how the server was found:

* **Discovered.** signalk-server advertises `_signalk-https._tcp` instead of
  `_signalk-http._tcp` when its `ssl` setting is on (`src/interfaces/rest.js`),
  so the server has already said. The device looks for both; there is no
  probe.
* **Entered by hand.** Nothing was advertised, so the device sends one probe:
  `GET http://host:port/signalk` with redirects off. A `30x` pointing to an
  `https://` address means TLS, and if that address names a port, the device
  uses it (a server with SSL on often listens on another port). If nothing
  answers on the plain port, the device tries https once before deciding the
  host is down. **The probe carries no token.** The address isn't known to
  be our server yet, and handing the token to whatever answers is exactly what
  must not happen. A manual host that discovery saw within the last two intervals
  plus 5 s (picked from the server list, say) uses the scheme it advertised,
  with no probe.

  No answer on either is not an answer. The host may be off, still starting,
  or not reachable yet; on a boat the server often starts after its devices.
  So the device guesses plain http without remembering the guess, and asks
  again when the network comes up and after every attempt made on the guess.
  For the same reason a manual host isn't chosen or probed before there is a
  network at all.

The answer is stored with the chosen server (`espos_sk_server_t::tls`),
remembered per host and port, and decided again only when the choice of
server changes. Changing it needs no restart: `sk_ws.c` rebuilds its
connections when the scheme changes, so a server that gains or loses TLS is
followed straight away.

`GET /sk/status` shows what is actually in use as `server.scheme`. Under
`auto` that is the only place to see it.

### Which certificates are trusted (`sk.tls_trust`)

`tofu`, `ca` or `bundle`; `tofu` by default. **There is no "accept anything"
mode.**

* **`tofu`** means trust on first use, the way ssh does it. The first
  connection that works records what the server presented, and every later
  one must match. Certificates get renewed, so there are two kinds of record:
  * **A CA record**: the highest `CA:TRUE` certificate in the chain, plus the
    set of DNS names and IP addresses the server's certificate is for. A
    renewal signed by the same CA for the same names is accepted without
    anybody pressing anything, which is what lets a 90-day certificate work
    on a device in a locker. The names matter as well as the CA: otherwise a
    private CA that signed one host would vouch for every other name it ever
    signs.
  * **A leaf record**: the SHA-256 of the server's certificate itself, used
    when the chain has no CA or the certificate lists no names.
    signalk-server's own self-signed certificate ends up here, and a renewal
    then needs one deliberate press of "Trust the new certificate".
* **`ca`** works the same way, but you supply the CA instead of the device
  recording it. Put the issuing CA in `sk.ca_pem` (a blob, base64 over the
  REST API), or send `PUT /api/v1/sk/tls/ca {"pem": "-----BEGIN CERTIFICATE-----…"}`.
  The device trusts it immediately, so a fleet set up in advance connects on
  the first try with nothing to record. An invalid PEM is answered `400`,
  rather than leaving a device that quietly stops connecting.
* **`bundle`** uses the public Mozilla roots and records nothing, for a
  server with a certificate from a real certificate authority.

The host name check is off except in `bundle` mode. It would add nothing,
since the certificate has already been matched by fingerprint and names, and
it fails for a boat server reached by IP address, which is most of them.

### Record only after the connection works

The certificate check runs during a handshake that hasn't finished yet.
Recording the certificate at that point would let someone in the middle of a
first connection plant their own certificate and be trusted from then on.
So the check only notes the certificate in RAM, and `espos_sk_tls_commit()`
saves it to NVS once the connection has proved itself: an HTTP status from
the far end, or a WebSocket `101`. Every handshake starts by throwing away
whatever the last one noted.

The save runs on whichever task made the request. When that task's stack is
in PSRAM (the BLE gateway's can be), the NVS write goes to a short-lived task
with a stack in internal RAM instead, and so does a retried load of the
record after a failed one at boot. Reading or writing flash switches off the
cache, and PSRAM with it, so a task whose stack is in PSRAM would crash. A save
that fails is tried again on the next connection.

The record is kept in the `skstate` NVS namespace next to the token
(`tls_kind`, `tls_fp`, `tls_ca`, `tls_san`, `tls_cn`, `tls_self`, `tls_at`).
It is device state, so it doesn't travel with a settings export. `sk.ca_pem`
is a setting and does.

### When it doesn't match: `cert_error`

`token.state == "cert_error"` means the connection was refused, not the
token:

* **The token is kept.** Dropping it would mean approving the device again in
  the server's admin page after every renewal.
* **The device retries every 60 s**, not on the growing backoff an
  unreachable server gets. The fix comes from outside (the server renews, or
  someone presses the button) and should be noticed within a minute, not an
  hour.
* **The stream stays down** (`espos_sk_stream_allowed()` is false). Falling
  back to plain http would send the token to whoever answered.
* `espos_health` shows it as `skCertificate` (`ALARM`, but it doesn't restart
  the device, because a restart wouldn't bring a new certificate), and that
  also goes out as a Signal K notification ([Health](health.md#built-in-conditions)).

`GET /api/v1/sk/tls` shows the recorded certificate next to the one the
server presented. Comparing the two fingerprints tells you whether this is
your own renewal. `DELETE /api/v1/sk/tls` forgets the record and retries at
once; that is the "Trust the new certificate" button on the Signal K page.

### Why a 401 over plain http gets a second chance

Over TLS nobody on the way can fake an answer, so a `401` to a request that
carried the token comes from the server, and the token is cleared.

Over plain http that isn't so. A captive portal, a proxy or a router's own
login page all answer `401`, and throwing the token away for one of them
means approving the device in the server's admin page again. So the first
refusal over plain http only starts a second check: the device keeps the
token, asks again after 5 s, and clears it only if that is refused too. Any
`200` resets the count. (SensESP's `should_clear_token_on_status` works the
same way, for the same reason.)

### Memory

A TLS handshake needs about 20 KB of internal RAM in one piece, and that is
exactly what an ESP32 running WiFi is short of. On a board with PSRAM the
total free memory is the wrong number to look at: megabytes of PSRAM hide the
few kilobytes that matter.

* **One handshake at a time on the whole device.** Two at once is where the
  memory runs out. A caller waits up to 10 s for its turn
  (`CONFIG_ESPOS_SK_TLS_HANDSHAKE_TIMEOUT_MS`), then retries on its own
  backoff.
* **A check before starting.** If the largest free block of internal RAM is
  below 24 KB (`CONFIG_ESPOS_SK_TLS_MIN_FREE_BLOCK_KB`), the handshake waits
  and `tlsMemory` is raised as a `WARN`. Running out partway through is not a
  clean error: mbedTLS leaves a half-built session and a socket behind, and
  in a reconnect loop that starves WiFi of the same memory.
* **No separate token check on TLS servers.** The WebSocket connection
  carries the same token and refuses it just as clearly, so the check would
  only cost a second handshake per reconnect.
* `sdkconfig.d/espos.defaults` turns on mbedTLS's dynamic buffers
  (`MBEDTLS_DYNAMIC_BUFFER`, `DYNAMIC_FREE_PEER_CERT`,
  `DYNAMIC_FREE_CONFIG_DATA`) and leaves out the server side
  (`MBEDTLS_TLS_CLIENT_ONLY`). `SSL_IN_CONTENT_LEN` stays at 16384, which a
  burst of subscription data needs.

### Cost

Flash, measured on an ESP32-C6 as the size with `CONFIG_ESPOS_SK_TLS=y`
minus the size with `=n`, same source:

| Build | y | n | Cost |
|---|---|---|---|
| with `espos_ota` (the reference app) | 1 485 308 | 1 475 280 | **about 10 KB** |
| without it (the `tls_server` example) | 1 437 772 | 1 358 004 | **about 78 KB** |

A firmware with `espos_ota` already includes mbedTLS and the certificate
bundle for https updates, so all TLS adds here is the trust store, the
connections and the check. Without it, most of the 78 KB is the Mozilla
bundle, which the trust store still needs for `bundle` mode.

RAM: about 20 KB while a connection is open. The dynamic-buffer settings
above cost about 17 KB of flash in both columns, because they are in
`sdkconfig.d/espos.defaults` and apply to the update client too; they are
what gives the RAM back between handshakes.

### Trying it against a real server

Turn SSL on in signalk-server (Server → Settings → SSL, or `"ssl": true` in
`settings.json`) and restart it. It makes a self-signed certificate and
advertises `_signalk-https._tcp`.

1. The device's Signal K page should show the server with scheme `https`
   and, after the first connection, a recorded **certificate** (a leaf
   record, since a generated self-signed certificate has no CA and often no
   names).
2. Delete signalk-server's certificate and restart it, so it makes a new
   one. Within a minute the device shows `cert_error`, keeps its token, and
   the page shows the recorded fingerprint next to the new one.
3. Press **Trust the new certificate**. The device reconnects within a few
   seconds.
4. For the CA path, issue the server's certificate from a CA of your own and
   put that CA in `sk.ca_pem`. Re-issuing the certificate for the same names
   should interrupt nothing; re-issuing it for a different name should give
   `cert_error` with "names different hosts".

## Task stacks

`espos_sk` has two tasks, each with a Kconfig option and measurements behind
its default:

* **`CONFIG_ESPOS_SK_TASK_STACK`** (12288 by default, 6144 to 16384), the
  client task: discovery, the token machine and its HTTP calls (access
  request, polling, checking), the https probe, and the metadata calls,
  which send an HTTP GET and PUT per path (`espos_sk_ws_meta_round()`).
* **`CONFIG_ESPOS_SK_WS_TASK_STACK`** (10240 by default, 8192 to 16384), the
  stream task: putting frames together, parsing deltas, the server's PUTs
  and the notification sink.

The measurements below were taken with the metadata calls on the stream
task. The client task was not re-measured with them.

Measured on an ESP32-C5 running the BLE gateway against a live server
(discovery, an approved token, metadata for the published paths), with
`CONFIG_FREERTOS_USE_TRACE_FACILITY=y` and `uxTaskGetStackHighWaterMark()`:

| Task | Allocated | Most used | Never touched |
|---|---|---|---|
| `espos_sk` | 12288 | 1488 | 10800 (88 %) |
| `espos_skws` | 8192 | 5272 | 2920 (36 %) |

A second run on the same kind of board, 112 minutes with the gateway's
control WebSocket on, REST requests every 10 minutes and an update attempt
every 30 (still plain http and ws):

| Task | Allocated | Most used | Never touched |
|---|---|---|---|
| `espos_sk` | 12288 | 4280 | 8008 (65 %) |
| `espos_skws` | 8192 | 6904 | 1288 (16 %) |

With nothing but a busier server, the stream task used 1.6 KB more, which
left 1.3 KB before a stack overflow and a reboot loop, and that without any
TLS. So its default is now 10240. The client task's peak nearly tripled too,
which is why its default wasn't lowered either.

**Neither run is a reason to lower a default.** That server was plain http on
port 80, so neither task ever went through a TLS handshake, and mbedTLS is
the deepest code either of them runs. Plenty of room left over with the
deepest path never taken doesn't show the stack is too big; it shows the
measurement missed something. The numbers are a floor, not a budget. Before
lowering either default, measure:

* with `CONFIG_ESPOS_SK_TLS=y` (the default), against a `wss` server, so the
  token calls and the metadata calls actually go through mbedTLS;
* with `CONFIG_ESPOS_SK_TLS=n`, where the device can only speak http and ws,
  the heaviest load that build has: a reconnect that replays a full burst of
  subscriptions and sends metadata for every published path.

The smallest allowed values are 6144 for the client task and 8192 for the
stream task, the nearest round sizes above what each was measured using
(4280 and 6904 bytes). A range that lets a build crash on the first large
frame would be a trap, not a choice: a stack overflow here is a reboot loop,
not a slower device.

## API

* `GET /api/v1/sk/status`: token, server and discovery status, plus the `ws`
  stream object ([REST API](rest-api.md)).
* `POST /api/v1/sk/put {"path","value"}` and `GET /api/v1/sk/put`: a PUT over
  the stream, and the last answer.
* `POST /api/v1/sk/publish {"path","value"[,"meta","period_ms"]}`: publish a
  value over HTTP.
* `GET /api/v1/sk/servers`: the discovered servers, with a `selected` flag.
* `POST /api/v1/sk/discover`: run a discovery round now.
* `POST /api/v1/sk/request`: ask for access again (from denied, error or
  open).
* `POST /api/v1/sk/token {"token"}`: set a token by hand.
* `POST /api/v1/sk/forget`: drop the token and start over. A request still
  waiting for approval is kept and polled, because the server keeps it anyway
  and refuses duplicates.
* `GET /api/v1/sk/tls`: the recorded certificate next to the one the server
  last presented, and why they don't match if they don't.
* `DELETE /api/v1/sk/tls`: forget the recorded certificate and retry now.
* `PUT /api/v1/sk/tls/ca {"pem"}`: supply the issuing CA (switches
  `sk.tls_trust` to `ca`).
* SSE events: `sk`, `sk_servers`, `sk_ws` (the `ws` object on every stream
  change) and `sk_tls` (the `/sk/tls` document when a client connects and
  whenever the record changes).

## Testing

* `test/host/espos_sk_test` has Unity tests for the token state machine, the
  token store, server selection, the TLS rules, delta batching, buffering and
  draining, the frame parser and path patterns.
* `test/host/espos_httpd_test` runs the real HTTP client and WebSocket
  against a Python stand-in for signalk-server's security API and stream.
  `SkTests` covers approve, deny, revoke, forget, security off, a pasted
  token, a manual host, deltas with metadata, and offline buffering with
  ordered draining. It also drives the HTTP helper: a 200 with body and
  Bearer header, a 404 as a reply, an oversized body coming back
  `truncated`, value, meta and URL lookups, a PUT through the same code, and
  a 401 sending the token machine to ask again. `SkInboundTests` covers
  subscribe frames, value and meta delivery, exact against family matching,
  a PUT round trip, failure and timeout, raw frames, unsubscribing and
  resubscribing after a reconnect, and putting a 9 KiB frame back together.
* Against a real signalk-server on your computer: run
  `node bin/signalk-server -c <fresh config dir>` from a checkout, call
  `POST /skServer/enableSecurity {"userId","password","type":"admin"}`,
  restart, then approve with
  `PUT /skServer/security/access/requests/<clientId>/approved`, using the
  admin cookie from `POST /signalk/v1/auth/login`. Revoke with
  `DELETE /skServer/security/devices/<clientId>`.

## Notifications

`espos_sk_notify(key, state, message)` raises or clears a Signal K
notification at `notifications.espos.<label>.<key>`:

```c
espos_sk_notify("wakeService", ESPOS_SK_ALERT_WARN, "wake service unreachable");
espos_sk_notify("wakeService", ESPOS_SK_ALERT_NORMAL, "");   /* cleared */
```

Use it for things the device knows about and the crew would want to see: low
memory, an overheating chip, a service the firmware depends on that went
away. Without a notification, these look like a device that has quietly
stopped doing its job, which is indistinguishable from a hardware fault and
expensive to track down.

It is a thin layer over [`espos_health`](health.md). With
`CONFIG_ESPOS_SK_NOTIFICATIONS` on (the default), `espos_sk`'s sink publishes
every health condition, whoever raised it, and `espos_sk_notify()` calls
`espos_health_report()`. It returns `ESP_ERR_INVALID_STATE` when that sink
isn't running (Signal K not started, or the sink couldn't register), and
`ESP_ERR_NOT_SUPPORTED` in a build with the option off. So:

* **Reporting the same thing twice is harmless.** The same state and message
  again sends nothing, so you can report on every poll. The first report
  after boot always goes out, even a `NORMAL`, because the server may still
  hold an alert from before the restart.
* **`key` is a short, fixed name** (`lowMemory`, `wakeService`). It becomes
  part of the path, and rules and dashboards match on the path. The
  `message` is for people and can change freely.
* **`method`** is `["visual"]` for a warning or alarm and `[]` when cleared.
  What to do about it is up to the server, not the device.
* **The number of keys** is limited by `CONFIG_ESPOS_HEALTH_MAX_CONDITIONS`
  (12 by default), the size of the health table, shared with every other
  condition. A key too many gets `ESP_ERR_NO_MEM`. A key or message that is
  too long is refused with `ESP_ERR_INVALID_SIZE` rather than cut short,
  because a shortened key would never match the next call. While offline,
  notifications are buffered like any other delta.
* **Every connect restates every condition**, cleared ones included, and so
  does a config reload while connected (a rename moves the paths). The
  offline buffer drops its oldest deltas when it fills, and a server restart
  forgets every notification, so without this a lost clear would leave a
  warning on the server for good, and a warning that is still true would
  vanish until it next changed.
* **Nothing clears the notifications of a device that is gone.** A device
  that is switched off for good, or renamed, which changes `<label>`, leaves
  its last state on the server. Signal K keeps notifications in memory only,
  so a server restart removes them.

espOS raises `lowMemory` and its other conditions through `espos_health`,
not through Signal K, so they exist on a device without Signal K too. The
list, the thresholds and the restart rules are in [Health](health.md).
