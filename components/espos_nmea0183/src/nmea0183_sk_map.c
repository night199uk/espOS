/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sentence -> Signal K values. Pure C, host-tested; the paths are the ones
 * signalk-server's nmea0183-signalk parser writes for the same sentences.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "espos_nmea0183_sk.h"

/* 2020-01-01T00:00:00Z: no receiver with a fix reports an earlier date. */
#define CLOCK_FLOOR_MS 1577836800000LL

typedef struct {
    espos_nmea0183_sk_update_t *out;
    size_t max, n;
} sink_t;

static espos_nmea0183_sk_update_t *next(sink_t *k, const char *path, espos_nmea0183_sk_kind_t kind)
{
    if (k->n >= k->max) {
        return NULL;
    }
    espos_nmea0183_sk_update_t *u = &k->out[k->n++];
    memset(u, 0, sizeof(*u));
    u->path = path;
    u->kind = kind;
    return u;
}

static void number(sink_t *k, const char *path, double v)
{
    espos_nmea0183_sk_update_t *u = next(k, path, ESPOS_NMEA0183_SK_NUMBER);
    if (u) {
        u->number = v;
    }
}

static void string(sink_t *k, const char *path, const char *v)
{
    espos_nmea0183_sk_update_t *u = next(k, path, ESPOS_NMEA0183_SK_STRING);
    if (u) {
        snprintf(u->text, sizeof(u->text), "%s", v);
    }
}

static void position(sink_t *k, double lat, double lon)
{
    espos_nmea0183_sk_update_t *u = next(k, "navigation.position", ESPOS_NMEA0183_SK_JSON);
    if (u) {
        /* Seven decimals: about a centimetre, finer than any GNSS on a boat. */
        snprintf(u->text, sizeof(u->text), "{\"longitude\":%.7f,\"latitude\":%.7f}", lon, lat);
    }
}

static void datetime(sink_t *k, int64_t unix_ms)
{
    time_t secs = (time_t)(unix_ms / 1000);
    struct tm tm;
    if (!gmtime_r(&secs, &tm)) {
        return;
    }
    char iso[48];
    size_t len = strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(iso + len, sizeof(iso) - len, ".%03dZ", (int)(unix_ms % 1000));
    string(k, "navigation.datetime", iso);
}

/* Signal K's navigation.gnss.methodQuality names for the GGA fix quality. */
static const char *method_quality(int q)
{
    static const char *const names[] = {
        "no GPS",
        "GNSS Fix",
        "DGNSS fix",
        "Precise GNSS",
        "RTK fixed integer",
        "RTK float",
        "Estimated (DR) mode",
        "Manual input",
        "Simulator mode",
    };
    return q >= 0 && q < (int)(sizeof(names) / sizeof(names[0])) ? names[q] : "Error";
}

