/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_nmea0183 — NMEA 0183 sentences: framing, checksum, fields, and the
 * sentences a GPS, a compass, a depth sounder or a log talks.
 *
 * Pure C with no IDF dependency, so every byte of it runs in the host tests.
 * The UART that delivers the bytes is espos_nmea0183_uart.h; what the values
 * mean in Signal K is espos_nmea0183_sk.h.
 *
 * Every decoded value is in Signal K units — radians, metres, m/s, kelvin,
 * unix milliseconds — so nothing downstream converts knots or degrees twice.
 * Each value carries a has_ flag: an empty field is "not reported", never 0,
 * because a zero depth or a 0° heading is a value a consumer acts on.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The standard caps a sentence at 82 characters including CR LF. Real
 * talkers exceed it (multi-constellation GSV, proprietary sentences), so the
 * reader accepts more and leaves the 82 to the talker's conscience. */
#define ESPOS_NMEA0183_LINE_MAX   160
#define ESPOS_NMEA0183_MAX_FIELDS 40

typedef enum {
    ESPOS_NMEA0183_OK = 0,
    ESPOS_NMEA0183_ERR_EMPTY = 1,           /* nothing but whitespace */
    ESPOS_NMEA0183_ERR_NO_START = 2,        /* does not begin with '$' or '!' */
    ESPOS_NMEA0183_ERR_TOO_LONG = 3,        /* over ESPOS_NMEA0183_LINE_MAX */
    ESPOS_NMEA0183_ERR_BAD_CHAR = 4,        /* a byte outside printable ASCII */
    ESPOS_NMEA0183_ERR_NO_CHECKSUM = 5,     /* none, and the caller requires one */
    ESPOS_NMEA0183_ERR_BAD_CHECKSUM = 6,    /* present and wrong, or malformed */
    ESPOS_NMEA0183_ERR_BAD_ADDRESS = 7,     /* the address field is not TTSSS or Pxxx */
    ESPOS_NMEA0183_ERR_TOO_MANY_FIELDS = 8, /* more than ESPOS_NMEA0183_MAX_FIELDS */
    ESPOS_NMEA0183_ERR_MAX = 9,
} espos_nmea0183_err_t;

/** A sentence split into its fields. The pointers point into the line the
 * caller passed to espos_nmea0183_parse(), which it overwrites: the struct
 * lives exactly as long as that buffer. */
typedef struct {
    char talker[3];       /* "GP", "GN", "SD", "II"...; "" for a proprietary sentence */
    char type[8];         /* "GGA", "RMC"...; for $PGRME it is "GRME" */
    bool proprietary;     /* address began with 'P' */
    bool encapsulated;    /* began with '!' (AIS and other encapsulated data) */
    bool has_checksum;    /* a checksum was present (and matched) */
    uint8_t n_fields;     /* data fields after the address */
    const char *field[ESPOS_NMEA0183_MAX_FIELDS];
} espos_nmea0183_sentence_t;

/**
 * Validate and split one sentence, in place. `line` is NUL-terminated and may
 * end in CR, LF or both. With `require_checksum` a sentence without "*hh" is
 * refused; one with a wrong checksum is refused always. On success the field
 * pointers in `out` point into `line`.
 */
espos_nmea0183_err_t espos_nmea0183_parse(char *line, bool require_checksum, espos_nmea0183_sentence_t *out);

/** A short constant name for an error ("bad checksum"), for logs and stats. */
const char *espos_nmea0183_err_str(espos_nmea0183_err_t err);

/** True when `s` is a `type` sentence from any talker ("GGA" matches $GPGGA
 * and $GNGGA alike). Proprietary sentences never match. */
bool espos_nmea0183_is(const espos_nmea0183_sentence_t *s, const char *type);

/** XOR of every byte between the start character and '*', the way the
 * checksum is computed. Exposed so a talker in an application builds its
 * sentences with the same code that checks them. */
uint8_t espos_nmea0183_checksum(const char *body, size_t len);

/**
 * Make `line` ("$IIXDR,..." with or without "*hh" and CR LF) a sentence ready
 * to send: its checksum (recomputed when present) and CR LF, into `out`.
 * Returns the length written, 0 when `line` has no start character or the
 * result does not fit in `n` (or in ESPOS_NMEA0183_LINE_MAX).
 */
