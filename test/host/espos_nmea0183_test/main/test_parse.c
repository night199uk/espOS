/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>

#include "espos_nmea0183.h"
#include "unity.h"

static espos_nmea0183_err_t parse(const char *text, bool require, espos_nmea0183_sentence_t *s)
{
    static char buf[256];
    snprintf(buf, sizeof(buf), "%s", text);
    return espos_nmea0183_parse(buf, require, s);
}

TEST_CASE("a GGA splits into talker, type and fields", "[parse]")
{
    espos_nmea0183_sentence_t s;
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_OK,
                      parse("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n", true, &s));
    TEST_ASSERT_EQUAL_STRING("GP", s.talker);
    TEST_ASSERT_EQUAL_STRING("GGA", s.type);
    TEST_ASSERT_TRUE(s.has_checksum);
    TEST_ASSERT_FALSE(s.proprietary);
    TEST_ASSERT_FALSE(s.encapsulated);
    /* Two trailing empty fields are fields too: station age and id. */
    TEST_ASSERT_EQUAL(14, s.n_fields);
    TEST_ASSERT_EQUAL_STRING("123519", s.field[0]);
    TEST_ASSERT_EQUAL_STRING("E", s.field[4]);
    TEST_ASSERT_EQUAL_STRING("", s.field[13]);
    TEST_ASSERT_TRUE(espos_nmea0183_is(&s, "GGA"));
    TEST_ASSERT_FALSE(espos_nmea0183_is(&s, "RMC"));
}

