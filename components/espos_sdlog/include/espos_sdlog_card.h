/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_sdlog_card — a microSD card as a FAT file system, over SDMMC or SPI.
 *
 * The ESP32, the S3 and the P4 have an SDMMC host; every chip can talk to a
 * card over SPI, slower but on any four pins. Once mounted, the card is
 * ordinary files under its mount point, which is where espos_sdlog.h keeps
 * its sessions and where anything else (a layout, a map tile) can go too.
 *
 * Bringing the card up and mounting FAT on it are two steps, because the
 * card has exactly one owner at a time. espOS's own FAT mount is one; a
 * component that hands the raw blocks to someone else (a USB host that sees
 * the card as its own storage) is the other, and claims the card instead of
 * mounting it. Two writers on the same blocks each cache the file system's
 * tables and corrupt it, so the states exclude each other:
 *
 *     off --init--> idle --mount_fat--> mounted
 *                    |  <--unmount_fat--
 *                    +--claim--> claimed --release--> idle
 *
 * Call these from one task, or serialise them; the sessions on top must all
 * be closed before the FAT is unmounted.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPOS_SDLOG_BUS_SDMMC = 0,
    ESPOS_SDLOG_BUS_SPI = 1,
    ESPOS_SDLOG_BUS_MAX = 2,
} espos_sdlog_bus_t;

/** The SDMMC host's slot and pins, for ESPOS_SDLOG_BUS_SDMMC. */
typedef struct {
    int32_t slot;  /**< 0 or 1; the P4's IOMUX pins are slot 0 */
    int32_t width; /**< 1 or 4 data lines */
    int32_t clk, cmd, d0, d1, d2, d3;
    /** The P4's on-chip LDO channel that powers the card's I/O (4 on
     * Waveshare's P4 boards), or 0 when the board powers it. */
    int32_t ldo_chan;
} espos_sdlog_sdmmc_cfg_t;

/** The SPI host and pins, for ESPOS_SDLOG_BUS_SPI. */
typedef struct {
    int32_t host; /**< SPI2_HOST (1) or SPI3_HOST (2) */
    int32_t mosi, miso, sclk, cs;
} espos_sdlog_spi_cfg_t;

typedef struct {
    espos_sdlog_bus_t bus;
    const char *mount_point; /**< NULL means "/sdcard" */
    uint32_t max_freq_khz;   /**< 0: 20 MHz, the speed every card manages */
    espos_sdlog_sdmmc_cfg_t sdmmc; /**< read when `bus` is SDMMC */
    espos_sdlog_spi_cfg_t spi;     /**< read when `bus` is SPI */
} espos_sdlog_card_cfg_t;

/** The card slot of the Waveshare ESP32-P4 boards (the 7B panel, the PoE
 * board): SDMMC slot 0 on its IOMUX pins, four lines, I/O powered by LDO 4. */
#define ESPOS_SDLOG_CARD_WAVESHARE_P4()                          \
    (espos_sdlog_card_cfg_t)                                     \
    {                                                            \
        .bus = ESPOS_SDLOG_BUS_SDMMC, .sdmmc = { .slot = 0,      \
                                                 .width = 4,     \
                                                 .clk = 43,      \
                                                 .cmd = 44,      \
                                                 .d0 = 39,       \
                                                 .d1 = 40,       \
                                                 .d2 = 41,       \
                                                 .d3 = 42,       \
                                                 .ldo_chan = 4 } \
    }

/** A card on SPI pins, the way most breakout boards and many ESP32 boards
 * with a slot (the LilyGO T-CAN485 among them) wire it. */
#define ESPOS_SDLOG_CARD_SPI(host_, mosi_, miso_, sclk_, cs_) \
    (espos_sdlog_card_cfg_t)                                  \
    {                                                         \
        .bus = ESPOS_SDLOG_BUS_SPI, .spi = { .host = (host_), \
                                             .mosi = (mosi_), \
                                             .miso = (miso_), \
                                             .sclk = (sclk_), \
                                             .cs = (cs_) }    \
    }

typedef enum {
    ESPOS_SDLOG_CARD_OFF = 0,     /**< no card brought up */
    ESPOS_SDLOG_CARD_IDLE = 1,    /**< the card answers; nobody uses it */
    ESPOS_SDLOG_CARD_MOUNTED = 2, /**< FAT mounted at the mount point */
    ESPOS_SDLOG_CARD_CLAIMED = 3, /**< handed to another component as raw blocks */
    ESPOS_SDLOG_CARD_STATE_MAX = 4,
} espos_sdlog_card_state_t;

/**
 * Bring the card up: the host, the bus, the card's identification. No file
 * system yet. ESP_ERR_NOT_SUPPORTED for SDMMC on a chip without the host,
 * ESP_ERR_INVALID_STATE when a card is already up, ESP_ERR_NOT_FOUND when
 * no card answers.
 */
esp_err_t espos_sdlog_card_init(const espos_sdlog_card_cfg_t *cfg);

/** Take the card down and free the bus. ESP_ERR_INVALID_STATE unless idle. */
esp_err_t espos_sdlog_card_deinit(void);

/** Mount FAT on the idle card. ESP_FAIL when it holds nothing FatFs can
 * read (exFAT, unformatted): this never formats a card, since one from a
 * camera or a plotter is someone's data. */
esp_err_t espos_sdlog_card_mount_fat(void);

/** Unmount FAT; the card stays up, idle. Close every file first. */
esp_err_t espos_sdlog_card_unmount_fat(void);

/** init() then mount_fat(), undone on failure: the usual way in. */
esp_err_t espos_sdlog_card_mount(const espos_sdlog_card_cfg_t *cfg);

/** unmount_fat() then deinit(). */
esp_err_t espos_sdlog_card_unmount(void);

/**
 * Take the idle card as raw blocks: `*card` is its `sdmmc_card_t *`, for
 * sdmmc_read_sectors()/sdmmc_write_sectors() or a USB mass-storage class.
 * ESP_ERR_INVALID_STATE while FAT is mounted or someone else holds it: the
 * firmware closes its sessions and unmounts first, which is what makes a
 * logger stand down.
 */
esp_err_t espos_sdlog_card_claim(void **card);

/** Give a claimed card back; it is idle again, and mount_fat() reads what
 * the other owner wrote. Run espos_sdlog_recover() after mounting. */
esp_err_t espos_sdlog_card_release(void);

/** Where the card is. */
espos_sdlog_card_state_t espos_sdlog_card_state(void);

/** Whether FAT is mounted. */
bool espos_sdlog_card_mounted(void);

/** Where FAT is mounted, or NULL while it is not. */
const char *espos_sdlog_card_mount_point(void);

/** Size and free space of the mounted FAT, in bytes. */
esp_err_t espos_sdlog_card_space(uint64_t *total, uint64_t *free_bytes);

#ifdef __cplusplus
}
#endif
