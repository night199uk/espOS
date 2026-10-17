/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sessions on stdio and POSIX only, so the host tests run the same code on
 * a temporary directory that the device runs on FatFs.
 *
 * The order of writes is what makes a power cut survivable: the header is
 * complete (tmp, sync, rename) before ROWS.TXT exists, and the close record
 * is complete before it has its name. Whatever moment the power goes, a
 * session is either without a header (never started), open (recover() closes
 * it) or closed.
 */
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "espos_sdlog.h"

#define PATH_MAX_LEN  160
#define SEQ_DIGITS    8
#define MAX_SEQ       99999999u
#define LAST_SEQ_FILE "LASTSEQ.TXT"
#define LAST_SEQ_TMP  "LASTSEQ.TMP"

static const char *const FILE_NAMES[] = {
    [ESPOS_SDLOG_FILE_HEADER] = "HEAD.JSN",
    [ESPOS_SDLOG_FILE_ROWS] = "ROWS.TXT",
    [ESPOS_SDLOG_FILE_CLOSE] = "CLOSE.JSN",
    [ESPOS_SDLOG_FILE_SYNCED] = "SYNCED",
};

struct espos_sdlog_session {
    FILE *rows_f;
    espos_sdlog_sha256_t sha;
    uint32_t seq;
    uint64_t rows;
    uint64_t bytes;
    uint32_t sync_every;
    uint32_t unsynced;
    bool row_crc;
    bool failed;
    char root[PATH_MAX_LEN];
};

size_t espos_sdlog_path(const char *root, uint32_t seq, espos_sdlog_file_t which, char *buf, size_t n)
{
    if (!root || !buf || (unsigned)which > ESPOS_SDLOG_FILE_SYNCED) {
        return 0;
    }
    int len = snprintf(buf, n, "%s/%08" PRIu32 "/%s", root, seq, FILE_NAMES[which]);
    return (len < 0 || (size_t)len >= n) ? 0 : (size_t)len;
}

static bool path_in(char *buf, size_t n, const char *root, uint32_t seq, const char *name)
{
    int len = name ? snprintf(buf, n, "%s/%08" PRIu32 "/%s", root, seq, name)
                   : snprintf(buf, n, "%s/%08" PRIu32, root, seq);
    return len > 0 && (size_t)len < n;
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool session_file_exists(const char *root, uint32_t seq, const char *name)
{
    char p[PATH_MAX_LEN];
    return path_in(p, sizeof(p), root, seq, name) && exists(p);
}

/* A session directory is exactly eight digits; anything else under the root
 * belongs to someone else and is left alone. */
static bool parse_seq(const char *name, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < SEQ_DIGITS; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        v = v * 10 + (uint32_t)(name[i] - '0');
    }
    if (name[SEQ_DIGITS] != '\0') {
        return false;
    }
    *out = v;
    return true;
}

static int cmp_seq(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* Every session number under root, sorted, in a malloc'd array (NULL when
 * there are none). A missing root has none. */
static esp_err_t scan(const char *root, uint32_t **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    DIR *d = opendir(root);
    if (!d) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    size_t cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t seq;
        if (!parse_seq(e->d_name, &seq)) {
            continue;
        }
        if (*count == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            uint32_t *grown = realloc(*out, ncap * sizeof(uint32_t));
            if (!grown) {
                closedir(d);
                free(*out);
                *out = NULL;
                *count = 0;
                return ESP_ERR_NO_MEM;
            }
            *out = grown;
            cap = ncap;
        }
        (*out)[(*count)++] = seq;
    }
    closedir(d);
    if (*count > 1) {
        qsort(*out, *count, sizeof(uint32_t), cmp_seq);
    }
    return ESP_OK;
}

static esp_err_t mkdir_p(const char *path)
{
    char p[PATH_MAX_LEN];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(p)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(p, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (p[i] == '/' || p[i] == '\0') {
            char c = p[i];
            p[i] = '\0';
            if (mkdir(p, 0775) != 0 && errno != EEXIST) {
                /* A mount point such as /sdcard is not a directory one can
                 * make, but it is one that exists. */
                struct stat st;
                if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) {
                    return ESP_FAIL;
                }
            }
            p[i] = c;
        }
    }
    return ESP_OK;
}

