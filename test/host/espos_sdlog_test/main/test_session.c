/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "espos_sdlog.h"
#include "unity.h"

/* A fresh root per test, under /tmp, so a test never sees another's files. */
static char root[128];

static void fresh_root(void)
{
    char tmpl[] = "/tmp/espos_sdlog_XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(tmpl));
    snprintf(root, sizeof(root), "%s/trips", tmpl);
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (len) {
        *len = n;
    }
    return buf;
}

static char *file_of(uint32_t seq, espos_sdlog_file_t which)
{
    static char p[200];
    TEST_ASSERT_NOT_EQUAL(0, espos_sdlog_path(root, seq, which, p, sizeof(p)));
    return p;
}

static void write_raw(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs(text, f);
    fclose(f);
}

static void sha_of(const char *text, char hex[ESPOS_SDLOG_SHA256_HEX_MAX])
{
    espos_sdlog_sha256_t c;
    espos_sdlog_sha256_init(&c);
    espos_sdlog_sha256_update(&c, text, strlen(text));
    espos_sdlog_sha256_hex(&c, hex);
}

static uint32_t one_session(const char *header, const char *const *rows, int n, const char *meta)
{
    espos_sdlog_session_t s;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_open(root, header, NULL, &s));
    uint32_t seq = espos_sdlog_seq(s);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_append(s, rows[i]));
    }
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_close(s, meta));
    return seq;
}

TEST_CASE("a session keeps its header, its rows and a close record that hashes them", "[session]")
{
    fresh_root();
    espos_sdlog_session_t s;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_open(root, "{\"trip\":\"a\"}", NULL, &s));
    TEST_ASSERT_EQUAL_UINT32(1, espos_sdlog_seq(s));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_append(s, "t,lat,lon,depth"));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_append(s, "1,54.1,10.2,3.4"));
    TEST_ASSERT_EQUAL_UINT64(2, espos_sdlog_rows(s));
    TEST_ASSERT_EQUAL_UINT64(32, espos_sdlog_bytes(s));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_close(s, "{\"end\":\"2026-10-16T18:00:00.000Z\"}"));

    TEST_ASSERT_EQUAL_STRING("{\"trip\":\"a\"}", slurp(file_of(1, ESPOS_SDLOG_FILE_HEADER), NULL));
    const char *rows = "t,lat,lon,depth\n1,54.1,10.2,3.4\n";
    TEST_ASSERT_EQUAL_STRING(rows, slurp(file_of(1, ESPOS_SDLOG_FILE_ROWS), NULL));
    char sha[ESPOS_SDLOG_SHA256_HEX_MAX];
    sha_of(rows, sha);
    char want[400];
    snprintf(want, sizeof(want),
             "{\"closed\":true,\"clean\":true,\"rows\":2,\"bytes\":32,\"sha256\":\"%s\","
             "\"meta\":{\"end\":\"2026-10-16T18:00:00.000Z\"}}\n",
             sha);
    TEST_ASSERT_EQUAL_STRING(want, slurp(file_of(1, ESPOS_SDLOG_FILE_CLOSE), NULL));

    espos_sdlog_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_info(root, 1, &info));
    TEST_ASSERT_TRUE(info.closed);
    TEST_ASSERT_TRUE(info.clean);
    TEST_ASSERT_FALSE(info.synced);
    TEST_ASSERT_EQUAL_UINT64(2, info.rows);
    TEST_ASSERT_EQUAL_UINT64(32, info.bytes);
    TEST_ASSERT_EQUAL_STRING(sha, info.sha256);
}

TEST_CASE("a close without meta records null, and no temporary file is left", "[session]")
{
    fresh_root();
    uint32_t seq = one_session("{}", NULL, 0, NULL);
    const char *close = slurp(file_of(seq, ESPOS_SDLOG_FILE_CLOSE), NULL);
    TEST_ASSERT_NOT_NULL(strstr(close, "\"rows\":0,\"bytes\":0,"));
    TEST_ASSERT_NOT_NULL(strstr(close, "\"meta\":null}"));
    char p[200];
    snprintf(p, sizeof(p), "%s/%08u/CLOSE.TMP", root, (unsigned)seq);
    TEST_ASSERT_NOT_EQUAL(0, access(p, F_OK));
    snprintf(p, sizeof(p), "%s/%08u/HEAD.TMP", root, (unsigned)seq);
    TEST_ASSERT_NOT_EQUAL(0, access(p, F_OK));
}

