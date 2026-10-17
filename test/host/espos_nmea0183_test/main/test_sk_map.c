/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "espos_nmea0183_sk.h"
#include "unity.h"

static char buf[256];
static espos_nmea0183_sk_update_t up[ESPOS_NMEA0183_SK_MAX_UPDATES];

static size_t map(const char *text)
{
    espos_nmea0183_sentence_t s;
    snprintf(buf, sizeof(buf), "%s", text);
    TEST_ASSERT_EQUAL_MESSAGE(ESPOS_NMEA0183_OK, espos_nmea0183_parse(buf, true, &s), text);
    return espos_nmea0183_sk_map(&s, up, ESPOS_NMEA0183_SK_MAX_UPDATES);
}

static const espos_nmea0183_sk_update_t *find(size_t n, const char *path)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(up[i].path, path) == 0) {
            return &up[i];
        }
    }
    return NULL;
}

TEST_CASE("GGA maps to position and the gnss branch", "[sk]")
{
    size_t n = map("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    const espos_nmea0183_sk_update_t *u = find(n, "navigation.position");
    TEST_ASSERT_NOT_NULL(u);
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_SK_JSON, u->kind);
    TEST_ASSERT_EQUAL_STRING("{\"longitude\":11.5166667,\"latitude\":48.1173000}", u->text);
    u = find(n, "navigation.gnss.methodQuality");
    TEST_ASSERT_NOT_NULL(u);
    TEST_ASSERT_EQUAL_STRING("GNSS Fix", u->text);
    TEST_ASSERT_EQUAL_DOUBLE(8, find(n, "navigation.gnss.satellites")->number);
    TEST_ASSERT_EQUAL_DOUBLE(0.9, find(n, "navigation.gnss.horizontalDilution")->number);
    TEST_ASSERT_NOT_NULL(find(n, "navigation.gnss.antennaAltitude"));
    TEST_ASSERT_NOT_NULL(find(n, "navigation.gnss.geoidalSeparation"));

    /* No fix: the quality and the count, never a position. */
    n = map("$GPGGA,123519,4807.038,N,01131.000,E,0,00,,,M,,M,,*52");
    TEST_ASSERT_NULL(find(n, "navigation.position"));
    TEST_ASSERT_EQUAL_STRING("no GPS", find(n, "navigation.gnss.methodQuality")->text);
}

TEST_CASE("RMC maps to position, COG, SOG, variation and datetime", "[sk]")
{
    size_t n = map("$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191126,020.3,E*61");
    TEST_ASSERT_NOT_NULL(find(n, "navigation.position"));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 54.7 * M_PI / 180.0, find(n, "navigation.courseOverGroundTrue")->number);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.5 * 1852.0 / 3600.0, find(n, "navigation.speedOverGround")->number);
    TEST_ASSERT_NOT_NULL(find(n, "navigation.magneticVariation"));
    TEST_ASSERT_EQUAL_STRING("2026-11-19T22:54:46.000Z", find(n, "navigation.datetime")->text);

    n = map("$GNRMC,083559.00,V,4717.11437,N,00833.91522,E,0.004,77.52,091226,,,N*57");
    TEST_ASSERT_EQUAL(0, n);
}

TEST_CASE("DPT maps to the depth from the transducer, surface or keel", "[sk]")
{
    size_t n = map("$SDDPT,3.6,0.5,100*4A");
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 3.6, find(n, "environment.depth.belowTransducer")->number);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.5, find(n, "environment.depth.surfaceToTransducer")->number);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 4.1, find(n, "environment.depth.belowSurface")->number);

    n = map("$SDDPT,3.6,-1.2*7C");
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.2, find(n, "environment.depth.transducerToKeel")->number);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 2.4, find(n, "environment.depth.belowKeel")->number);
    TEST_ASSERT_NULL(find(n, "environment.depth.belowSurface"));

    n = map("$SDDBT,7.8,f,2.4,M,1.3,F*0D");
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 2.4, find(n, "environment.depth.belowTransducer")->number);
    n = map("$SDDBK,,f,-0.3,M,,F*37");
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, -0.3, find(n, "environment.depth.belowKeel")->number);
}

