/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_sdlog — sessions of rows on a card that survive a power cut.
 *
 * A logger on a dinghy loses power the way a dinghy does: the battery runs
 * flat, someone pulls the plug, the boat flips. A file that is open at that
 * moment ends in half a row, and a file that only becomes valid when it is
 * closed is lost whole. A session here is a directory under a root:
 *
 *     <root>/00000042/HEAD.JSN   the caller's header, written before any row
 *     <root>/00000042/ROWS.TXT   one row per line, appended, synced every N rows
 *     <root>/00000042/CLOSE.JSN  the close record: rows, bytes, sha256
 *     <root>/00000042/SYNCED     present once the session reached its server
 *
 * A session without CLOSE.JSN was cut short. espos_sdlog_recover() closes
 * it at the next boot: the half row is cut off, and the close record says
 * `"clean":false`. Only a closed session is complete enough to upload, and
 * its sha256 is what a server checks the upload against.
 *
 * The names fit 8.3, so the card works with FatFs' default of no long file
 * names. What a session means (a trip, a vessel, units) is the caller's,
 * in the header and the rows; this component counts, hashes and keeps it.
 *
 * Pure C on stdio and POSIX, so it is host-tested on a temporary directory;
 * espos_sdlog_card.h mounts the card the root lives on.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A sha256 as lower-case hex, with its terminator. */
#define ESPOS_SDLOG_SHA256_HEX_MAX 65

/** Rows between syncs when espos_sdlog_open_cfg_t.sync_every is 0. */
#define ESPOS_SDLOG_SYNC_EVERY_DEFAULT 10

typedef struct espos_sdlog_session *espos_sdlog_session_t;

typedef struct {
    /** Rows between fsync()s: what a power cut can lose at most. 0 means
     * ESPOS_SDLOG_SYNC_EVERY_DEFAULT. A sync costs a card write cycle, so 1
     * on a row a second wears a card faster than it needs to. */
    uint32_t sync_every;
    /** End every row with ",xxxxxxxx": the CRC-32 of the row before that
     * comma, as espos_sdlog_crc32() computes it, in lower-case hex. A reader
     * then keeps each row that ends in a newline and passes its check, and a
     * row a power cut or a bad sector garbled is dropped alone. */
    bool row_crc;
} espos_sdlog_open_cfg_t;

/**
 * Start a session under `root` (created if missing) with `header` as its
 * HEAD.JSN, usually a JSON object. Its number is one above the highest
 * ever handed out under `root` (LASTSEQ.TXT there keeps it), so a session
 * that was pruned never has its number handed out again. `cfg` may be NULL.
 *
 * The header is on the card, synced, before this returns, so a session
 * without one is a session that never started; recover() removes it.
 */
esp_err_t espos_sdlog_open(const char *root, const char *header, const espos_sdlog_open_cfg_t *cfg,
                           espos_sdlog_session_t *out);

/** The session's number, which names its directory. */
uint32_t espos_sdlog_seq(espos_sdlog_session_t s);

/** Rows appended so far. */
uint64_t espos_sdlog_rows(espos_sdlog_session_t s);

/** Bytes in ROWS.TXT so far, newlines included: what a caller rotating by
 * size compares. */
uint64_t espos_sdlog_bytes(espos_sdlog_session_t s);

/**
 * Append one row. `row` is a line without its newline, which this adds;
 * ESP_ERR_INVALID_ARG for a row that contains one (or a CR), since it would
 * read back as two. ESP_FAIL when the card refused the write; the session is
 * then unusable and should be closed.
 */
esp_err_t espos_sdlog_append(espos_sdlog_session_t s, const char *row);

/** Push everything appended to the card now, as before a planned sleep. */
esp_err_t espos_sdlog_sync(espos_sdlog_session_t s);

/**
 * Close the session: sync the rows and write the close record. `meta`, if
 * not NULL, is a JSON value stored as the record's `"meta"` (an end time, a
 * reason), and must be valid JSON, since it is written as it is. The handle
 * is freed whatever the result. ESP_FAIL leaves the session unclosed, for
 * recover() to close at the next boot.
 */
esp_err_t espos_sdlog_close(espos_sdlog_session_t s, const char *meta);

