# microSD logging

`espos_sdlog` mounts a microSD card and keeps sessions of rows on it that
survive a power cut. A depth logger in a dinghy, a trip recorder, an engine log
on a boat that is out of Wi-Fi range for a week: the device writes rows while
nothing is listening, and delivers them whole once something is.

It comes in two layers:

* **The card** (`espos_sdlog_card.h`): SDMMC (ESP32, S3, P4) or SPI (every
  chip), with FAT mounted at `/sdcard`, or the raw card handed to another
  component.
* **Sessions** (`espos_sdlog.h`, pure C on stdio): open, append, close,
  recover after a power cut, mark delivered, prune. Nothing in it is about the
  card, so the host tests run it on a temporary directory, and it works on any
  file system the VFS has mounted.

It is optional, so a project names it:

```cmake
espos_project_prologue(NAME "my_logger" COMPONENTS espos_sdlog)
```

## A logger in fifteen lines

```c
#include "espos.h"
#include "espos_sdlog.h"
#include "espos_sdlog_card.h"

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));
    const espos_sdlog_card_cfg_t card = ESPOS_SDLOG_CARD_WAVESHARE_P4();
    ESP_ERROR_CHECK(espos_sdlog_card_mount(&card));
    espos_sdlog_recover("/sdcard/logs", NULL, NULL);   /* what the last power cut left open */

    espos_sdlog_session_t s;
    ESP_ERROR_CHECK(espos_sdlog_open("/sdcard/logs", "{\"columns\":[\"t\",\"depth\"]}", NULL, &s));
    espos_sdlog_append(s, "1792170000000,3.42");
    espos_sdlog_close(s, NULL);
}
```

The whole example is
[`sd_logger`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sdlog/examples/sd_logger).

## What is on the card

```
/sdcard/logs/00000042/HEAD.JSN    the header the session was opened with
/sdcard/logs/00000042/ROWS.TXT    one row per line
/sdcard/logs/00000042/CLOSE.JSN   {"closed":true,"clean":true,"rows":3600,"bytes":151200,"sha256":"…","meta":null}
/sdcard/logs/00000042/SYNCED      present once the session reached its server
/sdcard/logs/LASTSEQ.TXT          the highest session number handed out
```

A session's number is one above the highest ever handed out under the root,
which `LASTSEQ.TXT` beside the sessions remembers: pruning delivered sessions
never makes a number come round again. It is unique on one card, not across
devices or a reformatted card, so a server tells sessions apart by their
header (a trip id) and recognises a repeated upload by its sha256. What a session
means is the caller's: the header is whatever it passes to
`espos_sdlog_open()` (a trip id, the vessel, the units, the sensor offsets),
the rows are whatever lines it appends, and `meta` in the close record is
whatever JSON it passes to `espos_sdlog_close()` (an end time). The component
counts rows, hashes them and keeps the order of writes that makes a power cut
harmless.

Names fit 8.3, so a card works with FatFs' default of no long file names, and
a session reads the same on a laptop.

## Power cuts

At every moment the power can go, a session is in one of three states:

| State | On the card | At the next boot, `espos_sdlog_recover()` |
|---|---|---|
| never started | a directory with no `HEAD.JSN` | removes it |
| open | `HEAD.JSN`, `ROWS.TXT`, no `CLOSE.JSN` | cuts the half row after the last newline, counts and hashes what is left, and writes a close record with `"clean":false` |
| closed | `CLOSE.JSN` | leaves it |

The header and the close record are written under a temporary name, synced,
then renamed, so neither is ever half there. Rows are synced every
`sync_every` rows (10 by default): that is what a power cut can lose at most.
A sync is a card write cycle, so syncing every row of a row-a-second logger
wears a card for little gain; `espos_sdlog_sync()` before a planned sleep or a
shutdown covers the rest.

One gap FatFs leaves: a rename writes the new name before it removes the old
one, so a power cut between those two sector writes can leave `HEAD.TMP` and
`HEAD.JSN` (or the `CLOSE` pair) pointing at the same data. The session reads
correctly; deleting it later frees that data under both names. The window is
two sector writes wide, once per session.

Call `espos_sdlog_recover()` once after mounting and before opening a
session: a session open at the time looks cut short too.

### Rows that stand alone

A power cut costs the half row after the last newline, which recovery cuts.
A card that was pulled, or a sector that went bad, can garble a row in the
middle too. With `.row_crc = true` in the open configuration every row ends
in `,xxxxxxxx`, the CRC-32 (`espos_sdlog_crc32()`, the zlib one) of the row
before that comma in lower-case hex. A reader keeps each row that ends in a
newline and passes its check, and drops the rest one by one, so nothing on
the card needs the close record to be read.

