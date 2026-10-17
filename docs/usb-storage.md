# USB storage bridge

`espos_usb_storage` lets an ESP32-P4 (or S3) stand in for a memory card. Its
microSD card is exported over USB device mode, so a chartplotter, a PC or any
other USB host sees an ordinary storage device and writes to it as usual.
Meanwhile espOS follows the files the host writes and hands each new run of
records to your code, which uploads it once the network is in reach.

The use it was built for: a Raymarine Axiom records RealBathy soundings to a
card (`realbathy/<id>.trl`) and has no WiFi to get them off the boat. In
LightHouse the board is a second card on the plotter's USB port, chosen as the
place to save to. espOS sends the new records to the mothership's Signal K
server whenever it is in range.

```cpp
#include "espos_usb_storage/usb_storage.h"

espos_usb_storage::Config cfg = {};
cfg.sd = {.clk = 43, .cmd = 44, .d0 = 39, .d1 = 40, .d2 = 41, .d3 = 42,
          .slot = 0, .freq_khz = 40000, .ldo_chan = 4};
ESP_ERROR_CHECK(espos_usb_storage::start(cfg));

espos_usb_storage::FollowConfig follow;
follow.follower.watches = {{"realbathy", ".trl", 24}};
follow.sink = [](const espos_usb_storage::Chunk& c) {
  return upload(c.path, c.offset, c.data, c.len);  // true once the server has it
};
ESP_ERROR_CHECK(espos_usb_storage::follow(std::move(follow)));
```

## Who owns the card

**The USB host does, for as long as the card is exported.** Two FAT drivers
writing one volume corrupt it, and the host's driver caches the FAT and
directory entries in its own RAM, where espOS cannot see them. So espOS never
writes to the card and never mounts it read-write:

* esp_tinyusb's automatic remount is switched off. By default it mounts the
  card read-write for the application whenever the host goes away, and formats
  a card it finds no filesystem on. Either would leave two writers on one FAT
  the moment the host comes back.
* Progress is kept in NVS, not on the card.
* The read path is FatFs behind a disk driver that refuses every write.

## Reading while the host owns it

Every block the host writes passes through espOS on its way to the card.
esp_tinyusb commits those writes with `sdmmc_write_sectors()` from its own
task, deferred from the SCSI command, so no TinyUSB callback marks when the card
changes. The component therefore wraps that function at link time
(`-Wl,--wrap=sdmmc_write_sectors`, `src/card_lock.cpp`). Writes to any other
card pass straight through. A write to the exported card takes a lock and
bumps a write counter before it lands, and espOS's own sector reads take the
same lock. So a read never reaches the card while it is still programming a
host write.

A pass runs only after the host has been quiet for `quiet_ms` (15 s by
default). It mounts a fresh read-only FAT view, so nothing cached from an
earlier pass survives. It checks the counter after every read and before
every hand-off. If the host wrote, the pass discards what it read and tries
again later. espOS never hands on a byte from a moment when the volume was
changing.

A host flushes a file's directory entry and its data in no guaranteed order,
so the newest tail of a file can still be unwritten sectors when its new size
is already visible. A size must therefore stay the same for `settle_ms`
(30 s) before bytes up to it are read.

## What gets handed off

The follower assumes **append-only files of fixed-size records**, which is
what a RealBathy track is (24 bytes: position, depth, time).

* Only whole records are handed off. A partial record at the end waits for
  the rest.
* A chunk is at most `max_chunk` bytes (16 KB), and files go in name order.
* A chunk counts as delivered when the sink returns true. On false, progress
  stays where it was and the pass ends, so the same bytes are offered again.
  Out of WiFi range is the normal case for this, and nothing is lost.
* Progress reaches NVS every `save_every` bytes (256 KB) and at the end of
  each file, not after every chunk, because each save is a flash write. A
  pass stops if a save fails.
* A file deleted and recorded anew under the same name is recognised by a
  CRC of its first 32 bytes and starts again from zero, with
  `Chunk::restarted` set. A file that shrank is treated the same way.
* Progress for files that are gone is pruned, but only after an undisturbed
  listing.

A crash before progress is saved offers the unsaved chunks again after
reboot, so make the receiving end idempotent on `(path, offset)`.

## Hardware

| Board | Card | USB to the host |
|---|---|---|
| Waveshare ESP32-P4 PoE-ETH | TF slot, slot 0: CLK 43, CMD 44, D0–D3 39–42, LDO channel 4 | USB 2.0 OTG high-speed, the 4-pin header J3 |
| Waveshare ESP32-P4 Touch-LCD 7B | the same pins | USB 2.0 OTG high-speed, Type-A (needs an A-to-A cable) |

The PoE-ETH's USB-C port goes only to its CH343 serial bridge, so a host
plugged in there sees a serial port and no card. The host connects to J3
instead, with a plain USB-A lead: pin 1 5 V, 2 D−, 3 D+, 4 GND. Pin 1 feeds
the board's 5 V rail ahead of the regulator, and a diode keeps it from the
USB-C port; with a PC on the USB-C for the console at the same time, leave
J3's 5 V wire unconnected.

On the P4 the SDMMC controller's slot 1 is the SDIO link to the C6 radio.
espOS brings that up first, and `start()` accepts the controller already
running.

The card must be FAT32. Raymarine plotters do not read exFAT, and espOS's
reader is FatFs. Format it in the plotter or on a PC: espOS never formats it.

**Power.** A plotter's USB port is specified for about 500 mA. A headless P4
with WiFi through the C6 peaks at roughly 300–400 mA, so it can run from the
port, but a panel with its backlight on cannot. A brown-out restarts the
board, and the plotter sees the card vanish. On the PoE-ETH never connect PoE
and USB power at the same time.

## Configuration

The app needs these in its `sdkconfig.defaults`:

```
# esp_tinyusb compiles no MSC class otherwise
CONFIG_TINYUSB_MSC_ENABLED=y
# realbathy/ is longer than 8.3; the build fails without long names
CONFIG_FATFS_LFN_HEAP=y
```

The host shows esp_tinyusb's SCSI inquiry strings. The USB product string is
`CONFIG_TINYUSB_DESC_PRODUCT_STRING`.

With the espOS prologue, name the component, since it is optional:
`espos_project_prologue(NAME ... COMPONENTS espos_usb_storage)`.

## Limits

* espOS ships no upload sink: the app passes one to `follow()` and decides
  where the chunks go.
* Only files that grow by appending are followed. A file rewritten in place
  (RealBathy's `.trk` tiles) is re-read only when its first 32 bytes change
  or it shrinks; other rewrites go unnoticed.