TEST_CASE("a wrong or malformed checksum is refused, a missing one only when required", "[parse]")
{
    espos_nmea0183_sentence_t s;
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_BAD_CHECKSUM, parse("$HEHDT,274.07,T*18", false, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_BAD_CHECKSUM, parse("$HEHDT,274.07,T*1", false, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_BAD_CHECKSUM, parse("$HEHDT,274.07,T*1G", false, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_BAD_CHECKSUM, parse("$HEHDT,274.07,T*19X", false, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_OK, parse("$HEHDT,274.07,T*19", true, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_OK, parse("$HEHDT,274.07,T*19", false, &s));
    /* Lower-case hex is what some talkers send. */
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_OK, parse("$SDDBT,7.8,f,2.4,M,1.3,F*0d", true, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_NO_CHECKSUM, parse("$HEHDT,274.07,T", true, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_OK, parse("$HEHDT,274.07,T", false, &s));
    TEST_ASSERT_FALSE(s.has_checksum);
}

TEST_CASE("junk is refused with a reason", "[parse]")
{
    espos_nmea0183_sentence_t s;
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_EMPTY, parse("  \r\n", false, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_EMPTY, espos_nmea0183_parse(NULL, false, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_NO_START, parse("GPGGA,1,2", false, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_BAD_CHAR, parse("$GPGGA,1\x01,2", false, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_BAD_ADDRESS, parse("$GPGG,1,2", false, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_BAD_ADDRESS, parse("$gpgga,1,2", false, &s));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_BAD_ADDRESS, parse("$,1,2", false, &s));
    TEST_ASSERT_EQUAL_STRING("bad checksum", espos_nmea0183_err_str(ESPOS_NMEA0183_ERR_BAD_CHECKSUM));

    char longline[ESPOS_NMEA0183_LINE_MAX + 8] = "$GPTXT,";
    memset(longline + 7, 'A', sizeof(longline) - 8);
    longline[sizeof(longline) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_TOO_LONG, espos_nmea0183_parse(longline, false, &s));

    char many[256] = "$GPXXX";
    for (int i = 0; i < ESPOS_NMEA0183_MAX_FIELDS + 1; i++) {
        strcat(many, ",1");
    }
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_ERR_TOO_MANY_FIELDS, espos_nmea0183_parse(many, false, &s));
}

TEST_CASE("proprietary and encapsulated sentences are told apart", "[parse]")
{
    espos_nmea0183_sentence_t s;
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_OK, parse("$PGRME,15.0,M,45.0,M,25.0,M*1C", true, &s));
    TEST_ASSERT_TRUE(s.proprietary);
    TEST_ASSERT_EQUAL_STRING("", s.talker);
    TEST_ASSERT_EQUAL_STRING("GRME", s.type);
    TEST_ASSERT_FALSE(espos_nmea0183_is(&s, "GRME"));

    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_OK, parse("!AIVDM,1,1,,A,13aEOK?P00PD2wVMdLDRhgvL289?,0*26", true, &s));
    TEST_ASSERT_TRUE(s.encapsulated);
    TEST_ASSERT_EQUAL_STRING("AI", s.talker);
    TEST_ASSERT_EQUAL_STRING("VDM", s.type);
    TEST_ASSERT_EQUAL(6, s.n_fields);
}

TEST_CASE("finish adds the checksum and CR LF, and replaces a stale one", "[parse]")
{
    char out[96];
    size_t n = espos_nmea0183_finish("$HEHDT,274.07,T", out, sizeof(out));
    TEST_ASSERT_EQUAL(strlen("$HEHDT,274.07,T*19\r\n"), n);
    TEST_ASSERT_EQUAL_STRING("$HEHDT,274.07,T*19\r\n", out);
    TEST_ASSERT_EQUAL(n, espos_nmea0183_finish("$HEHDT,274.07,T*00\r\n", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("$HEHDT,274.07,T*19\r\n", out);
    TEST_ASSERT_EQUAL(0, espos_nmea0183_finish("HEHDT,274.07,T", out, sizeof(out)));
    TEST_ASSERT_EQUAL(0, espos_nmea0183_finish("$HEHDT,274.07,T", out, 10));

    /* What finish makes, parse accepts. */
    espos_nmea0183_sentence_t s;
    espos_nmea0183_finish("$IIXDR,C,19.5,C,AIR", out, sizeof(out));
    TEST_ASSERT_EQUAL(ESPOS_NMEA0183_OK, espos_nmea0183_parse(out, true, &s));
}

/* ------------------------------------------------------------------------ */

typedef struct {
    char lines[8][ESPOS_NMEA0183_LINE_MAX + 1];
    int n;
} collected_t;

static void collect(char *line, size_t len, void *arg)
{
    collected_t *c = arg;
    TEST_ASSERT_EQUAL(strlen(line), len);
    if (c->n < 8) {
        snprintf(c->lines[c->n++], sizeof(c->lines[0]), "%s", line);
    }
}

static void feed(espos_nmea0183_reader_t *r, const char *bytes, collected_t *c)
{
    espos_nmea0183_reader_feed(r, (const uint8_t *)bytes, strlen(bytes), collect, c);
}

TEST_CASE("the reader assembles lines across reads and any line ending", "[reader]")
{
    espos_nmea0183_reader_t r;
    espos_nmea0183_reader_init(&r);
    collected_t c = { 0 };
    feed(&r, "$HEHDT,27", &c);
    TEST_ASSERT_EQUAL(0, c.n);
    feed(&r, "4.07,T*19\r\n$HCHDM,238.5,M*25\n\n\r$SDDPT,3.6,0.5,100*4A\r", &c);
    TEST_ASSERT_EQUAL(3, c.n);
    TEST_ASSERT_EQUAL_STRING("$HEHDT,274.07,T*19", c.lines[0]);
    TEST_ASSERT_EQUAL_STRING("$HCHDM,238.5,M*25", c.lines[1]);
    TEST_ASSERT_EQUAL_STRING("$SDDPT,3.6,0.5,100*4A", c.lines[2]);
    TEST_ASSERT_EQUAL(3, r.lines);
}

TEST_CASE("a start character mid-line drops the cut-off part", "[reader]")
{
    espos_nmea0183_reader_t r;
    espos_nmea0183_reader_init(&r);
    collected_t c = { 0 };
    /* Power-up noise, then a talker that restarted mid-sentence. */
    feed(&r, "\xff\xfe$GPGGA,1235$HEHDT,274.07,T*19\r\n", &c);
    TEST_ASSERT_EQUAL(1, c.n);
    TEST_ASSERT_EQUAL_STRING("$HEHDT,274.07,T*19", c.lines[0]);
    /* The noise before the first start character is skipped, not a line. */
    TEST_ASSERT_EQUAL(1, r.resyncs);
}

TEST_CASE("an over-long line is dropped whole, and the next one is fine", "[reader]")
{
    espos_nmea0183_reader_t r;
    espos_nmea0183_reader_init(&r);
    collected_t c = { 0 };
    char big[ESPOS_NMEA0183_LINE_MAX + 40];
    memset(big, 'A', sizeof(big) - 1);
    big[0] = '$';
    big[sizeof(big) - 1] = '\0';
    feed(&r, big, &c);
    feed(&r, "AAAA\r\n$HEHDT,274.07,T*19\r\n", &c);
    TEST_ASSERT_EQUAL(1, c.n);
    TEST_ASSERT_EQUAL_STRING("$HEHDT,274.07,T*19", c.lines[0]);
    TEST_ASSERT_EQUAL(1, r.overflows);

    /* A start character inside the over-long line ends the discard early. */
    feed(&r, big, &c);
    feed(&r, "$HCHDM,238.5,M*25\r\n", &c);
    TEST_ASSERT_EQUAL(2, c.n);
    TEST_ASSERT_EQUAL_STRING("$HCHDM,238.5,M*25", c.lines[1]);
}

TEST_CASE("a NUL or another control byte inside a line drops it", "[reader]")
{
    espos_nmea0183_reader_t r;
    espos_nmea0183_reader_init(&r);
    collected_t c = { 0 };
    /* A break on the wire mid-RMC: what is left would still parse. */
    static const uint8_t cut[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,08\0"
                                 "4.4,230394,003.1,W*6A\r\n";
    espos_nmea0183_reader_feed(&r, cut, sizeof(cut) - 1, collect, &c);
    TEST_ASSERT_EQUAL(0, c.n);
    TEST_ASSERT_EQUAL(1, r.garbled);
    /* Noise between lines is skipped and counted as nothing. */
    feed(&r, "\x01\x7f\xff$HEHDT,274.07,T*19\r\n", &c);
    TEST_ASSERT_EQUAL(1, c.n);
    TEST_ASSERT_EQUAL(1, r.garbled);
    TEST_ASSERT_EQUAL(0, r.resyncs);
}

TEST_CASE("a start character that ends a dropped line is not also a resync", "[reader]")
{
    espos_nmea0183_reader_t r;
    espos_nmea0183_reader_init(&r);
    collected_t c = { 0 };
    char big[ESPOS_NMEA0183_LINE_MAX + 40];
    memset(big, 'A', sizeof(big) - 1);
    big[0] = '$';
    big[sizeof(big) - 1] = '\0';
    feed(&r, big, &c);
    feed(&r, "$HCHDM,238.5,M*25\r\n", &c);
    TEST_ASSERT_EQUAL(1, c.n);
    TEST_ASSERT_EQUAL(1, r.overflows);
    TEST_ASSERT_EQUAL(0, r.resyncs);
}