static esp_err_t sync_file(FILE *f)
{
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Write `parts` (up to three, NULL ones skipped) to `tmp`, sync it, then
 * rename it to `dst`: a reader sees the whole file or none. */
static esp_err_t write_file_atomic(const char *tmp, const char *dst, const char *a, const char *b, const char *c)
{
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    bool ok = (!a || fputs(a, f) >= 0) && (!b || fputs(b, f) >= 0) && (!c || fputs(c, f) >= 0) &&
              sync_file(f) == ESP_OK;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        unlink(tmp);
        return ESP_FAIL;
    }
    /* FatFs refuses to rename onto an existing name. Only SYNCED and the
     * root's LASTSEQ.TXT are ever rewritten: losing SYNCED to a power cut
     * here costs a duplicate upload, which the server answers as one, and
     * LASTSEQ.TXT is read back from LASTSEQ.TMP. */
    unlink(dst);
    if (rename(tmp, dst) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t write_atomic(const char *root, uint32_t seq, const char *tmp_name, const char *name,
                              const char *a, const char *b, const char *c)
{
    char tmp[PATH_MAX_LEN], dst[PATH_MAX_LEN];
    if (!path_in(tmp, sizeof(tmp), root, seq, tmp_name) || !path_in(dst, sizeof(dst), root, seq, name)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return write_file_atomic(tmp, dst, a, b, c);
}

static uint32_t read_seq_file(const char *root, const char *name)
{
    char p[PATH_MAX_LEN];
    snprintf(p, sizeof(p), "%s/%s", root, name);
    FILE *f = fopen(p, "rb");
    if (!f) {
        return 0;
    }
    unsigned long v = 0;
    if (fscanf(f, "%lu", &v) != 1 || v > MAX_SEQ) {
        v = 0;
    }
    fclose(f);
    return (uint32_t)v;
}

/* The highest number ever handed out under root. Sessions are pruned once
 * delivered, the newest included, so the directories alone would hand a
 * number out twice. A power cut between removing LASTSEQ.TXT and renaming
 * the synced LASTSEQ.TMP onto it leaves only the temporary file, so both
 * count. */
static uint32_t read_last_seq(const char *root)
{
    uint32_t a = read_seq_file(root, LAST_SEQ_FILE), b = read_seq_file(root, LAST_SEQ_TMP);
    return a > b ? a : b;
}

static esp_err_t write_last_seq(const char *root, uint32_t seq)
{
    char tmp[PATH_MAX_LEN], dst[PATH_MAX_LEN], text[16];
    snprintf(tmp, sizeof(tmp), "%s/" LAST_SEQ_TMP, root);
    snprintf(dst, sizeof(dst), "%s/" LAST_SEQ_FILE, root);
    snprintf(text, sizeof(text), "%" PRIu32 "\n", seq);
    return write_file_atomic(tmp, dst, text, NULL, NULL);
}

static esp_err_t write_close(const char *root, uint32_t seq, bool clean, uint64_t rows, uint64_t bytes,
                             const char *sha, const char *meta)
{
    char head[192];
    int n = snprintf(head, sizeof(head),
                     "{\"closed\":true,\"clean\":%s,\"rows\":%" PRIu64 ",\"bytes\":%" PRIu64
                     ",\"sha256\":\"%s\",\"meta\":",
                     clean ? "true" : "false", rows, bytes, sha);
    if (n < 0 || (size_t)n >= sizeof(head)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return write_atomic(root, seq, "CLOSE.TMP", "CLOSE.JSN", head, meta ? meta : "null", "}\n");
}

esp_err_t espos_sdlog_open(const char *root, const char *header, const espos_sdlog_open_cfg_t *cfg,
                           espos_sdlog_session_t *out)
{
    if (!root || !header || !out || strlen(root) + 1 + SEQ_DIGITS + 1 + 12 >= PATH_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    esp_err_t err = mkdir_p(root);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t *seqs;
    size_t count;
    err = scan(root, &seqs, &count);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t last = read_last_seq(root);
    if (count && seqs[count - 1] > last) {
        last = seqs[count - 1];
    }
    free(seqs);
    if (last >= MAX_SEQ) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t seq = last + 1;
    /* Recorded before the directory exists: a power cut in between skips a
     * number, which is harmless, where handing one out twice is not. */
    err = write_last_seq(root, seq);
    if (err != ESP_OK) {
        return err;
    }

    char p[PATH_MAX_LEN];
    path_in(p, sizeof(p), root, seq, NULL);
    if (mkdir(p, 0775) != 0) {
        return ESP_FAIL;
    }
    err = write_atomic(root, seq, "HEAD.TMP", "HEAD.JSN", header, NULL, NULL);
    if (err != ESP_OK) {
        espos_sdlog_remove(root, seq);
        return err;
    }
    struct espos_sdlog_session *s = calloc(1, sizeof(*s));
    if (!s) {
        espos_sdlog_remove(root, seq);
        return ESP_ERR_NO_MEM;
    }
    path_in(p, sizeof(p), root, seq, FILE_NAMES[ESPOS_SDLOG_FILE_ROWS]);
    s->rows_f = fopen(p, "wb");
    if (!s->rows_f) {
        free(s);
        espos_sdlog_remove(root, seq);
        return ESP_FAIL;
    }
    strcpy(s->root, root);
    s->seq = seq;
    s->sync_every = (cfg && cfg->sync_every) ? cfg->sync_every : ESPOS_SDLOG_SYNC_EVERY_DEFAULT;
    s->row_crc = cfg && cfg->row_crc;
    espos_sdlog_sha256_init(&s->sha);
    *out = s;
    return ESP_OK;
}

uint32_t espos_sdlog_seq(espos_sdlog_session_t s)
{
    return s ? s->seq : 0;
}

uint64_t espos_sdlog_rows(espos_sdlog_session_t s)
{
    return s ? s->rows : 0;
}

uint64_t espos_sdlog_bytes(espos_sdlog_session_t s)
{
    return s ? s->bytes : 0;
}

esp_err_t espos_sdlog_sync(espos_sdlog_session_t s)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s->failed) {
        return ESP_FAIL;
    }
    if (sync_file(s->rows_f) != ESP_OK) {
        s->failed = true;
        return ESP_FAIL;
    }
    s->unsynced = 0;
    return ESP_OK;
}

esp_err_t espos_sdlog_append(espos_sdlog_session_t s, const char *row)
{
    if (!s || !row || strpbrk(row, "\r\n")) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s->failed) {
        return ESP_FAIL;
    }
    size_t n = strlen(row);
    char tail[16] = "\n";
    if (s->row_crc) {
        snprintf(tail, sizeof(tail), ",%08" PRIx32 "\n", espos_sdlog_crc32(0, row, n));
    }
    size_t t = strlen(tail);
    if (fwrite(row, 1, n, s->rows_f) != n || fwrite(tail, 1, t, s->rows_f) != t) {
        s->failed = true;
        return ESP_FAIL;
    }
    espos_sdlog_sha256_update(&s->sha, row, n);
    espos_sdlog_sha256_update(&s->sha, tail, t);
    s->rows++;
    s->bytes += n + t;
    if (++s->unsynced >= s->sync_every) {
        return espos_sdlog_sync(s);
    }
    return ESP_OK;
}

esp_err_t espos_sdlog_close(espos_sdlog_session_t s, const char *meta)
{
    if (!s) {
        return ESP_ERR_INVALID_ARG;
    }
    bool ok = !s->failed && sync_file(s->rows_f) == ESP_OK;
    ok = (fclose(s->rows_f) == 0) && ok;
    esp_err_t err = ESP_FAIL;
    if (ok) {
        char hex[ESPOS_SDLOG_SHA256_HEX_MAX];
        espos_sdlog_sha256_hex(&s->sha, hex);
        err = write_close(s->root, s->seq, true, s->rows, s->bytes, hex, meta);
    }
    free(s);
    return err;
}

static esp_err_t read_close(const char *root, uint32_t seq, espos_sdlog_info_t *info)
{
    char p[PATH_MAX_LEN];
    path_in(p, sizeof(p), root, seq, FILE_NAMES[ESPOS_SDLOG_FILE_CLOSE]);
    FILE *f = fopen(p, "rb");
    if (!f) {
        return ESP_FAIL;
    }
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* The record is this file's own, written in one fixed order with meta
     * last, so the fields before meta are read as a fixed format. */
    char clean[6] = "";
    unsigned long long rows = 0, bytes = 0;
    char sha[ESPOS_SDLOG_SHA256_HEX_MAX] = "";
    if (sscanf(buf, "{\"closed\":true,\"clean\":%5[a-z],\"rows\":%llu,\"bytes\":%llu,\"sha256\":\"%64[0-9a-f]\"", clean,
               &rows, &bytes, sha) != 4 ||
        strlen(sha) != 64) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    info->clean = strcmp(clean, "true") == 0;
    info->rows = rows;
    info->bytes = bytes;
    memcpy(info->sha256, sha, sizeof(sha));
    return ESP_OK;
}

esp_err_t espos_sdlog_info(const char *root, uint32_t seq, espos_sdlog_info_t *out)
{
    if (!root || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    char p[PATH_MAX_LEN];
    struct stat st;
    if (!path_in(p, sizeof(p), root, seq, NULL) || stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    out->seq = seq;
    out->synced = session_file_exists(root, seq, FILE_NAMES[ESPOS_SDLOG_FILE_SYNCED]);
    if (session_file_exists(root, seq, FILE_NAMES[ESPOS_SDLOG_FILE_CLOSE])) {
        out->closed = true;
        return read_close(root, seq, out);
    }
    path_in(p, sizeof(p), root, seq, FILE_NAMES[ESPOS_SDLOG_FILE_ROWS]);
    if (stat(p, &st) == 0) {
        out->bytes = (uint64_t)st.st_size;
    }
    return ESP_OK;
}

esp_err_t espos_sdlog_list(const char *root, espos_sdlog_visit_fn fn, void *arg)
{
    if (!root || !fn) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t *seqs;
    size_t count;
    esp_err_t err = scan(root, &seqs, &count);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < count; i++) {
        espos_sdlog_info_t info;
        if (espos_sdlog_info(root, seqs[i], &info) == ESP_OK && !fn(&info, arg)) {
            break;
        }
    }
    free(seqs);
    return ESP_OK;
}

esp_err_t espos_sdlog_remove(const char *root, uint32_t seq)
{
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    char dir[PATH_MAX_LEN];
    if (!path_in(dir, sizeof(dir), root, seq, NULL)) {
        return ESP_ERR_INVALID_SIZE;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    struct dirent *e;
    char p[PATH_MAX_LEN];
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (path_in(p, sizeof(p), root, seq, e->d_name)) {
            unlink(p);
        }
    }
    closedir(d);
    return rmdir(dir) == 0 ? ESP_OK : ESP_FAIL;
}

/* Close one session a power cut left open. */
static esp_err_t recover_one(const char *root, uint32_t seq)
{
    char p[PATH_MAX_LEN];
    path_in(p, sizeof(p), root, seq, "CLOSE.TMP");
    unlink(p);
    path_in(p, sizeof(p), root, seq, FILE_NAMES[ESPOS_SDLOG_FILE_ROWS]);

    /* Where the last complete row ends: anything after it is half a row. */
    uint64_t rows = 0, keep = 0, size = 0;
    uint8_t buf[512];
    FILE *f = fopen(p, "rb");
    if (f) {
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            for (size_t i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    rows++;
                    keep = size + i + 1;
                }
            }
            size += n;
        }
        bool read_ok = !ferror(f);
        fclose(f);
        if (!read_ok) {
            return ESP_FAIL;
        }
    } else if (errno != ENOENT) {
        /* Rows that are there but unreadable stay for a later recovery. */
        return ESP_FAIL;
    } else {
        /* The power went between the header and the rows file: the session
         * is empty, and an uploader opening ROWS.TXT must find it. */
        f = fopen(p, "wb");
        if (!f) {
            return ESP_FAIL;
        }
        bool ok = sync_file(f) == ESP_OK;
        ok = (fclose(f) == 0) && ok;
        if (!ok) {
            return ESP_FAIL;
        }
    }
    if (size > keep && truncate(p, (off_t)keep) != 0) {
        return ESP_FAIL;
    }

    espos_sdlog_sha256_t sha;
    espos_sdlog_sha256_init(&sha);
    f = fopen(p, "rb");
    if (!f) {
        return ESP_FAIL;
    }
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        espos_sdlog_sha256_update(&sha, buf, n);
    }
    bool read_ok = !ferror(f);
    fclose(f);
    if (!read_ok) {
        return ESP_FAIL;
    }
    char hex[ESPOS_SDLOG_SHA256_HEX_MAX];
    espos_sdlog_sha256_hex(&sha, hex);
    return write_close(root, seq, false, rows, keep, hex, NULL);
}

