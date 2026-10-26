# NMEA 2000 gateway

`espos_n2k` bridges an NMEA 2000 (CAN) bus to the network: a TWAI
receiver and transmitter, plus a TCP server that streams frames in
**candump** format so the bus is reachable from a laptop or a SignalK
server.

Frames are `espos_n2k::CanMessage` — espOS's own small struct, not the
driver's. That is what lets the candump codec be compiled and tested on the
host, and what keeps an IDF driver change from changing the type every
consumer names.

It is board-agnostic. The application chooses the pins and bitrate;
nothing here assumes a particular panel or transceiver.

```cpp
#include "espos_n2k/twai_receiver.h"
#include "espos_n2k/twai_transmitter.h"
#include "espos_n2k/candump_tcp_server.h"

static espos_n2k::TwaiReceiver rx({
    .tx_pin = GPIO_NUM_22, .rx_pin = GPIO_NUM_21, .bitrate = 250000});
static espos_n2k::TwaiTransmitter tx;
static espos_n2k::CandumpTcpServer srv(&rx, &tx, {.port = 2599});

rx.start();    // configures the bus: pins, bitrate
tx.start();    // joins the bus the receiver configured
srv.start();   // installs its own frame callback on the receiver
```

**Start the receiver first.** IDF 6's esp_twai allocates a *node* and hands
back a handle, where the old driver was a process-wide singleton any caller
could reach. The receiver owns that node — it is the one with the pins and
the bitrate — and the transmitter joins it. A transmitter started on its own
logs that the bus is not up and stays stopped, rather than dropping every
frame in silence the way it used to.