TEST_CASE("sessions are numbered one above the highest, and other names are ignored", "[session]")
{
    fresh_root();
    TEST_ASSERT_EQUAL_UINT32(1, one_session("{}", NULL, 0, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, one_session("{}", NULL, 0, NULL));
    char p[200];
    /* A directory and a file the component did not make: another writer's. */
    snprintf(p, sizeof(p), "%s/00000041", root);
    TEST_ASSERT_EQUAL(0, mkdir(p, 0775));
    snprintf(p, sizeof(p), "%s/README.TXT", root);
    write_raw(p, "not a session");
    snprintf(p, sizeof(p), "%s/123", root);
    TEST_ASSERT_EQUAL(0, mkdir(p, 0775));
    TEST_ASSERT_EQUAL_UINT32(42, one_session("{}", NULL, 0, NULL));
    snprintf(p, sizeof(p), "%s/README.TXT", root);
    TEST_ASSERT_EQUAL_STRING("not a session", slurp(p, NULL));
}

TEST_CASE("a row with a line break is refused, since it would read back as two", "[session]")
{
    fresh_root();
    espos_sdlog_session_t s;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_open(root, "{}", NULL, &s));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sdlog_append(s, "a\nb"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sdlog_append(s, "a\rb"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sdlog_append(s, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, espos_sdlog_rows(s));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_close(s, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_sdlog_open(root, NULL, NULL, &s));
}

TEST_CASE("synced rows are on disk before the session is closed", "[session]")
{
    fresh_root();
    const espos_sdlog_open_cfg_t cfg = { .sync_every = 2 };
    espos_sdlog_session_t s;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_open(root, "{}", &cfg, &s));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_append(s, "one"));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_append(s, "two"));
    TEST_ASSERT_EQUAL_STRING("one\ntwo\n", slurp(file_of(1, ESPOS_SDLOG_FILE_ROWS), NULL));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_append(s, "three"));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_sync(s));
    TEST_ASSERT_EQUAL_STRING("one\ntwo\nthree\n", slurp(file_of(1, ESPOS_SDLOG_FILE_ROWS), NULL));

    espos_sdlog_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_info(root, 1, &info));
    TEST_ASSERT_FALSE(info.closed);
    TEST_ASSERT_EQUAL_UINT64(14, info.bytes);
    TEST_ASSERT_EQUAL_STRING("", info.sha256);
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_close(s, NULL));
}

TEST_CASE("recover cuts the half row a power cut left and closes the session unclean", "[recover]")
{
    fresh_root();
    espos_sdlog_session_t s;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_open(root, "{}", NULL, &s));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_append(s, "one"));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_append(s, "two"));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_sync(s));
    /* The power goes mid-row: what the card holds is the rows and a stub. */
    write_raw(file_of(1, ESPOS_SDLOG_FILE_ROWS), "one\ntwo\nthr");
    /* The handle dies with the power; nothing closes it. Its FILE leaks on
     * purpose, and holds nothing unwritten: the rows were synced above. */
    free(s);

    uint32_t recovered, removed;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_recover(root, &recovered, &removed));
    TEST_ASSERT_EQUAL_UINT32(1, recovered);
    TEST_ASSERT_EQUAL_UINT32(0, removed);
    TEST_ASSERT_EQUAL_STRING("one\ntwo\n", slurp(file_of(1, ESPOS_SDLOG_FILE_ROWS), NULL));

    espos_sdlog_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_info(root, 1, &info));
    TEST_ASSERT_TRUE(info.closed);
    TEST_ASSERT_FALSE(info.clean);
    TEST_ASSERT_EQUAL_UINT64(2, info.rows);
    TEST_ASSERT_EQUAL_UINT64(8, info.bytes);
    char sha[ESPOS_SDLOG_SHA256_HEX_MAX];
    sha_of("one\ntwo\n", sha);
    TEST_ASSERT_EQUAL_STRING(sha, info.sha256);

    /* A second boot finds nothing more to do. */
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_recover(root, &recovered, &removed));
    TEST_ASSERT_EQUAL_UINT32(0, recovered);
}

