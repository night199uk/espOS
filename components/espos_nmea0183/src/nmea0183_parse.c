/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Framing, fields and the sentence decoders. Pure C: host-tested in
 * test/host/espos_nmea0183_test.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espos_nmea0183.h"

#define KNOT_MS   (1852.0 / 3600.0)
#define KMH_MS    (1000.0 / 3600.0)
#define FOOT_M    0.3048
#define FATHOM_M  1.8288
#define CELSIUS_K 273.15

static const double DEG_RAD = M_PI / 180.0;

const char *espos_nmea0183_err_str(espos_nmea0183_err_t err)
{
    switch (err) {
    case ESPOS_NMEA0183_OK:
        return "ok";
    case ESPOS_NMEA0183_ERR_EMPTY:
        return "empty";
    case ESPOS_NMEA0183_ERR_NO_START:
        return "no start character";
    case ESPOS_NMEA0183_ERR_TOO_LONG:
        return "too long";
    case ESPOS_NMEA0183_ERR_BAD_CHAR:
        return "bad character";
    case ESPOS_NMEA0183_ERR_NO_CHECKSUM:
        return "no checksum";
    case ESPOS_NMEA0183_ERR_BAD_CHECKSUM:
        return "bad checksum";
    case ESPOS_NMEA0183_ERR_BAD_ADDRESS:
        return "bad address";
    case ESPOS_NMEA0183_ERR_TOO_MANY_FIELDS:
        return "too many fields";
    case ESPOS_NMEA0183_ERR_MAX:
        break;
    }
    return "unknown";
}

uint8_t espos_nmea0183_checksum(const char *body, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum ^= (uint8_t)body[i];
    }
    return sum;
}

size_t espos_nmea0183_finish(const char *line, char *out, size_t n)
{
    if (!line || !out || (line[0] != '$' && line[0] != '!')) {
        return 0;
    }
    size_t len = strcspn(line, "*\r\n");
    /* body + "*hh\r\n" + NUL */
    if (len < 2 || len + 3 > ESPOS_NMEA0183_LINE_MAX || len + 6 > n) {
        return 0;
    }
    memcpy(out, line, len);
    int w = snprintf(out + len, n - len, "*%02X\r\n", espos_nmea0183_checksum(line + 1, len - 1));
    return w == 5 ? len + 5 : 0;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

espos_nmea0183_err_t espos_nmea0183_parse(char *line, bool require_checksum, espos_nmea0183_sentence_t *out)
{
    if (!line || !out) {
        return ESPOS_NMEA0183_ERR_EMPTY;
    }
    memset(out, 0, sizeof(*out));

    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return ESPOS_NMEA0183_ERR_EMPTY;
    }
    if (line[0] != '$' && line[0] != '!') {
        return ESPOS_NMEA0183_ERR_NO_START;
    }
    if (len > ESPOS_NMEA0183_LINE_MAX) {
        return ESPOS_NMEA0183_ERR_TOO_LONG;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c < 0x20 || c > 0x7e) {
            return ESPOS_NMEA0183_ERR_BAD_CHAR;
        }
    }

    size_t body_end = len;
    char *star = strchr(line, '*');
    if (star) {
        /* Exactly two hex digits, and nothing after them. */
        if ((size_t)(star - line) + 3 != len) {
            return ESPOS_NMEA0183_ERR_BAD_CHECKSUM;
        }
        int hi = hex_val(star[1]), lo = hex_val(star[2]);
        if (hi < 0 || lo < 0) {
            return ESPOS_NMEA0183_ERR_BAD_CHECKSUM;
        }
        if (espos_nmea0183_checksum(line + 1, (size_t)(star - line) - 1) != (uint8_t)(hi << 4 | lo)) {
            return ESPOS_NMEA0183_ERR_BAD_CHECKSUM;
        }
        out->has_checksum = true;
        body_end = (size_t)(star - line);
        *star = '\0';
    } else if (require_checksum) {
        return ESPOS_NMEA0183_ERR_NO_CHECKSUM;
    }
    out->encapsulated = line[0] == '!';

    /* Split in place: every comma ends a field. */
    char *addr = line + 1;
    char *p = strchr(addr, ',');
    if (p) {
        *p++ = '\0';
    }
    size_t alen = strlen(addr);
    for (size_t i = 0; i < alen; i++) {
        if (!isupper((unsigned char)addr[i]) && !isdigit((unsigned char)addr[i])) {
            return ESPOS_NMEA0183_ERR_BAD_ADDRESS;
        }
    }
    if (alen >= 2 && addr[0] == 'P') {
        /* Proprietary: P + a three-letter manufacturer code + its own type. */
        if (alen - 1 >= sizeof(out->type)) {
            return ESPOS_NMEA0183_ERR_BAD_ADDRESS;
        }
        out->proprietary = true;
        memcpy(out->type, addr + 1, alen - 1);
    } else if (alen == 5) {
        memcpy(out->talker, addr, 2);
        memcpy(out->type, addr + 2, 3);
    } else {
        return ESPOS_NMEA0183_ERR_BAD_ADDRESS;
    }

    while (p && p <= line + body_end) {
        if (out->n_fields >= ESPOS_NMEA0183_MAX_FIELDS) {
            return ESPOS_NMEA0183_ERR_TOO_MANY_FIELDS;
        }
        out->field[out->n_fields++] = p;
        p = strchr(p, ',');
        if (p) {
            *p++ = '\0';
        }
    }
    return ESPOS_NMEA0183_OK;
}

