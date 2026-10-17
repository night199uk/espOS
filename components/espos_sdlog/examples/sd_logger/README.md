# sd_logger

A row a second onto a microSD card, in sessions that survive a power cut. At
boot it mounts the card, closes whatever session the last power cut left open
(cutting off the half row and noting it in the close record), lists what is on
the card, and starts a new session. A session is an hour of rows.

Built in CI for the ESP32-C6 and the ESP32-P4.

## Wiring

On the Waveshare ESP32-P4 boards the card slot is on the board: SDMMC slot 0,
four lines, with its I/O powered by the P4's LDO 4
(`ESPOS_SDLOG_CARD_WAVESHARE_P4()`). Every other chip uses SPI, through a
microSD breakout or the slot of a board such as the LilyGO T-CAN485:

| Card | ESP32-C6 | ESP32-S3 | ESP32 |
|---|---|---|---|
| MOSI (DI, CMD) | GPIO 7 | GPIO 11 | GPIO 23 |
| MISO (DO, D0) | GPIO 2 | GPIO 13 | GPIO 19 |
| SCLK (CLK) | GPIO 6 | GPIO 12 | GPIO 18 |
| CS (D3) | GPIO 10 | GPIO 10 | GPIO 5 |
| VCC | 3V3 | 3V3 | 3V3 |

The pins are in `card_cfg()` in `main/main.c`; a board with its own slot
names its pins in its schematic. The card must be FAT32; cards over
32 GB come as exFAT, which ESP-IDF cannot read. The example never formats one.

What happens on the card, and how a firmware delivers a session to a server
and marks it synced, is in [docs/sdlog.md](../../../../docs/sdlog.md).
