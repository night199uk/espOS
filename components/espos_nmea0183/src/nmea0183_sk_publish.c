/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The thin half of espos_nmea0183_sk.h: hand mapped values to espos_sk, and
 * GPS time to espos_time. Both are optional, so a firmware that only logs
 * sentences does not link the Signal K client for them.
 */
#include "espos_nmea0183_sk.h"

#if ESPOS_NMEA0183_HAVE_SK
#include "espos_sk.h"
#endif
#if ESPOS_NMEA0183_HAVE_TIME
#include "espos_time.h"
#endif

esp_err_t espos_nmea0183_sk_publish(const espos_nmea0183_sentence_t *s, bool set_clock)
{
#if ESPOS_NMEA0183_HAVE_TIME
    if (set_clock) {
        int64_t ms = espos_nmea0183_sk_clock_ms(s);
        /* SNTP outranks a GPS: asking would only be refused, once a second. */
        if (ms > 0 && espos_time_source() <= ESPOS_TIME_SRC_MANUAL &&
            espos_nmea0183_sk_clock_due(ms, espos_time_now_ms(), espos_time_is_synced())) {
            (void)espos_time_set(ms, ESPOS_TIME_SRC_MANUAL);
        }
    }
#else
    (void)set_clock;
#endif
#if ESPOS_NMEA0183_HAVE_SK
    espos_nmea0183_sk_update_t up[ESPOS_NMEA0183_SK_MAX_UPDATES];
    size_t n = espos_nmea0183_sk_map(s, up, ESPOS_NMEA0183_SK_MAX_UPDATES);
    esp_err_t first = ESP_OK;
    for (size_t i = 0; i < n; i++) {
        esp_err_t err = ESP_OK;
        switch (up[i].kind) {
        case ESPOS_NMEA0183_SK_NUMBER:
            err = espos_sk_publish_number(up[i].path, up[i].number);
            break;
        case ESPOS_NMEA0183_SK_JSON:
            err = espos_sk_publish_json(up[i].path, up[i].text);
            break;
        case ESPOS_NMEA0183_SK_STRING:
            err = espos_sk_publish_string(up[i].path, up[i].text);
            break;
        case ESPOS_NMEA0183_SK_KIND_MAX:
            break;
        }
        if (first == ESP_OK) {
            first = err;
        }
    }
    return first;
#else
    (void)s;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
