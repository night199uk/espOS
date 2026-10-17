/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "espos_nmea0183.h"
#include "unity.h"

#define DEG(x) ((x) * M_PI / 180.0)
#define KN(x)  ((x) * 1852.0 / 3600.0)

static char buf[256];

static espos_nmea0183_sentence_t sentence(const char *text)
{
    espos_nmea0183_sentence_t s;
    snprintf(buf, sizeof(buf), "%s", text);
    TEST_ASSERT_EQUAL_MESSAGE(ESPOS_NMEA0183_OK, espos_nmea0183_parse(buf, true, &s), text);
    return s;
}

TEST_CASE("fields: numbers, latitude and longitude, time", "[fields]")
{
    espos_nmea0183_sentence_t s = sentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    double v;
    TEST_ASSERT_TRUE(espos_nmea0183_field_double(&s, 7, &v));
    TEST_ASSERT_EQUAL_DOUBLE(0.9, v);
    TEST_ASSERT_FALSE(espos_nmea0183_field_double(&s, 12, &v)); /* empty */
    TEST_ASSERT_FALSE(espos_nmea0183_field_double(&s, 2, &v));  /* "N" */
    TEST_ASSERT_FALSE(espos_nmea0183_field_double(&s, 99, &v)); /* past the end */
    TEST_ASSERT_TRUE(espos_nmea0183_field_latlon(&s, 1, &v));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 48.0 + 7.038 / 60.0, v);
    TEST_ASSERT_TRUE(espos_nmea0183_field_latlon(&s, 3, &v));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 11.0 + 31.0 / 60.0, v);
    int64_t ms;
    TEST_ASSERT_TRUE(espos_nmea0183_field_time(&s, 0, &ms));
    TEST_ASSERT_EQUAL_INT64((12 * 3600 + 35 * 60 + 19) * 1000LL, ms);
    TEST_ASSERT_EQUAL('N', espos_nmea0183_field_char(&s, 2));
    TEST_ASSERT_EQUAL('\0', espos_nmea0183_field_char(&s, 13));

    /* South and west are negative; minutes of 60 or more are not a place. */
    s = sentence("$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191126,020.3,E*61");
    TEST_ASSERT_TRUE(espos_nmea0183_field_latlon(&s, 4, &v));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, -(123.0 + 11.12 / 60.0), v);
}

TEST_CASE("civil dates become unix milliseconds", "[fields]")
{
    TEST_ASSERT_EQUAL_INT64(0, espos_nmea0183_unix_ms(1970, 1, 1, 0));
    TEST_ASSERT_EQUAL_INT64(951782400000LL, espos_nmea0183_unix_ms(2000, 2, 29, 0));
    TEST_ASSERT_EQUAL_INT64(1783196130000LL, espos_nmea0183_unix_ms(2026, 7, 4, (20 * 3600 + 15 * 60 + 30) * 1000LL));
    TEST_ASSERT_EQUAL_INT64(-1, espos_nmea0183_unix_ms(2026, 2, 29, 0));
    TEST_ASSERT_EQUAL_INT64(-1, espos_nmea0183_unix_ms(2026, 13, 1, 0));
    TEST_ASSERT_EQUAL_INT64(-1, espos_nmea0183_unix_ms(1969, 12, 31, 0));
}

TEST_CASE("GGA with a fix reports where, without one only how many satellites", "[gga]")
{
    espos_nmea0183_sentence_t s = sentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    espos_nmea0183_gga_t g;
    TEST_ASSERT_TRUE(espos_nmea0183_decode_gga(&s, &g));
    TEST_ASSERT_TRUE(g.has_position);
    TEST_ASSERT_EQUAL(1, g.quality);
    TEST_ASSERT_EQUAL(8, g.satellites);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 545.4, g.altitude);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 46.9, g.geoid_separation);

    s = sentence("$GPGGA,123519,4807.038,N,01131.000,E,0,00,,,M,,M,,*52");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_gga(&s, &g));
    TEST_ASSERT_FALSE(g.has_position);
    TEST_ASSERT_FALSE(g.has_hdop);
    TEST_ASSERT_TRUE(g.has_satellites);
    TEST_ASSERT_EQUAL(0, g.satellites);

    /* The wrong sentence decodes as nothing. */
    s = sentence("$HEHDT,274.07,T*19");
    TEST_ASSERT_FALSE(espos_nmea0183_decode_gga(&s, &g));
}