typedef struct {
    uint32_t seq;
    bool closed; /**< CLOSE.JSN is there */
    bool clean;  /**< closed by espos_sdlog_close(), not by recover() */
    bool synced; /**< SYNCED is there */
    uint64_t rows;  /**< from the close record; 0 while open */
    uint64_t bytes; /**< from the close record; the file's size while open */
    char sha256[ESPOS_SDLOG_SHA256_HEX_MAX]; /**< of ROWS.TXT; empty while open */
} espos_sdlog_info_t;

/** What session `seq` under `root` holds. ESP_ERR_NOT_FOUND when there is
 * none, ESP_ERR_INVALID_RESPONSE for a close record this could not read. */
esp_err_t espos_sdlog_info(const char *root, uint32_t seq, espos_sdlog_info_t *out);

/** Called on the task that called espos_sdlog_list(). Return false to stop. */
typedef bool (*espos_sdlog_visit_fn)(const espos_sdlog_info_t *info, void *arg);

/**
 * Every session under `root`, oldest first. A directory that is not a
 * session (another name, an unreadable close record) is skipped. A missing
 * root is an empty list.
 */
esp_err_t espos_sdlog_list(const char *root, espos_sdlog_visit_fn fn, void *arg);

/**
 * Close every session a power cut left open: cut ROWS.TXT after its last
 * complete row, count and hash it, and write a close record with
 * `"clean":false`. A session without a header never started and is
 * removed. Call it once after mounting and before opening a session, since
 * a session open at the time looks cut short too. `recovered` and `removed`
 * may be NULL.
 */
esp_err_t espos_sdlog_recover(const char *root, uint32_t *recovered, uint32_t *removed);

typedef enum {
    ESPOS_SDLOG_FILE_HEADER = 0, /**< HEAD.JSN */
    ESPOS_SDLOG_FILE_ROWS = 1,   /**< ROWS.TXT */
    ESPOS_SDLOG_FILE_CLOSE = 2,  /**< CLOSE.JSN */
    ESPOS_SDLOG_FILE_SYNCED = 3, /**< SYNCED */
    ESPOS_SDLOG_FILE_MAX = 4,
} espos_sdlog_file_t;

/** The path of one of session `seq`'s files, into `buf`. Returns its length,
 * or 0 when it does not fit. For an uploader opening the files itself. */
size_t espos_sdlog_path(const char *root, uint32_t seq, espos_sdlog_file_t which, char *buf, size_t n);

/**
 * Mark a closed session as delivered, with `note` (the server's reply, an
 * id; may be NULL) as SYNCED's content. ESP_ERR_INVALID_STATE for a session
 * that is not closed: what was delivered must be what the hash says.
 */
esp_err_t espos_sdlog_mark_synced(const char *root, uint32_t seq, const char *note);

/** Delete session `seq` and its directory, whatever its state. */
esp_err_t espos_sdlog_remove(const char *root, uint32_t seq);

/**
 * Delete the oldest synced sessions until the row files of all sessions
 * under `root` add up to `keep_bytes` or less. A session that has not
 * reached its server is never deleted, so a full card stops a logger
 * rather than eating its data. `removed` may be NULL.
 */
esp_err_t espos_sdlog_prune(const char *root, uint64_t keep_bytes, uint32_t *removed);

/** CRC-32 as zlib, Ethernet and PNG compute it (reflected 0x04C11DB7,
 * initial and final XOR 0xFFFFFFFF). Pass 0 to start and the previous result
 * to continue: crc32("123456789") is 0xcbf43926. */
uint32_t espos_sdlog_crc32(uint32_t crc, const void *data, size_t n);

/* sha256, exposed for an uploader that hashes what it sends. */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
} espos_sdlog_sha256_t;

void espos_sdlog_sha256_init(espos_sdlog_sha256_t *c);
void espos_sdlog_sha256_update(espos_sdlog_sha256_t *c, const void *data, size_t n);
/** Finish into `hex` (lower case, ESPOS_SDLOG_SHA256_HEX_MAX bytes). */
void espos_sdlog_sha256_hex(espos_sdlog_sha256_t *c, char hex[ESPOS_SDLOG_SHA256_HEX_MAX]);

#ifdef __cplusplus
}
#endif