esp_err_t espos_sdlog_recover(const char *root, uint32_t *recovered, uint32_t *removed)
{
    if (recovered) {
        *recovered = 0;
    }
    if (removed) {
        *removed = 0;
    }
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t *seqs;
    size_t count;
    esp_err_t err = scan(root, &seqs, &count);
    if (err != ESP_OK) {
        return err;
    }
    esp_err_t first = ESP_OK;
    for (size_t i = 0; i < count; i++) {
        uint32_t seq = seqs[i];
        if (session_file_exists(root, seq, FILE_NAMES[ESPOS_SDLOG_FILE_CLOSE])) {
            continue;
        }
        if (!session_file_exists(root, seq, FILE_NAMES[ESPOS_SDLOG_FILE_HEADER])) {
            err = espos_sdlog_remove(root, seq);
            if (err == ESP_OK && removed) {
                (*removed)++;
            }
        } else {
            err = recover_one(root, seq);
            if (err == ESP_OK && recovered) {
                (*recovered)++;
            }
        }
        if (first == ESP_OK) {
            first = err;
        }
    }
    free(seqs);
    return first;
}

esp_err_t espos_sdlog_mark_synced(const char *root, uint32_t seq, const char *note)
{
    espos_sdlog_info_t info;
    esp_err_t err = espos_sdlog_info(root, seq, &info);
    if (err != ESP_OK) {
        return err;
    }
    if (!info.closed) {
        return ESP_ERR_INVALID_STATE;
    }
    return write_atomic(root, seq, "SYNCED.TMP", FILE_NAMES[ESPOS_SDLOG_FILE_SYNCED], note ? note : "", NULL, NULL);
}