bool espos_nmea0183_is(const espos_nmea0183_sentence_t *s, const char *type)
{
    return s && type && !s->proprietary && strcmp(s->type, type) == 0;
}

/* ------------------------------------------------------------------------ */

void espos_nmea0183_reader_init(espos_nmea0183_reader_t *r)
{
    memset(r, 0, sizeof(*r));
}

void espos_nmea0183_reader_feed(espos_nmea0183_reader_t *r, const uint8_t *data, size_t n,
                                espos_nmea0183_line_fn fn, void *arg)
{
    for (size_t i = 0; i < n; i++) {
        char c = (char)data[i];
        if (c == '\r' || c == '\n') {
            if (r->discarding) {
                r->discarding = false;
            } else if (r->len > 0) {
                r->buf[r->len] = '\0';
                r->lines++;
                if (fn) {
                    fn(r->buf, r->len, arg);
                }
            }
            r->len = 0;
            continue;
        }
        if (c == '$' || c == '!') {
            /* A start character mid-line: what came before was cut off. Not
             * counted when it ends a discard: that line is already counted. */
            if (r->len > 0 && !r->discarding) {
                r->resyncs++;
            }
            r->discarding = false;
            r->len = 0;
        }
        if (r->discarding) {
            continue;
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) {
            /* A NUL or a framing error inside a line would cut it short for
             * strlen() without a trace, and the shorter line can still parse.
             * Between lines it is only noise. */
            if (r->len > 0) {
                r->garbled++;
                r->discarding = true;
                r->len = 0;
            }
            continue;
        }
        if (r->len >= ESPOS_NMEA0183_LINE_MAX) {
            r->overflows++;
            r->discarding = true;
            r->len = 0;
            continue;
        }
        r->buf[r->len++] = c;
    }
}

/* ------------------------------------------------------------------------ */

static const char *field(const espos_nmea0183_sentence_t *s, int i)
{
    if (!s || i < 0 || i >= s->n_fields || !s->field[i] || !s->field[i][0]) {
        return NULL;
    }
    return s->field[i];
}