TEST_CASE("recover removes a session that never got its header", "[recover]")
{
    fresh_root();
    TEST_ASSERT_EQUAL_UINT32(1, one_session("{}", NULL, 0, NULL));
    char p[200];
    snprintf(p, sizeof(p), "%s/00000002", root);
    TEST_ASSERT_EQUAL(0, mkdir(p, 0775));
    snprintf(p, sizeof(p), "%s/00000002/HEAD.TMP", root);
    write_raw(p, "{\"half");

    uint32_t recovered, removed;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_recover(root, &recovered, &removed));
    TEST_ASSERT_EQUAL_UINT32(0, recovered);
    TEST_ASSERT_EQUAL_UINT32(1, removed);
    espos_sdlog_info_t info;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_sdlog_info(root, 2, &info));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_info(root, 1, &info));
}

TEST_CASE("recover finishes a close the power cut interrupted", "[recover]")
{
    fresh_root();
    espos_sdlog_session_t s;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_open(root, "{}", NULL, &s));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_append(s, "one"));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_sync(s));
    free(s);
    char p[200];
    snprintf(p, sizeof(p), "%s/00000001/CLOSE.TMP", root);
    write_raw(p, "{\"closed\":tr");

    uint32_t recovered;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_recover(root, &recovered, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, recovered);
    TEST_ASSERT_NOT_EQUAL(0, access(p, F_OK));
    espos_sdlog_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_info(root, 1, &info));
    TEST_ASSERT_TRUE(info.closed);
    TEST_ASSERT_EQUAL_UINT64(1, info.rows);
}

static int visits;
static uint32_t visited[8];

static bool collect(const espos_sdlog_info_t *info, void *arg)
{
    (void)arg;
    if (visits < 8) {
        visited[visits] = info->seq;
    }
    visits++;
    return true;
}

static bool first_only(const espos_sdlog_info_t *info, void *arg)
{
    *(uint32_t *)arg = info->seq;
    return false;
}

TEST_CASE("list visits sessions oldest first and stops when asked", "[list]")
{
    fresh_root();
    visits = 0;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_list(root, collect, NULL));
    TEST_ASSERT_EQUAL(0, visits); /* a root that does not exist yet is empty */
    for (int i = 0; i < 3; i++) {
        one_session("{}", NULL, 0, NULL);
    }
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_list(root, collect, NULL));
    TEST_ASSERT_EQUAL(3, visits);
    TEST_ASSERT_EQUAL_UINT32(1, visited[0]);
    TEST_ASSERT_EQUAL_UINT32(2, visited[1]);
    TEST_ASSERT_EQUAL_UINT32(3, visited[2]);
    uint32_t first = 0;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_list(root, first_only, &first));
    TEST_ASSERT_EQUAL_UINT32(1, first);
}

TEST_CASE("only a closed session can be marked synced", "[sync]")
{
    fresh_root();
    espos_sdlog_session_t s;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_open(root, "{}", NULL, &s));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_sdlog_mark_synced(root, 1, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_close(s, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_mark_synced(root, 1, "201"));
    TEST_ASSERT_EQUAL_STRING("201", slurp(file_of(1, ESPOS_SDLOG_FILE_SYNCED), NULL));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_mark_synced(root, 1, "200"));
    TEST_ASSERT_EQUAL_STRING("200", slurp(file_of(1, ESPOS_SDLOG_FILE_SYNCED), NULL));
    espos_sdlog_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_info(root, 1, &info));
    TEST_ASSERT_TRUE(info.synced);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_sdlog_mark_synced(root, 9, NULL));
}

TEST_CASE("prune deletes the oldest synced sessions and never an unsynced one", "[prune]")
{
    fresh_root();
    const char *rows[] = { "0123456789" }; /* 11 bytes a session */
    for (int i = 0; i < 4; i++) {
        one_session("{}", rows, 1, NULL);
    }
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_mark_synced(root, 2, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_mark_synced(root, 3, NULL));

    uint32_t removed;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_prune(root, 44, &removed));
    TEST_ASSERT_EQUAL_UINT32(0, removed); /* already within the limit */

    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_prune(root, 33, &removed));
    TEST_ASSERT_EQUAL_UINT32(1, removed);
    espos_sdlog_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_info(root, 1, &info)); /* oldest, but not synced */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_sdlog_info(root, 2, &info));

    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_prune(root, 0, &removed));
    TEST_ASSERT_EQUAL_UINT32(1, removed);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_sdlog_info(root, 3, &info));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_info(root, 1, &info));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_info(root, 4, &info));
}

