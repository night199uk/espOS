/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The card half of espos_sdlog: IDF's SDMMC and SDSPI drivers under FatFs.
 * Nothing here decides anything a host test could check; the sessions on top
 * are pure and tested.
 *
 * The card and the FAT mount are brought up separately, not through
 * esp_vfs_fat_sdmmc_mount(): its unmount also takes the card down, and a
 * card that is handed over USB to another owner has to stay up without FAT.
 */
#include <string.h>

#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_host.h"
#endif
#if SOC_GP_LDO_SUPPORTED
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#include "espos_sdlog_card.h"

static const char *TAG = "sdlog";

#define MOUNT_POINT_MAX 32

static struct {
    espos_sdlog_card_state_t state;
    sdmmc_card_t card;
    espos_sdlog_bus_t bus;
    int spi_host;
    sdspi_dev_handle_t spi_dev;
    bool spi_bus_owned;
    int sdmmc_slot;
    FATFS *fs;
    BYTE pdrv;
    char mount_point[MOUNT_POINT_MAX];
#if SOC_GP_LDO_SUPPORTED
    sd_pwr_ctrl_handle_t pwr;
#endif
} s_card;

static esp_err_t no_card(esp_err_t err)
{
    /* What sdmmc_card_init() returns for a slot with nothing in it. */
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC) {
        return ESP_ERR_NOT_FOUND;
    }
    return err;
}

static void free_ldo(void)
{
#if SOC_GP_LDO_SUPPORTED
    if (s_card.pwr) {
        sd_pwr_ctrl_del_on_chip_ldo(s_card.pwr);
        s_card.pwr = NULL;
    }
#endif
}

#if SOC_SDMMC_HOST_SUPPORTED
static esp_err_t init_sdmmc(const espos_sdlog_card_cfg_t *cfg)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = cfg->sdmmc.slot;
    host.max_freq_khz = cfg->max_freq_khz ? (int)cfg->max_freq_khz : SDMMC_FREQ_DEFAULT;
    if (cfg->sdmmc.ldo_chan > 0) {
#if SOC_GP_LDO_SUPPORTED
        const sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = cfg->sdmmc.ldo_chan };
        esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_card.pwr);
        if (err != ESP_OK) {
            return err;
        }
        host.pwr_ctrl_handle = s_card.pwr;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = cfg->sdmmc.width == 1 ? 1 : 4;
#if SOC_SDMMC_USE_GPIO_MATRIX
    slot.clk = cfg->sdmmc.clk;
    slot.cmd = cfg->sdmmc.cmd;
    slot.d0 = cfg->sdmmc.d0;
    slot.d1 = cfg->sdmmc.d1;
    slot.d2 = cfg->sdmmc.d2;
    slot.d3 = cfg->sdmmc.d3;
#endif
    /* Most slots have no pull-ups of their own; the internal ones are weak
     * but enough at 20 MHz. */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /* The controller may already be up for the other slot (the P4's radio
     * talks SDIO on slot 1), so only the slot is ever ours to take down. */
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        free_ldo();
        return err;
    }
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err == ESP_OK) {
        err = sdmmc_card_init(&host, &s_card.card);
        if (err != ESP_OK) {
            sdmmc_host_deinit_slot(host.slot);
        }
    }
    if (err != ESP_OK) {
        free_ldo();
        return err;
    }
    s_card.sdmmc_slot = host.slot;
    return ESP_OK;
}
#endif

static esp_err_t init_spi(const espos_sdlog_card_cfg_t *cfg)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = cfg->spi.mosi,
        .miso_io_num = cfg->spi.miso,
        .sclk_io_num = cfg->spi.sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    spi_host_device_t id = (spi_host_device_t)cfg->spi.host;
    /* A bus another driver already brought up (a display on the same SPI)
     * is shared, and stays up when the card goes. */
    esp_err_t err = spi_bus_initialize(id, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_card.spi_bus_owned = err == ESP_OK;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id = id;
    dev.gpio_cs = cfg->spi.cs;
    err = sdspi_host_init_device(&dev, &s_card.spi_dev);
    if (err == ESP_OK) {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = s_card.spi_dev;
        host.max_freq_khz = cfg->max_freq_khz ? (int)cfg->max_freq_khz : SDMMC_FREQ_DEFAULT;
        err = sdmmc_card_init(&host, &s_card.card);
        if (err != ESP_OK) {
            sdspi_host_remove_device(s_card.spi_dev);
        }
    }
    if (err != ESP_OK) {
        if (s_card.spi_bus_owned) {
            spi_bus_free(id);
        }
        return err;
    }
    s_card.spi_host = cfg->spi.host;
    return ESP_OK;
}

