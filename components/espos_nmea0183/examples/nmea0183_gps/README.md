# nmea0183_gps

A GPS module on a UART, published to Signal K: `navigation.position`,
`navigation.courseOverGroundTrue`, `navigation.speedOverGround`,
`navigation.gnss.*` and `navigation.datetime`. A fix also sets the device's
clock, so its deltas carry GPS time on a boat with no internet.

Built in CI for the ESP32-C6 and the ESP32-P4.

## Wiring

| GPS module | ESP32-C6 | ESP32-P4 | ESP32-S3 | ESP32 |
|---|---|---|---|---|
| TX | GPIO 5 | GPIO 32 | GPIO 18 | GPIO 16 |
| VCC | 3V3 | 3V3 | 3V3 | 3V3 |
| GND | GND | GND | GND | GND |

The pin is `GPS_RX_GPIO` in `main/main.c`. The example reads at 9600 baud,
what u-blox NEO and ATGM336H modules send out of the box. A marine NMEA 0183
talker sends 4800 baud on an RS-422 pair and needs a receiver chip in between
([docs/nmea0183.md](../../../../docs/nmea0183.md)).

The same code reads a depth sounder or a compass that talks NMEA 0183: DPT,
DBT, HDG, HDT, MTW and VHW are published too.