TEST_CASE("RMC: units, date, variation, and nothing from a void fix", "[rmc]")
{
    espos_nmea0183_sentence_t s = sentence("$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191126,020.3,E*61");
    espos_nmea0183_rmc_t r;
    TEST_ASSERT_TRUE(espos_nmea0183_decode_rmc(&s, &r));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 49.0 + 16.45 / 60.0, r.lat);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, KN(0.5), r.sog);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, DEG(54.7), r.cog_true);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, DEG(20.3), r.variation);
    TEST_ASSERT_TRUE(r.has_datetime);
    TEST_ASSERT_EQUAL_INT64(espos_nmea0183_unix_ms(2026, 11, 19, (22 * 3600 + 54 * 60 + 46) * 1000LL), r.unix_ms);

    /* West variation is negative. */
    s = sentence("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_rmc(&s, &r));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, DEG(-3.1), r.variation);
    TEST_ASSERT_EQUAL_INT64(espos_nmea0183_unix_ms(1994, 3, 23, (12 * 3600 + 35 * 60 + 19) * 1000LL), r.unix_ms);

    /* NMEA 2.3 mode N: the status letter alone is not trusted. */
    s = sentence("$GNRMC,083559.00,V,4717.11437,N,00833.91522,E,0.004,77.52,091226,,,N*57");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_rmc(&s, &r));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_FALSE(r.has_position);
    TEST_ASSERT_FALSE(r.has_sog);
    TEST_ASSERT_EQUAL('N', r.mode);
}

TEST_CASE("VTG in the 2.x and the old 1.x layouts", "[vtg]")
{
    espos_nmea0183_vtg_t v;
    espos_nmea0183_sentence_t s = sentence("$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K,A*25");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_vtg(&s, &v));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, DEG(54.7), v.cog_true);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, DEG(34.4), v.cog_magnetic);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, KN(5.5), v.sog);

    s = sentence("$GPVTG,054.7,034.4,005.5,010.2*54");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_vtg(&s, &v));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, DEG(34.4), v.cog_magnetic);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, KN(5.5), v.sog);

    s = sentence("$GPVTG,,T,,M,0.000,N,0.000,K,N*2C");
    TEST_ASSERT_FALSE(espos_nmea0183_decode_vtg(&s, &v));
}

TEST_CASE("headings: HDT, HDM, and HDG with deviation and variation", "[heading]")
{
    espos_nmea0183_heading_t h;
    espos_nmea0183_sentence_t s = sentence("$HEHDT,274.07,T*19");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_heading(&s, &h));
    TEST_ASSERT_TRUE(h.has_true);
    TEST_ASSERT_FALSE(h.has_magnetic);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, DEG(274.07), h.heading_true);

    s = sentence("$HCHDM,238.5,M*25");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_heading(&s, &h));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, DEG(238.5), h.heading_magnetic);

    s = sentence("$HCHDG,98.3,0.0,E,12.6,W*57");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_heading(&s, &h));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, DEG(98.3), h.heading_magnetic);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, DEG(-12.6), h.variation);

    /* Deviation wraps past north, and no variation is no variation. */
    s = sentence("$HCHDG,350.0,15.0,E,,*1B");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_heading(&s, &h));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, DEG(5.0), h.heading_magnetic);
    TEST_ASSERT_FALSE(h.has_variation);
}

TEST_CASE("depth: DPT offsets, and DBT, DBS, DBK in whichever unit is filled", "[depth]")
{
    espos_nmea0183_depth_t d;
    espos_nmea0183_sentence_t s = sentence("$SDDPT,3.6,0.5,100*4A");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_depth(&s, &d));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_DEPTH_TRANSDUCER, d.below);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 3.6, d.depth);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.5, d.offset);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 100, d.range);

    s = sentence("$SDDBT,7.8,f,2.4,M,1.3,F*0D");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_depth(&s, &d));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 2.4, d.depth); /* metres win over feet */

    s = sentence("$SDDBT,7.8,f,,M,,F*09");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_depth(&s, &d));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 7.8 * 0.3048, d.depth);

    s = sentence("$SDDBS,,f,,M,2.0,F*03");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_depth(&s, &d));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_DEPTH_SURFACE, d.below);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 2.0 * 1.8288, d.depth);

    /* Aground: below the keel goes negative and is still a depth. */
    s = sentence("$SDDBK,,f,-0.3,M,,F*37");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_depth(&s, &d));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_DEPTH_KEEL, d.below);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, -0.3, d.depth);
}

TEST_CASE("MTW, VHW and ZDA", "[misc]")
{
    espos_nmea0183_mtw_t m;
    espos_nmea0183_sentence_t s = sentence("$YXMTW,17.5,C*11");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_mtw(&s, &m));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 290.65, m.temperature);

    espos_nmea0183_vhw_t w;
    s = sentence("$VWVHW,,T,,M,5.2,N,9.6,K*5C");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_vhw(&s, &w));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, KN(5.2), w.stw);
    TEST_ASSERT_FALSE(w.has_true);

    espos_nmea0183_zda_t z;
    s = sentence("$GPZDA,201530.00,04,07,2026,00,00*66");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_zda(&s, &z));
    TEST_ASSERT_EQUAL_INT64(1783196130000LL, z.unix_ms);
}