esp_err_t espos_sdlog_card_init(const espos_sdlog_card_cfg_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_card.state != ESPOS_SDLOG_CARD_OFF) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *mp = cfg->mount_point ? cfg->mount_point : "/sdcard";
    if (strlen(mp) >= sizeof(s_card.mount_point) || mp[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_card.card, 0, sizeof(s_card.card));
    esp_err_t err;
    if (cfg->bus == ESPOS_SDLOG_BUS_SPI) {
        err = init_spi(cfg);
    } else {
#if SOC_SDMMC_HOST_SUPPORTED
        err = init_sdmmc(cfg);
#else
        err = ESP_ERR_NOT_SUPPORTED;
#endif
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no card: %s", esp_err_to_name(err));
        return no_card(err);
    }
    strcpy(s_card.mount_point, mp);
    s_card.bus = cfg->bus;
    s_card.state = ESPOS_SDLOG_CARD_IDLE;
    ESP_LOGI(TAG, "card %s, %llu MB", s_card.card.cid.name,
             (unsigned long long)s_card.card.csd.capacity * s_card.card.csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

esp_err_t espos_sdlog_card_deinit(void)
{
    if (s_card.state != ESPOS_SDLOG_CARD_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_card.bus == ESPOS_SDLOG_BUS_SPI) {
        /* Only this card's device: sdspi_host_deinit() would remove every
         * SD-over-SPI device in the firmware. */
        sdspi_host_remove_device(s_card.spi_dev);
        if (s_card.spi_bus_owned) {
            spi_bus_free((spi_host_device_t)s_card.spi_host);
        }
    } else {
#if SOC_SDMMC_HOST_SUPPORTED
        sdmmc_host_deinit_slot(s_card.sdmmc_slot);
#endif
    }
    free_ldo();
    s_card.state = ESPOS_SDLOG_CARD_OFF;
    return ESP_OK;
}

esp_err_t espos_sdlog_card_mount_fat(void)
{
    if (s_card.state != ESPOS_SDLOG_CARD_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    BYTE pdrv = FF_DRV_NOT_USED;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == FF_DRV_NOT_USED) {
        return ESP_ERR_NO_MEM;
    }
    ff_diskio_register_sdmmc(pdrv, &s_card.card);
    char drv[3] = { (char)('0' + pdrv), ':', 0 };
    const esp_vfs_fat_conf_t conf = {
        .base_path = s_card.mount_point,
        .fat_drive = drv,
        .max_files = 5,
    };
    FATFS *fs = NULL;
    esp_err_t err = esp_vfs_fat_register(&conf, &fs);
    if (err != ESP_OK) {
        ff_diskio_unregister(pdrv);
        return err;
    }
    FRESULT res = f_mount(fs, drv, 1);
    if (res != FR_OK) {
        ESP_LOGW(TAG, "no FAT on the card (FatFs %d)", res);
        /* f_mount() keeps fs in its volume table even when it fails, and
         * the unregister below frees it. */
        f_mount(NULL, drv, 0);
        esp_vfs_fat_unregister_path(s_card.mount_point);
        ff_diskio_unregister(pdrv);
        return ESP_FAIL;
    }
    s_card.fs = fs;
    s_card.pdrv = pdrv;
    s_card.state = ESPOS_SDLOG_CARD_MOUNTED;
    ESP_LOGI(TAG, "FAT mounted at %s", s_card.mount_point);
    return ESP_OK;
}

esp_err_t espos_sdlog_card_unmount_fat(void)
{
    if (s_card.state != ESPOS_SDLOG_CARD_MOUNTED) {
        return ESP_ERR_INVALID_STATE;
    }
    char drv[3] = { (char)('0' + s_card.pdrv), ':', 0 };
    f_mount(NULL, drv, 0);
    esp_err_t err = esp_vfs_fat_unregister_path(s_card.mount_point);
    ff_diskio_unregister(s_card.pdrv);
    s_card.fs = NULL;
    s_card.state = ESPOS_SDLOG_CARD_IDLE;
    return err;
}

esp_err_t espos_sdlog_card_mount(const espos_sdlog_card_cfg_t *cfg)
{
    esp_err_t err = espos_sdlog_card_init(cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = espos_sdlog_card_mount_fat();
    if (err != ESP_OK) {
        espos_sdlog_card_deinit();
    }
    return err;
}

esp_err_t espos_sdlog_card_unmount(void)
{
    esp_err_t err = espos_sdlog_card_unmount_fat();
    if (err != ESP_OK) {
        return err;
    }
    return espos_sdlog_card_deinit();
}

esp_err_t espos_sdlog_card_claim(void **card)
{
    if (!card) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_card.state != ESPOS_SDLOG_CARD_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_card.state = ESPOS_SDLOG_CARD_CLAIMED;
    *card = &s_card.card;
    return ESP_OK;
}

esp_err_t espos_sdlog_card_release(void)
{
    if (s_card.state != ESPOS_SDLOG_CARD_CLAIMED) {
        return ESP_ERR_INVALID_STATE;
    }
    s_card.state = ESPOS_SDLOG_CARD_IDLE;
    return ESP_OK;
}

espos_sdlog_card_state_t espos_sdlog_card_state(void)
{
    return s_card.state;
}

bool espos_sdlog_card_mounted(void)
{
    return s_card.state == ESPOS_SDLOG_CARD_MOUNTED;
}

const char *espos_sdlog_card_mount_point(void)
{
    return s_card.state == ESPOS_SDLOG_CARD_MOUNTED ? s_card.mount_point : NULL;
}

esp_err_t espos_sdlog_card_space(uint64_t *total, uint64_t *free_bytes)
{
    if (!total || !free_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_card.state != ESPOS_SDLOG_CARD_MOUNTED) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_vfs_fat_info(s_card.mount_point, total, free_bytes);
}