TEST_CASE("a close record this did not write is reported, not trusted", "[list]")
{
    fresh_root();
    one_session("{}", NULL, 0, NULL);
    write_raw(file_of(1, ESPOS_SDLOG_FILE_CLOSE), "{\"closed\":true}");
    espos_sdlog_info_t info;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, espos_sdlog_info(root, 1, &info));
    visits = 0;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_list(root, collect, NULL));
    TEST_ASSERT_EQUAL(0, visits);
    uint32_t removed;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_prune(root, 0, &removed));
    TEST_ASSERT_EQUAL_UINT32(0, removed);
}

TEST_CASE("a pruned session's number is never handed out again", "[prune]")
{
    fresh_root();
    const char *rows[] = { "x" };
    TEST_ASSERT_EQUAL_UINT32(1, one_session("{}", rows, 1, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, one_session("{}", rows, 1, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_mark_synced(root, 1, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_mark_synced(root, 2, NULL));
    uint32_t removed;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_prune(root, 0, &removed));
    TEST_ASSERT_EQUAL_UINT32(2, removed);
    /* The card is empty of sessions; the next is still 3, not 1. */
    TEST_ASSERT_EQUAL_UINT32(3, one_session("{}", NULL, 0, NULL));

    /* A lost LASTSEQ.TXT falls back to the highest session there. */
    char p[200];
    snprintf(p, sizeof(p), "%s/LASTSEQ.TXT", root);
    TEST_ASSERT_EQUAL(0, unlink(p));
    TEST_ASSERT_EQUAL_UINT32(4, one_session("{}", NULL, 0, NULL));
    TEST_ASSERT_EQUAL_STRING("4\n", slurp(p, NULL));

    /* A power cut between removing LASTSEQ.TXT and renaming the synced
     * LASTSEQ.TMP onto it leaves only the temporary file, which still counts. */
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_mark_synced(root, 3, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_mark_synced(root, 4, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_prune(root, 0, &removed));
    TEST_ASSERT_EQUAL(0, unlink(p));
    char t[200];
    snprintf(t, sizeof(t), "%s/LASTSEQ.TMP", root);
    write_raw(t, "7\n");
    TEST_ASSERT_EQUAL_UINT32(8, one_session("{}", NULL, 0, NULL));
    TEST_ASSERT_EQUAL_STRING("8\n", slurp(p, NULL));
}

TEST_CASE("row_crc ends every row with the CRC-32 of what comes before it", "[session]")
{
    fresh_root();
    const espos_sdlog_open_cfg_t cfg = { .row_crc = true };
    espos_sdlog_session_t s;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_open(root, "{}", &cfg, &s));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_append(s, "123456789"));
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_append(s, "t,depth"));
    uint32_t seq = espos_sdlog_seq(s);
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_close(s, NULL));

    char expect[64];
    snprintf(expect, sizeof(expect), "123456789,cbf43926\nt,depth,%08" PRIx32 "\n",
             espos_sdlog_crc32(0, "t,depth", 7));
    size_t len;
    TEST_ASSERT_EQUAL_STRING(expect, slurp(file_of(seq, ESPOS_SDLOG_FILE_ROWS), &len));

    espos_sdlog_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_info(root, seq, &info));
    TEST_ASSERT_EQUAL_UINT64(len, info.bytes);
    char hex[ESPOS_SDLOG_SHA256_HEX_MAX];
    sha_of(expect, hex);
    TEST_ASSERT_EQUAL_STRING(hex, info.sha256);
}

TEST_CASE("recover gives a session cut off before its rows file an empty one", "[recover]")
{
    fresh_root();
    TEST_ASSERT_EQUAL(0, mkdir(root, 0775));
    char p[200];
    snprintf(p, sizeof(p), "%s/00000001", root);
    TEST_ASSERT_EQUAL(0, mkdir(p, 0775));
    snprintf(p, sizeof(p), "%s/00000001/HEAD.JSN", root);
    write_raw(p, "{}");

    uint32_t recovered, removed;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_recover(root, &recovered, &removed));
    TEST_ASSERT_EQUAL_UINT32(1, recovered);
    size_t len = 99;
    TEST_ASSERT_EQUAL_STRING("", slurp(file_of(1, ESPOS_SDLOG_FILE_ROWS), &len));
    TEST_ASSERT_EQUAL(0, len);
    espos_sdlog_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, espos_sdlog_info(root, 1, &info));
    TEST_ASSERT_TRUE(info.closed);
    TEST_ASSERT_EQUAL_UINT64(0, info.rows);
}