TEST_CASE("headings, water temperature, speed through water", "[sk]")
{
    size_t n = map("$HEHDT,274.07,T*19");
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_NOT_NULL(find(n, "navigation.headingTrue"));
    n = map("$HCHDG,98.3,0.0,E,12.6,W*57");
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_NOT_NULL(find(n, "navigation.headingMagnetic"));
    TEST_ASSERT_NOT_NULL(find(n, "navigation.magneticVariation"));
    n = map("$YXMTW,17.5,C*11");
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 290.65, find(n, "environment.water.temperature")->number);
    n = map("$VWVHW,,T,,M,5.2,N,9.6,K*5C");
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_NOT_NULL(find(n, "navigation.speedThroughWater"));
}

TEST_CASE("unknown sentences and a full buffer map to nothing past the limit", "[sk]")
{
    TEST_ASSERT_EQUAL(0, map("$PGRME,15.0,M,45.0,M,25.0,M*1C"));
    TEST_ASSERT_EQUAL(0, map("!AIVDM,1,1,,A,13aEOK?P00PD2wVMdLDRhgvL289?,0*26"));

    espos_nmea0183_sentence_t s;
    snprintf(buf, sizeof(buf), "%s", "$SDDPT,3.6,0.5,100*4A");
    espos_nmea0183_parse(buf, true, &s);
    TEST_ASSERT_EQUAL(2, espos_nmea0183_sk_map(&s, up, 2));
    TEST_ASSERT_EQUAL(0, espos_nmea0183_sk_map(&s, up, 0));
    TEST_ASSERT_EQUAL(0, espos_nmea0183_sk_map(NULL, up, 4));
}

TEST_CASE("only a valid RMC from 2020 on sets the clock", "[sk]")
{
    espos_nmea0183_sentence_t s;
    snprintf(buf, sizeof(buf), "%s", "$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191126,020.3,E*61");
    espos_nmea0183_parse(buf, true, &s);
    TEST_ASSERT_EQUAL_INT64(1795128886000LL, espos_nmea0183_sk_clock_ms(&s));

    /* 1994: a battery-backed clock after a week rollover, not a fix. */
    snprintf(buf, sizeof(buf), "%s", "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A");
    espos_nmea0183_parse(buf, true, &s);
    TEST_ASSERT_EQUAL_INT64(0, espos_nmea0183_sk_clock_ms(&s));

    snprintf(buf, sizeof(buf), "%s", "$GNRMC,083559.00,V,4717.11437,N,00833.91522,E,0.004,77.52,091226,,,N*57");
    espos_nmea0183_parse(buf, true, &s);
    TEST_ASSERT_EQUAL_INT64(0, espos_nmea0183_sk_clock_ms(&s));

    snprintf(buf, sizeof(buf), "%s", "$GPZDA,201530.00,04,07,2026,00,00*66");
    espos_nmea0183_parse(buf, true, &s);
    TEST_ASSERT_EQUAL_INT64(0, espos_nmea0183_sk_clock_ms(&s));
}

TEST_CASE("no navigation.datetime before 2020", "[sk]")
{
    TEST_ASSERT_EQUAL(0, map("$GPZDA,201530.00,04,07,2006,00,00*64"));
    size_t n = map("$GPZDA,201530.00,04,07,2026,00,00*66");
    TEST_ASSERT_EQUAL_STRING("2026-07-04T20:15:30.000Z", find(n, "navigation.datetime")->text);
    n = map("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A");
    TEST_ASSERT_NOT_NULL(find(n, "navigation.position"));
    TEST_ASSERT_NULL(find(n, "navigation.datetime"));
}

TEST_CASE("the clock is set once, then only after a drift", "[sk]")
{
    const int64_t gps = 1795128886000LL;
    TEST_ASSERT_FALSE(espos_nmea0183_sk_clock_due(0, 0, false));
    TEST_ASSERT_TRUE(espos_nmea0183_sk_clock_due(gps, 0, false));
    TEST_ASSERT_TRUE(espos_nmea0183_sk_clock_due(gps, gps, false));
    TEST_ASSERT_FALSE(espos_nmea0183_sk_clock_due(gps, gps - 400, true));
    TEST_ASSERT_FALSE(espos_nmea0183_sk_clock_due(gps, gps + ESPOS_NMEA0183_SK_CLOCK_SLACK_MS, true));
    TEST_ASSERT_TRUE(espos_nmea0183_sk_clock_due(gps, gps + ESPOS_NMEA0183_SK_CLOCK_SLACK_MS + 1, true));
    TEST_ASSERT_TRUE(espos_nmea0183_sk_clock_due(gps, gps - 60000, true));
}
