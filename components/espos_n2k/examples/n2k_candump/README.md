# n2k_candump

An NMEA 2000 → Signal K gateway. Reads CAN frames off the boat's bus and
streams them over TCP in candump ASCII, which
[signalk-server](https://github.com/SignalK/signalk-server) reads with
canboatjs.

The interesting part of the application:

```cpp
static espos_n2k::TwaiReceiver rx({.tx_pin = GPIO_NUM_20, .rx_pin = GPIO_NUM_21,
                                   .bitrate = 250000});
static espos_n2k::TwaiTransmitter tx;
rx.start();
tx.start();

static espos_n2k::CandumpTcpServer server(&rx, &tx, {});
server.start();
```

## The gateway decodes nothing

No PGN parsing happens on the device. That is deliberate: PGN decoding changes
far more often than firmware should, and canboat already does it well, so the
server owns it and this stays a pipe. The device contributes the thing a Pi
below decks cannot — a galvanically sane tap on the backbone, on mains-free
power, wherever the cable happens to run.

## Adding it to signalk-server

Server → Connections → Add, then:

* type **NMEA 2000**, source **IP gateway** (`n2k-ip-gateway-canboatjs`)
* format **candump3**
* host: the device's address, port **2599**
  (`CONFIG_ESPOS_N2K_CANDUMP_PORT`)

The device also advertises `_sensesp-n2k._tcp` over mDNS, so it is findable
without typing an address.

## Wiring is the part that goes wrong

Two things are not optional, and both fail in a way that looks exactly like a
software fault:

**A transceiver.** The ESP32's TWAI peripheral is TTL and cannot drive a
differential pair. You need an SN65HVD230, a TJA1051 or similar between the
GPIOs and CAN_H/CAN_L.

**Termination.** 120 Ω at each end of the bus. On a boat the backbone is
already terminated at both ends and a drop cable adds none — do not add a
third. On a bench with two devices and no terminators, nothing is received.

**The pins in `main.cpp` are for the Waveshare ESP32-P4 boards.** There is no
sensible default: which GPIOs carry CAN is a property of the hardware, and a
pin number that exists on one target does not on another. `espos_n2k` refuses
to guess — an unset pin leaves the receiver unstarted with one clear log line
rather than binding whatever GPIO 22 means on this chip.

**250 kbit/s is not negotiable.** A device at the wrong bitrate does not merely
miss frames; it corrupts them for every other device on the bus by
acknowledging at the wrong moment.

## Telling the failures apart

`GET /api/v1/n2k` distinguishes the three states that all look like "no data",
without a serial cable:

| | |
|---|---|
| `frames: 0`, `errors: 0` | electrically quiet: nothing is transmitting, or you are not on the bus at all |
| `frames: 0`, `errors` climbing | you are on the bus and cannot understand it — wrong bitrate, or a broken pair |
| `frames` climbing | it works |

That distinction is why the endpoint exists: the first needs a cable checked,
the second a configuration checked, and they are indistinguishable from the
outside.

A frozen `stuff_err` count with frames still arriving is normal — it is the
signature of a cable having been reconnected on a live bus, not an ongoing
fault.

## Transmit

Passing the transmitter to `CandumpTcpServer` makes the gateway
**bidirectional**: a candump line arriving from the server is written to the
bus. Pass `nullptr` instead for a read-only gateway, which is worth doing on a
boat where nothing should originate frames from this device.

## Verified

The component runs on the author's boat on a bus with 47 devices: 292 million
frames, 0 dropped, 0 bus-off. Bus-off *recovery* has never been exercised
there, because a gateway that only listens never transmits and bus-off
requires transmit failures.