bool espos_nmea0183_field_double(const espos_nmea0183_sentence_t *s, int32_t i, double *out)
{
    const char *f = field(s, i);
    if (!f) {
        return false;
    }
    /* NMEA numbers are plain decimals; strtod would also take hex, exponents
     * and leading blanks, so "0x10" would read as 16. */
    for (const char *c = f; *c; c++) {
        if (!(isdigit((unsigned char)*c) || *c == '.' || *c == '-' || *c == '+')) {
            return false;
        }
    }
    char *end = NULL;
    double v = strtod(f, &end);
    if (end == f || *end != '\0' || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

char espos_nmea0183_field_char(const espos_nmea0183_sentence_t *s, int32_t i)
{
    const char *f = field(s, i);
    return f ? f[0] : '\0';
}

static bool field_int(const espos_nmea0183_sentence_t *s, int i, int32_t *out)
{
    const char *f = field(s, i);
    if (!f) {
        return false;
    }
    char *end = NULL;
    long v = strtol(f, &end, 10);
    if (end == f || *end != '\0' || v < INT32_MIN || v > INT32_MAX) {
        return false;
    }
    *out = (int32_t)v;
    return true;
}

bool espos_nmea0183_field_latlon(const espos_nmea0183_sentence_t *s, int32_t i, double *out_deg)
{
    double v;
    char hemi = espos_nmea0183_field_char(s, i + 1);
    /* 18000 is 180°00': anything bigger is no place, and must not reach the
     * integer conversion below. */
    if (!espos_nmea0183_field_double(s, i, &v) || v < 0 || v >= 18100.0) {
        return false;
    }
    double deg = (double)(long)(v / 100.0); /* v >= 0: truncation is floor */
    double min = v - deg * 100.0;
    if (min >= 60.0) {
        return false;
    }
    deg += min / 60.0;
    switch (hemi) {
    case 'N':
    case 'S':
        if (deg > 90.0) {
            return false;
        }
        break;
    case 'E':
    case 'W':
        if (deg > 180.0) {
            return false;
        }
        break;
    default:
        return false;
    }
    *out_deg = (hemi == 'S' || hemi == 'W') ? -deg : deg;
    return true;
}

static int digits(const char *p, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (!isdigit((unsigned char)p[i])) {
            return -1;
        }
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

bool espos_nmea0183_field_time(const espos_nmea0183_sentence_t *s, int32_t i, int64_t *out_ms)
{
    const char *f = field(s, i);
    if (!f || strlen(f) < 6) {
        return false;
    }
    int hh = digits(f, 2), mm = digits(f + 2, 2), ss = digits(f + 4, 2);
    if (hh < 0 || mm < 0 || ss < 0 || hh > 23 || mm > 59 || ss > 60) {
        return false;
    }
    double frac = 0;
    if (f[6] != '\0') {
        /* Digits only: strtod would take an exponent, and "05.5e300" must
         * not reach the integer conversion. */
        if (f[6] != '.' || f[7] == '\0' || strspn(f + 7, "0123456789") != strlen(f + 7)) {
            return false;
        }
        char *end = NULL;
        frac = strtod(f + 6, &end);
        if (*end != '\0') {
            return false;
        }
    }
    *out_ms = ((int64_t)hh * 3600 + mm * 60 + ss) * 1000 + (int64_t)(frac * 1000.0 + 0.5);
    return true;
}

int64_t espos_nmea0183_unix_ms(int32_t year, int32_t month, int32_t day, int64_t ms_of_day)
{
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (year < 1970 || month < 1 || month > 12 || day < 1 || ms_of_day < 0 || ms_of_day >= 86401000) {
        return -1;
    }
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (day > mdays[month - 1] + (month == 2 && leap ? 1 : 0)) {
        return -1;
    }
    /* Days from civil, H. Hinnant: exact for every proleptic Gregorian date. */
    int y = year - (month <= 2);
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return days * 86400000LL + ms_of_day;
}

/* An angle in degrees as radians in [0, 2π). Loops rather than fmod(): the
 * inputs are at most a turn or two out, and the host tests link no libm. */
static double rad_of(double deg)
{
    while (deg < 0) {
        deg += 360.0;
    }
    while (deg >= 360.0) {
        deg -= 360.0;
    }
    return deg * DEG_RAD;
}

/* A variation or deviation and its E/W letter as signed radians, + east. */
static bool signed_angle(const espos_nmea0183_sentence_t *s, int i, double *out)
{
    double v;
    char ew = espos_nmea0183_field_char(s, i + 1);
    /* Bounded, because rad_of() steps a heading plus this deviation back
     * into a turn: 1e300 would never step back. */
    if (!espos_nmea0183_field_double(s, i, &v) || v < 0 || v > 180.0 || (ew != 'E' && ew != 'W')) {
        return false;
    }
    *out = (ew == 'W' ? -v : v) * DEG_RAD;
    return true;
}

static bool angle(const espos_nmea0183_sentence_t *s, int i, double *out)
{
    double v;
    if (!espos_nmea0183_field_double(s, i, &v) || v < 0 || v > 360.0) {
        return false;
    }
    *out = rad_of(v);
    return true;
}

static bool position(const espos_nmea0183_sentence_t *s, int i, double *lat, double *lon)
{
    return espos_nmea0183_field_latlon(s, i, lat) && espos_nmea0183_field_latlon(s, i + 2, lon);
}

/* ------------------------------------------------------------------------ */

bool espos_nmea0183_decode_gga(const espos_nmea0183_sentence_t *s, espos_nmea0183_gga_t *out)
{
    if (!espos_nmea0183_is(s, "GGA") || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->has_time = espos_nmea0183_field_time(s, 0, &out->time_ms);
    if (!field_int(s, 5, &out->quality) || out->quality < 0) {
        out->quality = 0;
    }
    /* Without a fix a receiver repeats its last position: not a place. Dead
     * reckoning, manual input and a simulator (6 to 8) are not fixes either. */
    bool fix = out->quality >= 1 && out->quality <= 5;
    out->has_position = fix && position(s, 1, &out->lat, &out->lon);
    out->has_satellites = field_int(s, 6, &out->satellites) && out->satellites >= 0;
    out->has_hdop = espos_nmea0183_field_double(s, 7, &out->hdop) && out->hdop >= 0;
    /* The height belongs to the fix: without one it is as stale as the
     * repeated position. */
    out->has_altitude = fix && espos_nmea0183_field_double(s, 8, &out->altitude);
    out->has_geoid = fix && espos_nmea0183_field_double(s, 10, &out->geoid_separation);
    return out->has_time || out->has_position || out->has_satellites;
}

/* An NMEA 2.3+ mode indicator that means a fix: autonomous, differential,
 * RTK float, RTK fixed, precise. Not E (estimated), M (manual), S (simulator)
 * or N. No indicator at all is a talker older than 2.3. */
static bool mode_is_fix(char mode)
{
    return mode == '\0' || strchr("ADFRP", mode) != NULL;
}

/* ddmmyy, two-digit years from 1980 (the GPS epoch) to 2079. */
static bool date_ddmmyy(const espos_nmea0183_sentence_t *s, int i, int *y, int *m, int *d)
{
    const char *f = field(s, i);
    if (!f || strlen(f) != 6) {
        return false;
    }
    *d = digits(f, 2);
    *m = digits(f + 2, 2);
    int yy = digits(f + 4, 2);
    if (*d < 0 || *m < 0 || yy < 0) {
        return false;
    }
    *y = yy >= 80 ? 1900 + yy : 2000 + yy;
    return true;
}

bool espos_nmea0183_decode_rmc(const espos_nmea0183_sentence_t *s, espos_nmea0183_rmc_t *out)
{
    if (!espos_nmea0183_is(s, "RMC") || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->mode = espos_nmea0183_field_char(s, 11);
    out->valid = espos_nmea0183_field_char(s, 1) == 'A' && mode_is_fix(out->mode);
    out->has_time = espos_nmea0183_field_time(s, 0, &out->time_ms);
    int y, m, d;
    if (out->has_time && date_ddmmyy(s, 8, &y, &m, &d)) {
        out->unix_ms = espos_nmea0183_unix_ms(y, m, d, out->time_ms);
        out->has_datetime = out->unix_ms >= 0;
    }
    if (out->valid) {
        out->has_position = position(s, 2, &out->lat, &out->lon);
        double kn;
        if (espos_nmea0183_field_double(s, 6, &kn) && kn >= 0) {
            out->sog = kn * KNOT_MS;
            out->has_sog = true;
        }
        out->has_cog = angle(s, 7, &out->cog_true);
    }
    out->has_variation = signed_angle(s, 9, &out->variation);
    return out->has_time || out->valid || out->has_variation;
}

bool espos_nmea0183_decode_vtg(const espos_nmea0183_sentence_t *s, espos_nmea0183_vtg_t *out)
{
    if (!espos_nmea0183_is(s, "VTG") || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    /* NMEA 0183 1.x sent four bare numbers; 2.0 added a unit letter after each. */
    bool lettered = espos_nmea0183_field_char(s, 1) == 'T' || espos_nmea0183_field_char(s, 3) == 'M' ||
                    espos_nmea0183_field_char(s, 5) == 'N';
    int i_true = 0, i_mag = lettered ? 2 : 1, i_kn = lettered ? 4 : 2, i_kmh = lettered ? 6 : 3;
    out->mode = lettered ? espos_nmea0183_field_char(s, 8) : '\0';
    out->valid = mode_is_fix(out->mode);
    if (!out->valid) {
        return false;
    }
    out->has_cog_true = angle(s, i_true, &out->cog_true);
    out->has_cog_magnetic = angle(s, i_mag, &out->cog_magnetic);
    double v;
    if (espos_nmea0183_field_double(s, i_kn, &v) && v >= 0) {
        out->sog = v * KNOT_MS;
        out->has_sog = true;
    } else if (espos_nmea0183_field_double(s, i_kmh, &v) && v >= 0) {
        out->sog = v * KMH_MS;
        out->has_sog = true;
    }
    return out->has_cog_true || out->has_cog_magnetic || out->has_sog;
}

bool espos_nmea0183_decode_heading(const espos_nmea0183_sentence_t *s, espos_nmea0183_heading_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (espos_nmea0183_is(s, "HDT")) {
        out->has_true = angle(s, 0, &out->heading_true);
    } else if (espos_nmea0183_is(s, "HDM")) {
        out->has_magnetic = angle(s, 0, &out->heading_magnetic);
    } else if (espos_nmea0183_is(s, "HDG")) {
        double sensor, dev = 0;
        if (angle(s, 0, &sensor)) {
            /* Deviation corrects the sensor to magnetic; left out, it is 0. */
            signed_angle(s, 1, &dev);
            out->heading_magnetic = rad_of((sensor + dev) / DEG_RAD);
            out->has_magnetic = true;
        }
        out->has_variation = signed_angle(s, 3, &out->variation);
    } else {
        return false;
    }
    return out->has_true || out->has_magnetic || out->has_variation;
}

bool espos_nmea0183_decode_depth(const espos_nmea0183_sentence_t *s, espos_nmea0183_depth_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (espos_nmea0183_is(s, "DPT")) {
        out->below = ESPOS_NMEA0183_DEPTH_TRANSDUCER;
        out->has_depth = espos_nmea0183_field_double(s, 0, &out->depth) && out->depth >= 0;
        out->has_offset = espos_nmea0183_field_double(s, 1, &out->offset);
        out->has_range = espos_nmea0183_field_double(s, 2, &out->range) && out->range > 0;
        return out->has_depth;
    }
    if (espos_nmea0183_is(s, "DBT")) {
        out->below = ESPOS_NMEA0183_DEPTH_TRANSDUCER;
    } else if (espos_nmea0183_is(s, "DBS")) {
        out->below = ESPOS_NMEA0183_DEPTH_SURFACE;
    } else if (espos_nmea0183_is(s, "DBK")) {
        out->below = ESPOS_NMEA0183_DEPTH_KEEL;
    } else {
        return false;
    }
    /* Feet, metres, fathoms: one sounder fills all three, another only one.
     * Metres first, so nothing is converted that need not be. */
    double v;
    if (espos_nmea0183_field_double(s, 2, &v)) {
        out->depth = v;
    } else if (espos_nmea0183_field_double(s, 0, &v)) {
        out->depth = v * FOOT_M;
    } else if (espos_nmea0183_field_double(s, 4, &v)) {
        out->depth = v * FATHOM_M;
    } else {
        return false;
    }
    /* Below the keel can be negative: the keel is in the mud. */
    out->has_depth = out->below == ESPOS_NMEA0183_DEPTH_KEEL || out->depth >= 0;
    return out->has_depth;
}

bool espos_nmea0183_decode_mtw(const espos_nmea0183_sentence_t *s, espos_nmea0183_mtw_t *out)
{
    if (!espos_nmea0183_is(s, "MTW") || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    double c;
    char unit = espos_nmea0183_field_char(s, 1);
    if (espos_nmea0183_field_double(s, 0, &c) && (unit == 'C' || unit == '\0')) {
        out->temperature = c + CELSIUS_K;
        out->has_temperature = true;
    }
    return out->has_temperature;
}

bool espos_nmea0183_decode_vhw(const espos_nmea0183_sentence_t *s, espos_nmea0183_vhw_t *out)
{
    if (!espos_nmea0183_is(s, "VHW") || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->has_true = angle(s, 0, &out->heading_true);
    out->has_magnetic = angle(s, 2, &out->heading_magnetic);
    double v;
    if (espos_nmea0183_field_double(s, 4, &v) && v >= 0) {
        out->stw = v * KNOT_MS;
        out->has_stw = true;
    } else if (espos_nmea0183_field_double(s, 6, &v) && v >= 0) {
        out->stw = v * KMH_MS;
        out->has_stw = true;
    }
    return out->has_true || out->has_magnetic || out->has_stw;
}

bool espos_nmea0183_decode_zda(const espos_nmea0183_sentence_t *s, espos_nmea0183_zda_t *out)
{
    if (!espos_nmea0183_is(s, "ZDA") || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    int64_t ms;
    int32_t d, m, y;
    const char *fy = field(s, 3);
    if (espos_nmea0183_field_time(s, 0, &ms) && field_int(s, 1, &d) && field_int(s, 2, &m) && fy &&
        strlen(fy) == 4 && field_int(s, 3, &y)) {
        out->unix_ms = espos_nmea0183_unix_ms(y, m, d, ms);
        out->has_datetime = out->unix_ms >= 0;
    }
    return out->has_datetime;
}

/* Newton's square root: the host tests link no libm, and a few iterations
 * from a halving start are exact to a double for a receiver's error. */
static double root(double x)
{
    if (x <= 0) {
        return 0;
    }
    double r = x > 1 ? x / 2 : 1;
    for (int i = 0; i < 60; i++) {
        double next = (r + x / r) / 2;
        if (next == r) {
            break;
        }
        r = next;
    }
    return r;
}

/* A deviation is a size: a negative one is a talker bug, not a value, and so
 * is one past 1000 km, which would also overflow h_accuracy's square. */
static bool deviation(const espos_nmea0183_sentence_t *s, int i, double *out)
{
    return espos_nmea0183_field_double(s, i, out) && *out >= 0 && *out < 1e6;
}

bool espos_nmea0183_decode_gst(const espos_nmea0183_sentence_t *s, espos_nmea0183_gst_t *out)
{
    if (!espos_nmea0183_is(s, "GST") || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->has_time = espos_nmea0183_field_time(s, 0, &out->time_ms);
    out->has_rms = deviation(s, 1, &out->rms);
    out->has_lat_lon = deviation(s, 5, &out->lat_sd) && deviation(s, 6, &out->lon_sd);
    if (out->has_lat_lon) {
        out->h_accuracy = root(out->lat_sd * out->lat_sd + out->lon_sd * out->lon_sd);
    } else {
        out->lat_sd = out->lon_sd = 0;
    }
    out->has_altitude = deviation(s, 7, &out->altitude_sd);
    return out->has_rms || out->has_lat_lon || out->has_altitude;
}