size_t espos_nmea0183_sk_map(const espos_nmea0183_sentence_t *s, espos_nmea0183_sk_update_t *out, size_t max)
{
    if (!s || !out || max == 0) {
        return 0;
    }
    sink_t k = { .out = out, .max = max, .n = 0 };

    espos_nmea0183_gga_t gga;
    espos_nmea0183_rmc_t rmc;
    espos_nmea0183_vtg_t vtg;
    espos_nmea0183_heading_t hdg;
    espos_nmea0183_depth_t dpt;
    espos_nmea0183_mtw_t mtw;
    espos_nmea0183_vhw_t vhw;
    espos_nmea0183_zda_t zda;

    if (espos_nmea0183_decode_gga(s, &gga)) {
        if (gga.has_position) {
            position(&k, gga.lat, gga.lon);
        }
        string(&k, "navigation.gnss.methodQuality", method_quality(gga.quality));
        if (gga.has_satellites) {
            number(&k, "navigation.gnss.satellites", gga.satellites);
        }
        if (gga.has_hdop) {
            number(&k, "navigation.gnss.horizontalDilution", gga.hdop);
        }
        if (gga.has_altitude) {
            number(&k, "navigation.gnss.antennaAltitude", gga.altitude);
        }
        if (gga.has_geoid) {
            number(&k, "navigation.gnss.geoidalSeparation", gga.geoid_separation);
        }
    } else if (espos_nmea0183_decode_rmc(s, &rmc)) {
        if (rmc.valid) {
            if (rmc.has_position) {
                position(&k, rmc.lat, rmc.lon);
            }
            if (rmc.has_cog) {
                number(&k, "navigation.courseOverGroundTrue", rmc.cog_true);
            }
            if (rmc.has_sog) {
                number(&k, "navigation.speedOverGround", rmc.sog);
            }
            if (rmc.has_datetime && rmc.unix_ms >= CLOCK_FLOOR_MS) {
                datetime(&k, rmc.unix_ms);
            }
        }
        if (rmc.has_variation) {
            number(&k, "navigation.magneticVariation", rmc.variation);
        }
    } else if (espos_nmea0183_decode_vtg(s, &vtg)) {
        if (vtg.has_cog_true) {
            number(&k, "navigation.courseOverGroundTrue", vtg.cog_true);
        }
        if (vtg.has_cog_magnetic) {
            number(&k, "navigation.courseOverGroundMagnetic", vtg.cog_magnetic);
        }
        if (vtg.has_sog) {
            number(&k, "navigation.speedOverGround", vtg.sog);
        }
    } else if (espos_nmea0183_decode_heading(s, &hdg)) {
        if (hdg.has_true) {
            number(&k, "navigation.headingTrue", hdg.heading_true);
        }
        if (hdg.has_magnetic) {
            number(&k, "navigation.headingMagnetic", hdg.heading_magnetic);
        }
        if (hdg.has_variation) {
            number(&k, "navigation.magneticVariation", hdg.variation);
        }
    } else if (espos_nmea0183_decode_depth(s, &dpt)) {
        switch (dpt.below) {
        case ESPOS_NMEA0183_DEPTH_TRANSDUCER:
            number(&k, "environment.depth.belowTransducer", dpt.depth);
            /* DPT's offset: + is the transducer's depth below the waterline,
             * − its height above the keel. Either one places the sounding. */
            if (dpt.has_offset && dpt.offset > 0) {
                number(&k, "environment.depth.surfaceToTransducer", dpt.offset);
                number(&k, "environment.depth.belowSurface", dpt.depth + dpt.offset);
            } else if (dpt.has_offset && dpt.offset < 0) {
                number(&k, "environment.depth.transducerToKeel", -dpt.offset);
                number(&k, "environment.depth.belowKeel", dpt.depth + dpt.offset);
            }
            break;
        case ESPOS_NMEA0183_DEPTH_SURFACE:
            number(&k, "environment.depth.belowSurface", dpt.depth);
            break;
        case ESPOS_NMEA0183_DEPTH_KEEL:
            number(&k, "environment.depth.belowKeel", dpt.depth);
            break;
        case ESPOS_NMEA0183_DEPTH_REF_MAX:
            break;
        }
    } else if (espos_nmea0183_decode_mtw(s, &mtw)) {
        number(&k, "environment.water.temperature", mtw.temperature);
    } else if (espos_nmea0183_decode_vhw(s, &vhw)) {
        if (vhw.has_stw) {
            number(&k, "navigation.speedThroughWater", vhw.stw);
        }
        if (vhw.has_true) {
            number(&k, "navigation.headingTrue", vhw.heading_true);
        }
        if (vhw.has_magnetic) {
            number(&k, "navigation.headingMagnetic", vhw.heading_magnetic);
        }
    } else if (espos_nmea0183_decode_zda(s, &zda)) {
        if (zda.unix_ms >= CLOCK_FLOOR_MS) {
            datetime(&k, zda.unix_ms);
        }
    }
    return k.n;
}

int64_t espos_nmea0183_sk_clock_ms(const espos_nmea0183_sentence_t *s)
{
    espos_nmea0183_rmc_t rmc;
    if (!espos_nmea0183_decode_rmc(s, &rmc) || !rmc.valid || !rmc.has_datetime || rmc.unix_ms < CLOCK_FLOOR_MS) {
        return 0;
    }
    return rmc.unix_ms;
}

bool espos_nmea0183_sk_clock_due(int64_t gps_ms, int64_t now_ms, bool synced)
{
    if (gps_ms <= 0) {
        return false;
    }
    if (!synced || now_ms <= 0) {
        return true;
    }
    int64_t drift = gps_ms - now_ms;
    return drift > ESPOS_NMEA0183_SK_CLOCK_SLACK_MS || drift < -ESPOS_NMEA0183_SK_CLOCK_SLACK_MS;
}