esp_err_t espos_sdlog_prune(const char *root, uint64_t keep_bytes, uint32_t *removed)
{
    if (removed) {
        *removed = 0;
    }
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t *seqs;
    size_t count;
    esp_err_t err = scan(root, &seqs, &count);
    if (err != ESP_OK) {
        return err;
    }
    espos_sdlog_info_t *infos = count ? calloc(count, sizeof(*infos)) : NULL;
    if (count && !infos) {
        free(seqs);
        return ESP_ERR_NO_MEM;
    }
    uint64_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (espos_sdlog_info(root, seqs[i], &infos[i]) == ESP_OK) {
            total += infos[i].bytes;
        } else {
            /* Unreadable: never deleted, and counted as nothing. */
            infos[i].seq = seqs[i];
            infos[i].synced = false;
            infos[i].bytes = 0;
        }
    }
    esp_err_t first = ESP_OK;
    for (size_t i = 0; i < count && total > keep_bytes; i++) {
        if (!infos[i].closed || !infos[i].synced) {
            continue;
        }
        err = espos_sdlog_remove(root, infos[i].seq);
        if (err == ESP_OK) {
            total -= infos[i].bytes;
            if (removed) {
                (*removed)++;
            }
        } else if (first == ESP_OK) {
            first = err;
        }
    }
    free(infos);
    free(seqs);
    return first;
}