size_t espos_nmea0183_finish(const char *line, char *out, size_t n);

/* ------------------------------------------------------------------------ */
/* Assembling lines from a byte stream                                      */
/* ------------------------------------------------------------------------ */

/** Called for every complete line, without its CR LF. `line` is writable and
 * valid until the callback returns. */
typedef void (*espos_nmea0183_line_fn)(char *line, size_t len, void *arg);

/**
 * Turns a byte stream into lines. A '$' or '!' in the middle of a line starts
 * a new one — the talker reset or the first bytes after power-up were noise.
 * A line longer than ESPOS_NMEA0183_LINE_MAX, or one with a byte that is not
 * printable ASCII in it, is dropped whole rather than delivered cut, because
 * a cut GGA still parses and reports the wrong place.
 */
typedef struct {
    char buf[ESPOS_NMEA0183_LINE_MAX + 1];
    size_t len;
    bool discarding; /* inside a dropped line: skip to its end */
    uint32_t lines;
    uint32_t overflows;
    uint32_t resyncs;
    uint32_t garbled; /* lines dropped for a byte that is not printable ASCII */
} espos_nmea0183_reader_t;

void espos_nmea0183_reader_init(espos_nmea0183_reader_t *r);

/** Feed `n` bytes; `fn` runs once per complete line, on the calling task. */
void espos_nmea0183_reader_feed(espos_nmea0183_reader_t *r, const uint8_t *data, size_t n,
                                espos_nmea0183_line_fn fn, void *arg);

/* ------------------------------------------------------------------------ */
/* Fields                                                                   */
/* ------------------------------------------------------------------------ */

/** Field `i` as a number. False when it is missing, empty or not a plain
 * decimal (no hex, exponent or blanks). */
bool espos_nmea0183_field_double(const espos_nmea0183_sentence_t *s, int32_t i, double *out);

/** Field `i` as its first character; '\0' when missing or empty. */
char espos_nmea0183_field_char(const espos_nmea0183_sentence_t *s, int32_t i);

/** Fields `i` ("ddmm.mmmm" or "dddmm.mmmm") and `i + 1` (N/S or E/W) as signed
 * decimal degrees. False when either is empty or out of range. */
bool espos_nmea0183_field_latlon(const espos_nmea0183_sentence_t *s, int32_t i, double *out_deg);

/** Field `i` ("hhmmss" or "hhmmss.sss") as milliseconds since midnight UTC. */
bool espos_nmea0183_field_time(const espos_nmea0183_sentence_t *s, int32_t i, int64_t *out_ms);

/** Unix milliseconds of a UTC civil date (full year) plus milliseconds since
 * midnight; -1 for an impossible date. */
int64_t espos_nmea0183_unix_ms(int32_t year, int32_t month, int32_t day, int64_t ms_of_day);

/* ------------------------------------------------------------------------ */
/* Decoded sentences, in Signal K units                                     */
/* ------------------------------------------------------------------------ */

/** GGA — fix data. Position, altitude and geoid separation are reported only
 * when the fix quality says the receiver has one (1 to 5); a receiver without
 * a fix repeats its last position, and dead reckoning, manual input and a
 * simulator are not fixes. */
typedef struct {
    bool has_time, has_position, has_satellites, has_hdop, has_altitude, has_geoid;
    int64_t time_ms;         /* since midnight UTC */
    double lat, lon;         /* decimal degrees, + north / east */
    int32_t quality;             /* 0 none, 1 GNSS, 2 DGNSS, 3 PPS, 4 RTK fixed, 5 RTK float, 6 dead reckoning, 7 manual, 8 simulator */
    int32_t satellites;
    double hdop;
    double altitude;         /* m above mean sea level */
    double geoid_separation; /* m */
} espos_nmea0183_gga_t;

/** RMC — recommended minimum. `valid` is the status field (A) and, from NMEA
 * 2.3 on, a mode indicator that means a fix (A, D, F, R, P; not E estimated,
 * M manual, S simulator or N). */
typedef struct {
    bool valid;
    bool has_time, has_datetime, has_position, has_sog, has_cog, has_variation;
    int64_t time_ms;         /* since midnight UTC */
    int64_t unix_ms;         /* date + time, when both are present */
    double lat, lon;
    double sog;              /* m/s */
    double cog_true;         /* rad */
    double variation;        /* rad, + east */
    char mode;               /* the 2.3+ mode indicator; '\0' before 2.3 */
} espos_nmea0183_rmc_t;