TEST_CASE("a simulator or an estimate is not a fix", "[gga][rmc]")
{
    espos_nmea0183_sentence_t s = sentence("$GPGGA,123519,4807.038,N,01131.000,E,8,08,0.9,545.4,M,46.9,M,,*4E");
    espos_nmea0183_gga_t g;
    TEST_ASSERT_TRUE(espos_nmea0183_decode_gga(&s, &g));
    TEST_ASSERT_EQUAL(8, g.quality);
    TEST_ASSERT_FALSE(g.has_position);

    espos_nmea0183_rmc_t r;
    s = sentence("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,191126,003.1,W,S*16");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_rmc(&s, &r));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_FALSE(r.has_position);
}

TEST_CASE("an RMC with only a variation still reports it", "[rmc]")
{
    espos_nmea0183_sentence_t s = sentence("$GPRMC,,V,,,,,,,,003.1,W,N*28");
    espos_nmea0183_rmc_t r;
    TEST_ASSERT_TRUE(espos_nmea0183_decode_rmc(&s, &r));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.has_variation);
}

TEST_CASE("GST: the receiver's error estimate, and no accuracy from empty fields", "[gst]")
{
    espos_nmea0183_sentence_t s = sentence("$GPGST,172814.0,0.006,0.023,0.020,273.6,0.023,0.020,0.031*6A");
    espos_nmea0183_gst_t g;
    TEST_ASSERT_TRUE(espos_nmea0183_decode_gst(&s, &g));
    TEST_ASSERT_TRUE(g.has_time);
    TEST_ASSERT_EQUAL_INT64((17 * 3600 + 28 * 60 + 14) * 1000LL, g.time_ms);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.006, g.rms);
    TEST_ASSERT_TRUE(g.has_lat_lon);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.023, g.lat_sd);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.020, g.lon_sd);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.030479501308256, g.h_accuracy); /* sqrt(0.023² + 0.020²) */
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.031, g.altitude_sd);

    /* A receiver without a fix sends the sentence with nothing in it. */
    s = sentence("$GNGST,172814.0,,,,,,,*5E");
    TEST_ASSERT_FALSE(espos_nmea0183_decode_gst(&s, &g));
    TEST_ASSERT_FALSE(g.has_lat_lon);

    s = sentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    TEST_ASSERT_FALSE(espos_nmea0183_decode_gst(&s, &g));
}

TEST_CASE("absurd numbers that pass the checksum are refused, not looped on", "[fields]")
{
    /* A deviation and variation of 1e300 degrees: no heading, and above all
     * no endless loop stepping it back into a turn. */
    espos_nmea0183_sentence_t s = sentence("$HCHDG,98.3,1e300,E,1e300,W*62");
    espos_nmea0183_heading_t h;
    TEST_ASSERT_TRUE(espos_nmea0183_decode_heading(&s, &h));
    TEST_ASSERT_TRUE(h.has_magnetic); /* the sensor alone, deviation left out */
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, DEG(98.3), h.heading_magnetic);
    TEST_ASSERT_FALSE(h.has_variation);

    double v;
    s = sentence("$GPGGA,123519,1e300,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*3E");
    TEST_ASSERT_FALSE(espos_nmea0183_field_latlon(&s, 1, &v));
    int64_t ms;
    s = sentence("$GPGGA,123519.5e300,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*0A");
    TEST_ASSERT_FALSE(espos_nmea0183_field_time(&s, 0, &ms));
}

TEST_CASE("only plain decimals are numbers, and only sane ones are values", "[fields][gga][gst]")
{
    /* strtod would read 0x10 as 16 and hand GST a made-up accuracy. */
    espos_nmea0183_sentence_t s = sentence("$GPGST,172814.0,0x10,0.023,0.020,273.6,0x10,0x10,0.031*08");
    espos_nmea0183_gst_t t;
    TEST_ASSERT_TRUE(espos_nmea0183_decode_gst(&s, &t));
    TEST_ASSERT_FALSE(t.has_rms);
    TEST_ASSERT_FALSE(t.has_lat_lon);
    TEST_ASSERT_TRUE(t.has_altitude);

    /* 1000 km and more is a talker bug, and its square would not stay finite. */
    s = sentence("$GPGST,172814.0,0.006,0.023,0.020,273.6,1000000000,1000000000,0.031*69");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_gst(&s, &t));
    TEST_ASSERT_FALSE(t.has_lat_lon);
    TEST_ASSERT_EQUAL_DOUBLE(0, t.h_accuracy);

    espos_nmea0183_gga_t g;
    s = sentence("$GPGGA,123519,4807.038,N,01131.000,E,1,08,-0.5,545.4,M,46.9,M,,*66");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_gga(&s, &g));
    TEST_ASSERT_TRUE(g.has_position);
    TEST_ASSERT_FALSE(g.has_hdop);

    /* Without a fix the height is as stale as the position. */
    s = sentence("$GPGGA,123519,4807.038,N,01131.000,E,0,08,-1.0,545.4,M,46.9,M,,*63");
    TEST_ASSERT_TRUE(espos_nmea0183_decode_gga(&s, &g));
    TEST_ASSERT_FALSE(g.has_position);
    TEST_ASSERT_FALSE(g.has_altitude);
    TEST_ASSERT_FALSE(g.has_geoid);
}