One receiver feeds several consumers. The server adds a listener of its own
when it starts, and anything else on the bus — an [NMEA 2000 node](#the-device-as-an-nmea-2000-node),
the application — adds its own with `TwaiReceiver::add_listener()`, before or
after, without taking frames from the others. There are
`CONFIG_ESPOS_N2K_MAX_LISTENERS` slots (4 by default).

N2K is 250 kbit/s; the transceiver (SN65HVD230 or similar) is the
board's business, not this component's.

## When the bus is silent

`GET /api/v1/n2k` (register it with `espos_n2k_api_register(&receiver)`) is
the answer to "the candump socket connects and nothing arrives", which
otherwise needs a serial cable to diagnose:

```json
{ "present": true, "running": true, "ever_received": true, "idle_s": 0,
  "frames": 89559, "dropped": 0, "errors": 59, "bus_off": 0,
  "last_error": { "flags": 8, "stuff_err": true, ... } }
```

| Reading | Means |
|---|---|
| `running: false` | the driver never came up — pins, or a failed `twai_new_node_onchip` |
| `frames: 0` **and** `errors: 0` | the wire is electrically quiet: unplugged, unpowered, nobody transmitting — or the driver missed the bus, see below |
| `frames: 0`, `errors` climbing | the bus is live and not understood. The flags are symptoms, not proof: `ack_err` alone (no node acknowledged the frame) usually means nothing else is listening; repeated `stuff_err`/`form_err` point at a bitrate or wiring mismatch |
| `dropped` climbing | frames arrive faster than they are consumed; raise `CONFIG_ESPOS_N2K_RX_QUEUE_DEPTH` |

**A bus connected after boot is not picked up until the device restarts.** A
device powered before its network stays deaf, and does not recover on its own
however long you wait. Whether that is IDF's TWAI driver or this component's
`start()` path is unproven; the evidence is in
[#15](https://github.com/signalk-espOS/espOS/issues/15).

This is the common case rather than an edge one: a device is routinely powered
before the network it listens to. **So on a silent bus, restart the device
before reaching for a multimeter** — and note that nothing raises an alarm for
it, deliberately, because a firmware may legitimately run with no N2K at all.

## Consuming the stream

The server advertises `_sensesp-n2k._tcp` over mDNS with
`format=candump3`, which SignalK's `n2k-ip-gateway-canboatjs` source
browses for. From a laptop the raw stream is readable directly:

```sh
nc <device> 2599
```

**The service type and the `model` TXT tag still say `sensesp-n2k`.**
They are on the wire and existing clients already browse for them;
renaming would make every deployed gateway invisible to every deployed
client, which is not worth tidiness.

## Migrating from the `driver/twai.h` version

* `TwaiMessage` is now `CanMessage`; `espos_n2k/twai_message.h` still
  defines the old name as an alias, so code that only *names* the type is
  unaffected. Code that reaches inside it is not: `identifier` →&nbsp;`id`,
  `data_length_code` →&nbsp;`dlc`, `extd` →&nbsp;`extended`, `rtr`
  →&nbsp;`remote`. The driver-only flags (`ss`, `self`, `dlc_non_comp`) are
  gone — they were a union's worth of bits nothing here ever set meaningfully.
* `TwaiTransmitter` no longer runs a task or a queue of its own: esp_twai
  queues internally, so `set()` hands the frame straight to the driver. It
  is still non-blocking and still drops (and counts) when the queue is full.
  `ever_transmitted()` now means "ever queued".
* Bus-off recovery moved from a poll in the RX loop to the driver's
  state-change callback, so it no longer waits for a receive timeout.

## Callbacks

`TwaiReceiver::add_listener()` takes a plain `std::function`, called on the
receiver's own task for every frame, after the listeners added before it. It
returns a handle for `remove_listener()`, which returns only once the
listener is not running and will not run again, so what it captured can then
be destroyed. Do not do slow work in a listener and do not touch UI state
directly — copy what you need and hand it to the task that owns it: every
other listener on the bus waits behind yours. Do not add or remove listeners
from inside one.

`set_on_frame()`, the single-callback API this class had before, still
works: it replaces the listener the previous `set_on_frame()` installed and
leaves the others alone. Before, it replaced the candump server's callback
too, which is why a firmware could not run its own NMEA 2000 code and the
server on one bus.

The predecessor to this component inherited a SensESP observable base class
for the same job; the callback is the whole reason this code no longer needs
SensESP at all.

## The device as an NMEA 2000 node

Everything above is a bridge: frames go through with whatever source address
they carry, and the NMEA 2000 identity belongs to whatever drives the bridge
(canboatjs on the Signal K side). A device that transmits *as itself* — a
switch bank an MFD can switch, a sensor that shows up in the MFD's device
list — has to be a node: a NAME, the ISO address claim and its contest,
answers to ISO requests, product information, the PGN lists, a heartbeat and
the device and system instances.

`espos_n2k::Node` (`espos_n2k/node.h`, built with `CONFIG_ESPOS_N2K_NODE=y`)
is that node. The protocol is Timo Lappalainen's
[NMEA2000](https://github.com/ttlappalainen/NMEA2000) library (MIT), vendored
under `components/espos_n2k/third_party/NMEA2000/`; `Node` gives it the
receiver and transmitter, a task of its own, the settings and the stored
address.

```cpp
#include "espos_n2k/node.h"
#include "N2kMessages.h"

static espos_n2k::TwaiReceiver rx({.tx_pin = GPIO_NUM_20, .rx_pin = GPIO_NUM_21});
static espos_n2k::TwaiTransmitter tx;
rx.start();
tx.start();

static const unsigned long kTx[] = {127501, 0}, kRx[] = {127502, 0};
espos_n2k::NodeConfig cfg;
cfg.device_class = 30;        // Electrical Distribution
cfg.device_function = 140;    // Load Controller
cfg.model_id = "My switch bank";
cfg.transmit_pgns = kTx;
cfg.receive_pgns = kRx;
static espos_n2k::Node node(&rx, &tx, cfg);

node.add_listener([](const tN2kMsg& msg) {   // on the node task
  if (msg.PGN == 127502) { /* ParseN2kSwitchbankControl(...) */ }
});
ESP_ERROR_CHECK(node.start());                // after espos_start()

tN2kMsg msg;
SetN2kBinaryStatus(msg, node.device_instance(), status);
node.send(msg);                               // from any task
```

[`n2k_switch_bank`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_n2k/examples/n2k_switch_bank)
is the whole of it: eight channels on PGN 127501/127502, with the candump
server on the same bus.

What `Node` does and what it leaves to you:

| | |
|---|---|
| **NAME** | Device class and function, manufacturer code (2046 by default, the code for uncertified devices), industry group from `NodeConfig`; the 21-bit unique number from the base MAC unless `NodeConfig::unique_number` says otherwise. |
| **Address** | Claims `NodeConfig::preferred_address` on first boot, then whatever it held last (stored in NVS, namespace `espos_n2k`). A node with a higher-priority NAME wins the address; the library moves on to a free one, and `address()` follows. |
| **Instances** | `n2k.device_instance` and `n2k.system_instance` (settings, below). An MFD that sets them through a group function changes the settings too, so the value survives a reboot and shows in the web UI. A change from either side is announced with a fresh address claim. |
| **Product information** | `model_id`, `model_version`, `product_code` and the load equivalency from `NodeConfig`; the software version is the app's (`esp_app_desc_t::version`), the serial code the base MAC. |
| **Answers** | ISO requests for the address claim, product and configuration information and the PGN lists, the heartbeat (60 s), and the group functions for them. |
| **Your PGNs** | `send()` from any task: it queues (`CONFIG_ESPOS_N2K_NODE_TX_QUEUE`) and the node task sends once the address is held. Listeners receive messages assembled, a fast-packet PGN once and whole — for the PGNs the library knows as fast packets; others go to `library().ExtendFastPacketMessages()` before `start()`. `N2kMessages.h` has encoders and decoders for most standard PGNs. |

It costs about 28 KB of flash over a bridge-only build (`libespos_n2k.a`
is 8.8 KB in `n2k_candump` and 36.8 KB in `n2k_switch_bank`, esp32s3) and,
by estimate, about 13 KB of RAM: the node task's 4 KB stack, the frame and
send queues (3 KB), 12 fast-packet assembly buffers (3 KB,
`CONFIG_ESPOS_N2K_NODE_FAST_PACKETS`) and the library's other buffers. Off, none of it is compiled and
the settings page does not appear.

Settings, namespace `n2k` (the "NMEA 2000" page; only with the node built):

| Key | Default | |
|---|---|---|
| `device_instance` | 0 | tells this device from others of the same type (0–255) |
| `system_instance` | 0 | which of several separate systems on one bus (0–15) |

### Several devices on one bus

A physical device sometimes has to look like several N2K devices: three tank
senders in one enclosure, or a bank of channels with independent identities.
`espos_n2k::MultiNode` (`espos_n2k/multi_node.h`) is the multi-device
counterpart of `Node`. It shares one receiver, transmitter and task, but
claims a NAME, product information and PGN lists for each `DeviceConfig`.
Device `i` uses `preferred_address + i`.

```cpp
#include "espos_n2k/multi_node.h"
#include "N2kMessages.h"

static const unsigned long kTx[] = {127505, 0};
static const espos_n2k::DeviceConfig kDevices[] = {
    {.device_class = 75, .device_function = 150,
     .unique_number = 2093736, .device_instance = 0,
     .model_id = "Starboard Blackwater", .model_version = "1.0.0",
     .serial_code = "8", .product_code = 104, .load_equivalency = 0xff,
     .transmit_pgns = kTx},
    {.device_class = 75, .device_function = 150,
     .unique_number = 2093745, .device_instance = 1,
     .model_id = "Starboard Aft Freshwater", .model_version = "1.0.0",
     .serial_code = "5", .product_code = 104, .load_equivalency = 0xff,
     .transmit_pgns = kTx},
};
static const espos_n2k::MultiNodeConfig kMulti{
    .devices = kDevices, .device_count = 2, .preferred_address = 85};
static espos_n2k::MultiNode multi(&rx, &tx, kMulti);
ESP_ERROR_CHECK(multi.start());

tN2kMsg msg;
SetN2kFluidLevel(msg, /*instance=*/0, N2kft_BlackWater,
                 /*level percent=*/50.0, /*capacity litres=*/80.0);
multi.send(0, msg);          // device 0 speaks with source 85
```

Unlike `Node`, `MultiNode` does not persist addresses or expose the settings
page; the identities are fixed by the firmware. It also has no listener API
yet, and the stock NMEA2000 library has no per-device configuration
information, so installation descriptions are left at the library default.

### Node and bridge on one bus

A node and a candump server can share the receiver and transmitter: two
logical devices on one physical interface, which NMEA 2000 allows. The node's
frames carry its own address; the server's clients send with whatever address
*their* node claimed (canboatjs's, typically). The one combination that must
not happen is a client frame with the node's address — on the wire it is the
node speaking. So:

```cpp
static espos_n2k::CandumpTcpServer server(&rx, &tx, {});
server.set_tx_filter(node.tx_filter());   // before start()
server.start();
```

drops those frames and counts them (`tx_filtered()`). Address claims are the
exception and always pass: a client's claim for the node's address is a
contest, settled by NAME like any other.

That only works if each side sees the other, and CAN does not echo a node's
frames back to it: the controller never reports what this device itself
sends. So both directions are looped back to the receiver's other listeners
(`TwaiReceiver::loopback()`), the way Linux socketcan loops local frames back.
The node sees the frames candump clients send — their address claims above
all — and candump clients see the node's PGNs, so signalk-server reading this
device sees the node like any other device on the bus.
