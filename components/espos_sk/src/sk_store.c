/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device state that must not travel with an exported configuration: the
 * persistent clientId, the access token (keyed by the server's self URN and
 * the address it was issued at)
 * and a pending request. Plain NVS in the same (optionally encrypted)
 * partition as the config store.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "espos_sk_priv.h"

static const char *TAG = "espos_sk";
#define NS "skstate"

#ifndef CONFIG_ESPOS_CONFIG_NVS_PARTITION
#define CONFIG_ESPOS_CONFIG_NVS_PARTITION "nvs"
#endif

static esp_err_t open_ns(nvs_handle_t *h)
{
    return nvs_open_from_partition(CONFIG_ESPOS_CONFIG_NVS_PARTITION, NS, NVS_READWRITE, h);
}

static void get_str(nvs_handle_t h, const char *key, char *buf, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
        buf[0] = '\0';
    }
}

/* RFC 4122 v4 from esp_random(); generated once, then immutable. */
static void make_uuid(char out[40])
{
    uint8_t b[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        memcpy(b + i, &r, 4);
    }
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    snprintf(out, 40, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/* FNV-1a over the token. tok_host/tok_port are only believed when this
 * matches: firmware from before they existed rewrites the token without
 * touching them (an OTA rollback that then pairs with another server), and the
 * new token must not inherit the old one's address. */
static uint32_t token_sum(const char *tok)
{
    uint32_t h = 2166136261u;
    for (; *tok; tok++) {
        h = (h ^ (uint8_t)*tok) * 16777619u;
    }
    return h;
}

esp_err_t espos_sk_store_load(espos_sk_tok_store_t *out, char client_id[40])
{
    memset(out, 0, sizeof(*out));
    client_id[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = open_ns(&h);
    if (err != ESP_OK) {
        /* No persistent state: still identify ourselves (the id will change
         * on the next boot, which the admin will notice as a new device). */
        ESP_LOGE(TAG, "cannot open state namespace: %s; using a volatile clientId", esp_err_to_name(err));
        make_uuid(client_id);
        return err;
    }
    get_str(h, "client_id", client_id, 40);
    if (client_id[0] == '\0') {
        make_uuid(client_id);
        err = nvs_set_str(h, "client_id", client_id);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        ESP_LOGI(TAG, "generated clientId %s (%s)", client_id, esp_err_to_name(err));
    }
    get_str(h, "token", out->token, sizeof(out->token));
    get_str(h, "tok_self", out->token_self, sizeof(out->token_self));
    get_str(h, "tok_host", out->token_host, sizeof(out->token_host));
    get_str(h, "pend_href", out->pending_href, sizeof(out->pending_href));
    get_str(h, "pend_host", out->pending_host, sizeof(out->pending_host));
    get_str(h, "pend_self", out->pending_self, sizeof(out->pending_self));
    uint16_t port = 0;
    if (nvs_get_u16(h, "pend_port", &port) == ESP_OK) {
        out->pending_port = port;
    }
    port = 0;
    if (nvs_get_u16(h, "tok_port", &port) == ESP_OK) {
        out->token_port = port;
    }
    uint32_t sum = 0;
    if (!out->token[0] || nvs_get_u32(h, "tok_sum", &sum) != ESP_OK || sum != token_sum(out->token)) {
        out->token_host[0] = '\0';
        out->token_port = 0;
    }
    nvs_close(h);
    return ESP_OK;
}

/* Kept apart from the clientId: that one names the device to the server's
 * security, this one names a vessel in the data, and a user who replaces the
 * board in a dinghy may one day want to carry the vessel over without the
 * access grant. Never generated volatile: a vessel that is new on every boot
 * splits one boat's soundings across a fresh identity each time. */
esp_err_t espos_sk_store_vessel_uuid(char uuid[40])
{
    uuid[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = open_ns(&h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot open state namespace: %s; no vessel UUID", esp_err_to_name(err));
        return err;
    }
    size_t len = 40;
    err = nvs_get_str(h, "vessel_id", uuid, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        /* A vessel that exists but cannot be read now must not be replaced:
         * a new UUID written over it would split the boat's data for good. */
        ESP_LOGE(TAG, "cannot read the vessel UUID: %s", esp_err_to_name(err));
        uuid[0] = '\0';
        nvs_close(h);
        return err;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        make_uuid(uuid);
        err = nvs_set_str(h, "vessel_id", uuid);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cannot store the vessel UUID: %s", esp_err_to_name(err));
            uuid[0] = '\0';
        } else {
            ESP_LOGI(TAG, "generated vessel UUID %s", uuid);
        }
    }
    nvs_close(h);
    return err;
}

static esp_err_t set_or_erase(nvs_handle_t h, const char *key, const char *val)
{
    if (val[0]) {
        return nvs_set_str(h, key, val);
    }
    esp_err_t err = nvs_erase_key(h, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t espos_sk_store_save(const espos_sk_tok_store_t *st)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(&h);
    if (err != ESP_OK) {
        return err;
    }
    esp_err_t e;
    if ((e = set_or_erase(h, "token", st->token)) != ESP_OK) err = e;
    if ((e = set_or_erase(h, "tok_self", st->token_self)) != ESP_OK) err = e;
    if ((e = set_or_erase(h, "tok_host", st->token_host)) != ESP_OK) err = e;
    if ((e = nvs_set_u16(h, "tok_port", st->token_port)) != ESP_OK) err = e;
    if ((e = nvs_set_u32(h, "tok_sum", token_sum(st->token))) != ESP_OK) err = e;
    if ((e = set_or_erase(h, "pend_href", st->pending_href)) != ESP_OK) err = e;
    if ((e = set_or_erase(h, "pend_host", st->pending_host)) != ESP_OK) err = e;
    if ((e = set_or_erase(h, "pend_self", st->pending_self)) != ESP_OK) err = e;
    if ((e = nvs_set_u16(h, "pend_port", st->pending_port)) != ESP_OK) err = e;
    if ((e = nvs_commit(h)) != ESP_OK) err = e;
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "state save failed: %s", esp_err_to_name(err));
    }
    return err;
}