For a logger that has no stop signal (a dinghy whose power is cut without
warning), set `sync_every` to the number of rows in a few seconds: that is
then all a cut loses, since FatFs writes the file's size and cluster chain
only on a sync.

## Delivering sessions

Every session but the open one is complete: one the power cut off is closed
by `espos_sdlog_recover()` at the next boot. Its close record is the device's
own bookkeeping and carries what an upload needs to send along: the row count
and the sha256 of `ROWS.TXT`. A server never needs `CLOSE.JSN` itself. The
delivery itself is the firmware's (it knows the server and its API); the
component gives it the pieces:

```c
static bool deliver(const espos_sdlog_info_t *info, void *arg)
{
    if (!info->closed || info->synced) {
        return true;                       /* still being written, or already there */
    }
    char head[96], rows[96];
    espos_sdlog_path(ROOT, info->seq, ESPOS_SDLOG_FILE_HEADER, head, sizeof(head));
    espos_sdlog_path(ROOT, info->seq, ESPOS_SDLOG_FILE_ROWS, rows, sizeof(rows));
    if (upload(head, rows, info->rows, info->sha256) == ESP_OK) {   /* the firmware's own */
        espos_sdlog_mark_synced(ROOT, info->seq, "201");
    }
    return true;
}

espos_sdlog_list(ROOT, deliver, NULL);
```

Mark a session synced only once the server has confirmed it. A server that
answers a repeated upload as a duplicate makes the rest safe: a power cut
between the server's answer and `SYNCED` costs one more upload, never a lost
session.

## A full card

`espos_sdlog_prune(root, keep_bytes, &removed)` deletes the oldest sessions
that are both closed and synced until the row files under the root add up to
`keep_bytes` or less. A session that has not reached its server is never
deleted: a card full of undelivered data stops the logger (an append fails)
rather than eating what it was there to keep. `espos_sdlog_card_space()` says
how much is left.

## Sharing the card

The root is the component's only claim on the file system: anything else on
the card is left alone, and under the root only eight-digit directories are
sessions. Other files, another program's directories, a chart plotter's own
folder can sit beside it.

What cannot be shared is the card itself. FatFs on the device and a second
writer on the same blocks (a PC, or a plotter the card is exported to over
USB) each cache the file system's tables, and two writers corrupt it. So the
card has one owner at a time, and bringing it up is a step of its own:

| Call | From | To |
|---|---|---|
| `espos_sdlog_card_init(&cfg)` | off | idle: the card answers, no file system |
| `espos_sdlog_card_mount_fat()` | idle | mounted: FAT at the mount point, sessions work |
| `espos_sdlog_card_unmount_fat()` | mounted | idle |
| `espos_sdlog_card_claim(&card)` | idle | claimed: `card` is the `sdmmc_card_t *` for raw sector access |
| `espos_sdlog_card_release()` | claimed | idle |
| `espos_sdlog_card_deinit()` | idle | off |

`espos_sdlog_card_mount()` is `init` plus `mount_fat`. A claim is refused
while FAT is mounted, so a firmware that exports its card first closes its
sessions and unmounts, which is what makes its logger stand down. After the
release it mounts again and calls `espos_sdlog_recover()`, then reads what the
other owner wrote.

## Wiring and cards

| Board | Bus | Configuration |
|---|---|---|
| Waveshare ESP32-P4 boards (the 7B panel, the PoE board) | SDMMC slot 0, 4 lines, I/O powered by LDO 4 | `ESPOS_SDLOG_CARD_WAVESHARE_P4()` |
| ESP32 or S3 with a 4-line slot | SDMMC | `.bus = ESPOS_SDLOG_BUS_SDMMC` with the slot's pins |
| Any chip with a breakout, LilyGO T-CAN485 | SPI | `ESPOS_SDLOG_CARD_SPI(host, mosi, miso, sclk, cs)` |

Use a card formatted FAT32. Cards over 32 GB come formatted exFAT, which
ESP-IDF's FatFs is built without; reformat such a card as FAT32 on a computer
(with a tool that allows FAT32 at that size), or use a smaller card. The
component never formats a card: one from a camera or a plotter is someone's
data.

## When it does not work

| Reading | Means |
|---|---|
| `ESP_ERR_NOT_FOUND` from mount | no card answers: not inserted, wrong pins, or (P4) the slot's I/O is not powered (`ldo_chan`) |
| `ESP_FAIL` from mount | a card answers but holds nothing FatFs reads: exFAT (most cards over 32 GB), or unformatted |
| `ESP_ERR_NOT_SUPPORTED` from mount | SDMMC on a chip without the host (C3, C5, C6): use SPI |
| `ESP_FAIL` from append | the card refused a write: full, or pulled out. Close the session; the next boot recovers it if the close fails too |
| sessions closed `"clean":false` at every boot | the logger is never closed before the power goes; normal on a dinghy, and why recover() exists |