/** VTG — course and speed over ground. `valid` as for RMC's mode indicator. */
typedef struct {
    bool valid;
    bool has_cog_true, has_cog_magnetic, has_sog;
    double cog_true, cog_magnetic; /* rad */
    double sog;                    /* m/s */
    char mode;
} espos_nmea0183_vtg_t;

/** HDT, HDM and HDG — heading. HDG's sensor reading plus deviation is the
 * magnetic heading; its variation, when present, is reported as well. */
typedef struct {
    bool has_true, has_magnetic, has_variation;
    double heading_true, heading_magnetic; /* rad */
    double variation;                      /* rad, + east */
} espos_nmea0183_heading_t;

/** DPT, DBT, DBS, DBK — depth. `below` says what the depth is measured from;
 * DPT's offset is + transducer to waterline, − transducer to keel. */
typedef enum {
    ESPOS_NMEA0183_DEPTH_TRANSDUCER = 0,
    ESPOS_NMEA0183_DEPTH_SURFACE = 1,
    ESPOS_NMEA0183_DEPTH_KEEL = 2,
    ESPOS_NMEA0183_DEPTH_REF_MAX = 3,
} espos_nmea0183_depth_ref_t;

typedef struct {
    bool has_depth, has_offset, has_range;
    espos_nmea0183_depth_ref_t below;
    double depth;  /* m */
    double offset; /* m, DPT only */
    double range;  /* m, DPT only: the sounder's maximum range scale */
} espos_nmea0183_depth_t;

/** MTW — water temperature. */
typedef struct {
    bool has_temperature;
    double temperature; /* K */
} espos_nmea0183_mtw_t;

/** VHW — speed and heading through the water. */
typedef struct {
    bool has_stw, has_true, has_magnetic;
    double stw;                            /* m/s */
    double heading_true, heading_magnetic; /* rad */
} espos_nmea0183_vhw_t;

/** ZDA — UTC date and time. */
typedef struct {
    bool has_datetime;
    int64_t unix_ms;
} espos_nmea0183_zda_t;

/** GST — the receiver's own estimate of its position error, in metres, one
 * standard deviation. `h_accuracy` combines the latitude and longitude
 * errors (the root of the sum of their squares), the horizontal accuracy a
 * logger weights a sounding by. Signal K has no path for it, so it is decoded
 * but not published. */
typedef struct {
    bool has_time, has_rms, has_lat_lon, has_altitude;
    int64_t time_ms;     /* since midnight UTC */
    double rms;          /* RMS of the pseudorange residuals, m */
    double lat_sd;       /* m */
    double lon_sd;       /* m */
    double h_accuracy;   /* m: sqrt(lat_sd² + lon_sd²) */
    double altitude_sd;  /* m */
} espos_nmea0183_gst_t;

/* Each returns false when `s` is not that sentence or carries nothing usable;
 * the has_ flags say which values it did carry. */
bool espos_nmea0183_decode_gga(const espos_nmea0183_sentence_t *s, espos_nmea0183_gga_t *out);
bool espos_nmea0183_decode_rmc(const espos_nmea0183_sentence_t *s, espos_nmea0183_rmc_t *out);
bool espos_nmea0183_decode_vtg(const espos_nmea0183_sentence_t *s, espos_nmea0183_vtg_t *out);
bool espos_nmea0183_decode_heading(const espos_nmea0183_sentence_t *s, espos_nmea0183_heading_t *out); /* HDT, HDM, HDG */
bool espos_nmea0183_decode_depth(const espos_nmea0183_sentence_t *s, espos_nmea0183_depth_t *out);     /* DPT, DBT, DBS, DBK */
bool espos_nmea0183_decode_mtw(const espos_nmea0183_sentence_t *s, espos_nmea0183_mtw_t *out);
bool espos_nmea0183_decode_vhw(const espos_nmea0183_sentence_t *s, espos_nmea0183_vhw_t *out);
bool espos_nmea0183_decode_zda(const espos_nmea0183_sentence_t *s, espos_nmea0183_zda_t *out);
bool espos_nmea0183_decode_gst(const espos_nmea0183_sentence_t *s, espos_nmea0183_gst_t *out);

#ifdef __cplusplus
}
#endif
