# usb_storage_follow

The board's microSD card shows up on a chartplotter as a second USB card, and
every new RealBathy record the plotter writes there is handed to a sink once
the plotter has gone quiet. espOS never writes the card: it belongs to the
plotter. Details: [docs/usb-storage.md](../../../../docs/usb-storage.md).

Built for the **Waveshare ESP32-P4 PoE-ETH**, whose TF slot the Touch-LCD
panels share (pins at the top of `main/main.cpp`). It also builds for the
ESP32-S3; USB mass storage needs an OTG port, so those are the only two
targets. The entry point is C++, because the follower API is.

## Wiring

The plotter goes on the board's high-speed USB port. On the PoE-ETH that is
the 4-pin header J3, not the USB-C: the USB-C reaches only the serial bridge.
A USB-A lead does it: pin 1 5 V, 2 D−, 3 D+, 4 GND. The 5 V powers the board
from the plotter. Never connect PoE at the same time.

The card must be FAT32, formatted in the plotter or on a PC.

## The sink

The sink here only logs. A real firmware uploads the chunk and returns true
only once the server has it; returning false keeps the bytes for the next
pass, which is how a boat out of WiFi range loses nothing.

## Checking it

1. Plug the board into the plotter. The plotter shows a second card, and the
   log says `card exported over USB (high speed)`.
2. Pick that card as RealBathy's save location and record for a while.
3. About a minute after the plotter stops writing (15 s quiet, 30 s
   settled, polled every 10 s), the log shows `realbathy/<id>.trl: N records from byte M`.
