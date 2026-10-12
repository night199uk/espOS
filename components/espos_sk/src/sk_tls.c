/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Trust for the SignalK connection: one anchor record, one verify callback,
 * installed at every TLS call site (sk_http.c, sk_ws.c).
 *
 * Why this exists at all: a boat's signalk-server presents a certificate no
 * public root signed. Verifying against the bundled Mozilla roots — the only
 * thing espOS could do before — refuses every one of them, which made
 * CONFIG_ESPOS_SK_TLS unusable in the case it was written for. The decision
 * table lives in sk_tls_policy.c (pure, host-tested); this file does the
 * mbedTLS side: parse the chain, fill in the facts, apply the verdict, and
 * persist an anchor when one is captured.
 *
 * Stash, then commit. A verify callback runs during a handshake that has not
 * finished, so a machine-in-the-middle answering a first connect would
 * otherwise plant its own certificate as the anchor and be trusted from then
 * on. Instead the callback fills a single capture slot in RAM;
 * espos_sk_tls_commit() writes it to NVS and is called only after the
 * connection has proved itself (an HTTP 2xx, a WebSocket 101). Every
 * handshake begins with espos_sk_tls_discard(), so a slot never survives the
 * attempt it belongs to.
 *
 * One handshake at a time, device-wide (s_handshake_lock). Two reasons: a TLS
 * handshake wants ~20 KB of internal RAM and two at once is where an ESP32
 * with a WiFi stack runs out, and the esp-tls attach hook takes no user
 * pointer, so a single global capture slot is only well defined while a single
 * handshake owns it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

/* Needed on both sides of the switch: the hooks other components may hold
 * are declared here and exist in every build. */
#include "espos_sk_tls_policy.h"

#if CONFIG_ESPOS_SK_TLS

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#if CONFIG_SPIRAM
#include "esp_memory_utils.h"
#endif
#include "mbedtls/md.h"
#include "mbedtls/oid.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"

#include "espos_health.h"
#include "espos_httpd_sse.h"
#include "espos_sk_priv.h"
#include "espos_sk_tls.h"
#include "espos_sk_tls_policy.h"

static const char *TAG = "espos_sktls";
#define NS "skstate"

#ifndef CONFIG_ESPOS_CONFIG_NVS_PARTITION
#define CONFIG_ESPOS_CONFIG_NVS_PARTITION "nvs"
#endif

/* espos_time is optional: a build that has it gets a real "pinned at", a
 * build without it stores 0 and the UI says "unknown". A weak declaration
 * rather than a PRIV_REQUIRES keeps espos_sk buildable either way -- adding
 * the dependency would drag a clock into every SignalK firmware for one
 * cosmetic field. */
__attribute__((weak)) int64_t espos_time_now_ms(void);

static int64_t pinned_now_unix(void)
{
    if (espos_time_now_ms) {
        int64_t ms = espos_time_now_ms();
        return ms > 0 ? ms / 1000 : 0;
    }
    return 0;
}

/* ------------------------------------------------------------ the record */

typedef struct {
    espos_sk_tls_anchor_kind_t kind;
    uint8_t leaf_fp[ESPOS_SK_TLS_FP_LEN];
    uint8_t ca_fp[ESPOS_SK_TLS_FP_LEN];
    char san[ESPOS_SK_TLS_SAN_MAX];
    char cn[ESPOS_SK_TLS_CN_MAX];
    char server_self[ESPOS_SK_SELF_MAX];
    int64_t pinned_unix;
} anchor_t;

/* What one handshake observed, before anything is committed. */
typedef struct {
    bool valid;
    espos_sk_tls_anchor_kind_t capture_as;
    uint8_t leaf_fp[ESPOS_SK_TLS_FP_LEN];
    uint8_t ca_fp[ESPOS_SK_TLS_FP_LEN];
    bool have_ca;
    char san[ESPOS_SK_TLS_SAN_MAX];
    char cn[ESPOS_SK_TLS_CN_MAX];
} capture_t;

static struct {
    SemaphoreHandle_t lock;      /* guards the anchor, the capture and the status strings */
    SemaphoreHandle_t handshake; /* one TLS handshake at a time, device-wide */
    bool loaded;
    anchor_t anchor;
    bool store_pending; /* s.anchor differs from NVS: a deferred write failed to start */
    capture_t cap;
    espos_sk_tls_trust_t trust;
    char ca_pem[CONFIG_ESPOS_SK_TLS_CA_MAX]; /* trust == CA: the operator's anchor, PEM */
    mbedtls_x509_crt ca_crt;                 /* ... parsed, so the callback needs no parse per handshake */
    bool ca_crt_ready;
    /* last outcome, for GET /api/v1/sk/tls */
    char last_error[96];
    char seen_cn[ESPOS_SK_TLS_CN_MAX];
    char seen_fp[ESPOS_SK_TLS_FP_LEN * 2 + 1];
    bool have_seen;
} s;

static void lock(void)
{
    if (s.lock) {
        xSemaphoreTake(s.lock, portMAX_DELAY);
    }
}
static void unlock(void)
{
    if (s.lock) {
        xSemaphoreGive(s.lock);
    }
}

static void hex(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

static bool unhex(const char *in, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int hi = -1, lo = -1;
        char a = in[i * 2], b = in[i * 2 + 1];
        if (!a || !b) {
            return false;
        }
        hi = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10
                                                                       : -1;
        lo = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10
                                                                       : -1;
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* ------------------------------------------------------------ persistence */

static esp_err_t open_ns(nvs_handle_t *h)
{
    return nvs_open_from_partition(CONFIG_ESPOS_CONFIG_NVS_PARTITION, NS, NVS_READWRITE, h);
}

static void get_str(nvs_handle_t h, const char *key, char *buf, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
        buf[0] = '\0';
    }
}

static void anchor_load(void)
{
    memset(&s.anchor, 0, sizeof(s.anchor));
    nvs_handle_t h;
    if (open_ns(&h) != ESP_OK) {
        return;
    }
    uint8_t kind = 0;
    if (nvs_get_u8(h, "tls_kind", &kind) != ESP_OK) {
        kind = 0;
    }
    char fp[ESPOS_SK_TLS_FP_LEN * 2 + 1];
    get_str(h, "tls_fp", fp, sizeof(fp));
    if (fp[0] && !unhex(fp, s.anchor.leaf_fp, ESPOS_SK_TLS_FP_LEN)) {
        kind = 0;
    }
    get_str(h, "tls_ca", fp, sizeof(fp));
    if (fp[0] && !unhex(fp, s.anchor.ca_fp, ESPOS_SK_TLS_FP_LEN)) {
        kind = 0;
    }
    get_str(h, "tls_san", s.anchor.san, sizeof(s.anchor.san));
    get_str(h, "tls_cn", s.anchor.cn, sizeof(s.anchor.cn));
    get_str(h, "tls_self", s.anchor.server_self, sizeof(s.anchor.server_self));
    int64_t at = 0;
    if (nvs_get_i64(h, "tls_at", &at) == ESP_OK) {
        s.anchor.pinned_unix = at;
    }
    nvs_close(h);
    s.anchor.kind = (kind == 1) ? ESPOS_SK_TLS_ANCHOR_CA : (kind == 2) ? ESPOS_SK_TLS_ANCHOR_LEAF
                                                                       : ESPOS_SK_TLS_ANCHOR_NONE;
    s.loaded = true;
    if (s.anchor.kind != ESPOS_SK_TLS_ANCHOR_NONE) {
        ESP_LOGI(TAG, "trust anchor: %s%s%s", s.anchor.kind == ESPOS_SK_TLS_ANCHOR_CA ? "CA" : "certificate",
                 s.anchor.cn[0] ? " for " : "", s.anchor.cn);
    }
}

static esp_err_t anchor_store(const anchor_t *a)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(&h);
    if (err != ESP_OK) {
        return err;
    }
    char fp[ESPOS_SK_TLS_FP_LEN * 2 + 1];
    esp_err_t e;
    if ((e = nvs_set_u8(h, "tls_kind", (uint8_t)a->kind)) != ESP_OK) err = e;
    hex(a->leaf_fp, ESPOS_SK_TLS_FP_LEN, fp);
    if ((e = nvs_set_str(h, "tls_fp", fp)) != ESP_OK) err = e;
    hex(a->ca_fp, ESPOS_SK_TLS_FP_LEN, fp);
    if ((e = nvs_set_str(h, "tls_ca", fp)) != ESP_OK) err = e;
    if ((e = nvs_set_str(h, "tls_san", a->san)) != ESP_OK) err = e;
    if ((e = nvs_set_str(h, "tls_cn", a->cn)) != ESP_OK) err = e;
    if ((e = nvs_set_str(h, "tls_self", a->server_self)) != ESP_OK) err = e;
    if ((e = nvs_set_i64(h, "tls_at", a->pinned_unix)) != ESP_OK) err = e;
    if ((e = nvs_commit(h)) != ESP_OK) err = e;
    nvs_close(h);
    return err;
}

static void anchor_erase(void)
{
    nvs_handle_t h;
    if (open_ns(&h) != ESP_OK) {
        return;
    }
    static const char *const keys[] = { "tls_kind", "tls_fp", "tls_ca", "tls_san", "tls_cn", "tls_self", "tls_at" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        (void)nvs_erase_key(h, keys[i]);
    }
    (void)nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------- chain inspection */

static void crt_fingerprint(const mbedtls_x509_crt *crt, uint8_t out[ESPOS_SK_TLS_FP_LEN])
{
    /* The DER of the whole certificate, which is what "this certificate" can
     * only mean: a fingerprint over the public key alone would accept a
     * re-issue with different names, and over the subject alone anything.
     *
     * Through mbedtls_md() rather than mbedtls_sha256(): mbedTLS 4 (IDF 6)
     * moved the algorithm-specific headers under mbedtls/private/, and md.h
     * is the public way to ask for one digest of one buffer. */
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md(md, crt->raw.p, crt->raw.len, out) != 0) {
        /* Cannot happen with SHA-256 compiled in, and if it somehow does, a
         * zero fingerprint must not accidentally match a stored one -- so
         * make it a value nothing real produces. */
        memset(out, 0xff, ESPOS_SK_TLS_FP_LEN);
    }
}

/* The CN of the subject, for the operator to recognise the server by. Not
 * used in any decision -- SANs are what a modern certificate is checked
 * against -- so a missing one costs nothing. */
static void crt_cn(const mbedtls_x509_crt *crt, char *out, size_t n)
{
    out[0] = '\0';
    for (const mbedtls_x509_name *nm = &crt->subject; nm; nm = nm->next) {
        if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &nm->oid) == 0) {
            size_t len = nm->val.len < n - 1 ? nm->val.len : n - 1;
            memcpy(out, nm->val.p, len);
            out[len] = '\0';
            return;
        }
    }
}

/* Collect the leaf's dNSName and iPAddress SANs into the normalised set the
 * policy compares. Walks the raw sequence rather than
 * mbedtls_x509_parse_subject_alt_name(), which allocates per entry: this runs
 * inside a handshake, on a stack we do not own, in the memory situation the
 * pre-flight check exists to protect. */
#define SAN_ENTRY_MAX 8
#define SAN_NAME_MAX  64

static void crt_san_set(const mbedtls_x509_crt *crt, char *out, size_t out_size, bool *truncated)
{
    char names[SAN_ENTRY_MAX][SAN_NAME_MAX];
    const char *ptrs[SAN_ENTRY_MAX];
    size_t n = 0;
    bool overflow = false;
    for (const mbedtls_x509_sequence *cur = &crt->subject_alt_names; cur; cur = cur->next) {
        if (cur->buf.len == 0 || !cur->buf.p) {
            continue;
        }
        /* Context-specific tag: 2 = dNSName (a string), 7 = iPAddress (4 or
         * 16 raw bytes). Everything else -- e-mail, URI, directoryName -- says
         * nothing about which host this is and is skipped. */
        unsigned tag = cur->buf.tag & MBEDTLS_ASN1_TAG_VALUE_MASK;
        if (n >= SAN_ENTRY_MAX) {
            overflow = true;
            break;
        }
        if (tag == MBEDTLS_X509_SAN_DNS_NAME) {
            size_t len = cur->buf.len < SAN_NAME_MAX - 1 ? cur->buf.len : SAN_NAME_MAX - 1;
            memcpy(names[n], cur->buf.p, len);
            names[n][len] = '\0';
        } else if (tag == MBEDTLS_X509_SAN_IP_ADDRESS && cur->buf.len == 4) {
            snprintf(names[n], SAN_NAME_MAX, "%u.%u.%u.%u", cur->buf.p[0], cur->buf.p[1], cur->buf.p[2], cur->buf.p[3]);
        } else if (tag == MBEDTLS_X509_SAN_IP_ADDRESS && cur->buf.len == 16) {
            char *w = names[n];
            for (int i = 0; i < 16; i += 2) {
                w += snprintf(w, (size_t)(names[n] + SAN_NAME_MAX - w), "%s%02x%02x", i ? ":" : "", cur->buf.p[i],
                              cur->buf.p[i + 1]);
            }
        } else {
            continue;
        }
        ptrs[n] = names[n];
        n++;
    }
    bool cut = false;
    if (espos_sk_tls_san_normalise(ptrs, n, out, out_size, &cut) != ESP_OK || cut || overflow) {
        /* A set we could not record whole is marked, and a marked set never
         * compares equal (espos_sk_tls_san_equal), so it can only ever refuse
         * -- never accept a certificate on the strength of the part we saw. */
        if (out_size > 1) {
            memmove(out + 1, out, out_size - 2);
            out[0] = '!';
            out[out_size - 1] = '\0';
        }
        if (truncated) {
            *truncated = true;
        }
        return;
    }
    if (truncated) {
        *truncated = false;
    }
}

/* ---------------------------------------------------------- verify callback */

/* mbedTLS walks the chain from the root end down to the leaf (depth 0 last),
 * so the first call we see is the highest certificate the server sent. The
 * highest CA:TRUE among them is the anchor candidate; the leaf, at depth 0,
 * is where the decision is made -- by then everything the policy needs is in
 * the capture slot. */
static int verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)ctx;
    if (!crt) {
        return 0;
    }
    lock();
    if (depth > 0) {
        if (mbedtls_x509_crt_get_ca_istrue(crt) == 1) {
            /* Highest first: the first CA we are handed is the top of what
             * the server presented, and a later (lower) one must not replace
             * it -- pinning an intermediate would let its parent mint a new
             * one at will. */
            if (!s.cap.have_ca) {
                crt_fingerprint(crt, s.cap.ca_fp);
                s.cap.have_ca = true;
            }
        }
        unlock();
        /* Intermediates are the leaf's problem to justify; the anchor
         * decision happens once, at depth 0. Clearing the flags here is what
         * lets a chain with no publicly trusted root reach that point at all. */
        *flags = 0;
        return 0;
    }

    /* depth == 0: the leaf. */
    crt_fingerprint(crt, s.cap.leaf_fp);
    crt_cn(crt, s.cap.cn, sizeof(s.cap.cn));
    bool san_cut = false;
    crt_san_set(crt, s.cap.san, sizeof(s.cap.san), &san_cut);

    espos_sk_tls_facts_t f = {
        .has_anchor = s.anchor.kind != ESPOS_SK_TLS_ANCHOR_NONE,
        .anchor_is_leaf = s.anchor.kind == ESPOS_SK_TLS_ANCHOR_LEAF,
        .leaf_matches = memcmp(s.cap.leaf_fp, s.anchor.leaf_fp, ESPOS_SK_TLS_FP_LEN) == 0,
        .ca_present = s.cap.have_ca,
        .ca_matches = s.cap.have_ca && memcmp(s.cap.ca_fp, s.anchor.ca_fp, ESPOS_SK_TLS_FP_LEN) == 0,
        .leaf_has_san = s.cap.san[0] != '\0' && !san_cut,
        .san_matches = espos_sk_tls_san_equal(s.cap.san, s.anchor.san),
    };
    espos_sk_tls_decision_t d = espos_sk_tls_decide(&f);

    /* What the operator sees on the SignalK page as "presented", whatever the
     * verdict -- a rejected certificate is exactly the one worth showing. */
    hex(s.cap.leaf_fp, ESPOS_SK_TLS_FP_LEN, s.seen_fp);
    snprintf(s.seen_cn, sizeof(s.seen_cn), "%s", s.cap.cn);
    s.have_seen = true;

    int rc = 0;
    if (d.verdict == ESPOS_SK_TLS_REJECT) {
        snprintf(s.last_error, sizeof(s.last_error), "%s", espos_sk_tls_reason_str(d.reason));
        s.cap.valid = false;
        rc = MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    } else {
        s.last_error[0] = '\0';
        /* ACCEPT re-arms the slot too: a CA-anchored connection may have
         * renewed the leaf, and committing after success keeps the recorded
         * CN and fingerprint the ones actually in use. */
        s.cap.capture_as = d.verdict == ESPOS_SK_TLS_CAPTURE ? d.capture_as : s.anchor.kind;
        s.cap.valid = true;
        *flags = 0;
    }
    unlock();
    if (rc != 0) {
        ESP_LOGW(TAG, "certificate refused: %s", espos_sk_tls_reason_str(d.reason));
    } else if (d.verdict == ESPOS_SK_TLS_CAPTURE) {
        ESP_LOGI(TAG, "first use: will pin the server's %s once the connection works",
                 d.capture_as == ESPOS_SK_TLS_ANCHOR_CA ? "CA" : "certificate");
    }
    return rc;
}

/* -------------------------------------------------------------- public API */

static inline bool run_on_internal_stack(void (*fn)(void));

static void ensure_init(void)
{
    if (!s.lock) {
        SemaphoreHandle_t l = xSemaphoreCreateMutex();
        SemaphoreHandle_t hs = xSemaphoreCreateMutex();
        s.lock = l;
        s.handshake = hs;
        mbedtls_x509_crt_init(&s.ca_crt);
    }
    /* Normally done at boot; a retry after a failed open can come from a
     * task whose stack is in PSRAM. */
    if (!s.loaded) {
        (void)run_on_internal_stack(anchor_load);
    }
}

void espos_sk_tls_set_trust(espos_sk_tls_trust_t trust, const char *ca_pem)
{
    ensure_init();
    lock();
    bool pem_changed = ca_pem && strncmp(s.ca_pem, ca_pem, sizeof(s.ca_pem)) != 0;
    s.trust = trust;
    if (ca_pem) {
        snprintf(s.ca_pem, sizeof(s.ca_pem), "%s", ca_pem);
    }
    if (pem_changed) {
        if (s.ca_crt_ready) {
            mbedtls_x509_crt_free(&s.ca_crt);
            mbedtls_x509_crt_init(&s.ca_crt);
            s.ca_crt_ready = false;
        }
        if (s.ca_pem[0] && espos_sk_tls_parse_ca(s.ca_pem, &s.ca_crt) == ESP_OK) {
            s.ca_crt_ready = true;
            /* An operator-supplied CA IS the anchor: adopt it without waiting
             * for a handshake, so a fleet that pre-seeds sk.ca_pem connects on
             * the first try instead of pinning whatever answered first. */
            anchor_t a = { .kind = ESPOS_SK_TLS_ANCHOR_CA, .pinned_unix = pinned_now_unix() };
            crt_fingerprint(&s.ca_crt, a.ca_fp);
            crt_cn(&s.ca_crt, a.cn, sizeof(a.cn));
            /* No SAN yet: the CA does not name the server. The first
             * successful connect binds the leaf's set (commit below). */
            s.anchor = a;
            s.store_pending = anchor_store(&a) != ESP_OK;
            ESP_LOGI(TAG, "CA supplied by configuration: anchored on %s", a.cn[0] ? a.cn : "(no CN)");
        }
    }
    unlock();
}

espos_sk_tls_trust_t espos_sk_tls_trust_mode(void)
{
    ensure_init();
    lock();
    espos_sk_tls_trust_t t = s.trust;
    unlock();
    return t;
}

esp_err_t espos_sk_tls_parse_ca(const char *pem, mbedtls_x509_crt *out)
{
    if (!pem || !pem[0] || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(pem);
    if (len + 1 > CONFIG_ESPOS_SK_TLS_CA_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* mbedtls_x509_crt_parse wants the NUL counted for PEM input. */
    int rc = mbedtls_x509_crt_parse(out, (const unsigned char *)pem, len + 1);
    if (rc != 0) {
        /* The code is worth logging: -0x2180 (invalid format) is a bad paste,
         * -0x3b00 (invalid public key) is a key type this build of mbedTLS
         * cannot handle, and the two want different things from the operator. */
        ESP_LOGW(TAG, "CA certificate rejected: mbedtls_x509_crt_parse -0x%04x (%u bytes)", (unsigned)-rc,
                 (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t espos_sk_tls_attach(void *ssl_conf)
{
    ensure_init();
    lock();
    espos_sk_tls_trust_t trust = s.trust;
    unlock();
    if (trust == ESPOS_SK_TLS_TRUST_BUNDLE) {
        return esp_crt_bundle_attach(ssl_conf);
    }
    if (!ssl_conf) {
        return ESP_OK;
    }
    mbedtls_ssl_config *conf = (mbedtls_ssl_config *)ssl_conf;
    /* The chain the server sends is the only chain: our callback decides,
     * and mbedTLS only needs a non-NULL ca_chain to get as far as calling it.
     * (esp-tls has already set MBEDTLS_SSL_VERIFY_REQUIRED, which is what
     * makes the callback run at all -- OPTIONAL would let a failure through.) */
    lock();
    mbedtls_x509_crt *chain = s.ca_crt_ready ? &s.ca_crt : NULL;
    unlock();
    if (chain) {
        mbedtls_ssl_conf_ca_chain(conf, chain, NULL);
    } else {
        /* No CA to offer: the bundle's own dummy chain is what esp_crt_bundle
         * uses for exactly this, and attaching it costs only the pointer --
         * our verify callback replaces its one straight after. */
        esp_err_t err = esp_crt_bundle_attach(conf);
        if (err != ESP_OK) {
            return err;
        }
    }
    mbedtls_ssl_conf_verify(conf, verify_cb, NULL);
    return ESP_OK;
}

void espos_sk_tls_discard(void)
{
    ensure_init();
    lock();
    memset(&s.cap, 0, sizeof(s.cap));
    unlock();
}

/* NVS access switches the cache, and with it PSRAM, off: a caller whose stack
 * is in PSRAM (the BLE gateway with CONFIG_ESPOS_BLE_STACK_IN_PSRAM) would
 * fault mid-access. espos_sk calls run on whichever task made the request, so
 * such a caller hands the NVS work to a short-lived task on an internal stack
 * and waits for it. Returns false if that task could not be started. Without
 * PSRAM it is a plain call: the C5's image sits close to its size budget. */
#if CONFIG_SPIRAM
typedef struct {
    void (*fn)(void);
    SemaphoreHandle_t done;
} nvs_job_t;

static void nvs_job_task(void *arg)
{
    nvs_job_t *job = arg;
    /* No lock() here: the caller waits for this task, and when it holds the
     * lock nothing can change the anchor until the work is done. */
    job->fn();
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

static bool run_on_internal_stack(void (*fn)(void))
{
    int probe = 0; /* only its address is used */
    if (!esp_ptr_external_ram(&probe)) {
        fn();
        return true;
    }
    /* A semaphore rather than a task notification: the caller is whatever
     * task made the request, and its notification value may be in use. */
    nvs_job_t job = { .fn = fn, .done = xSemaphoreCreateBinary() };
    bool ran = job.done && xTaskCreate(nvs_job_task, "espos_sktls", 4096, &job, 5, NULL) == pdPASS;
    if (ran) {
        (void)xSemaphoreTake(job.done, portMAX_DELAY);
    }
    if (job.done) {
        vSemaphoreDelete(job.done);
    }
    return ran;
}
#else
static inline bool run_on_internal_stack(void (*fn)(void))
{
    fn();
    return true;
}
#endif

static void store_anchor(void)
{
    s.store_pending = anchor_store(&s.anchor) != ESP_OK;
}

/* Called with the lock held, s.anchor already updated. Waits for the write
 * even when it is handed off: a commit returns with the anchor in NVS either
 * way, so a reboot straight after cannot leave the next connect to pin
 * afresh. */
static void persist_anchor(void)
{
    if (!run_on_internal_stack(store_anchor)) {
        s.store_pending = true;
        ESP_LOGW(TAG, "no memory to save the trust anchor now; retrying on the next connection");
    }
}

void espos_sk_tls_commit(const char *server_self)
{
    ensure_init();
    lock();
    if (!s.cap.valid) {
        unlock();
        return;
    }
    anchor_t a = s.anchor;
    bool changed = false;
    if (a.kind == ESPOS_SK_TLS_ANCHOR_NONE) {
        a.kind = s.cap.capture_as;
        a.pinned_unix = pinned_now_unix();
        changed = true;
    }
    if (memcmp(a.leaf_fp, s.cap.leaf_fp, ESPOS_SK_TLS_FP_LEN) != 0) {
        memcpy(a.leaf_fp, s.cap.leaf_fp, ESPOS_SK_TLS_FP_LEN);
        changed = true;
    }
    if (s.cap.have_ca && memcmp(a.ca_fp, s.cap.ca_fp, ESPOS_SK_TLS_FP_LEN) != 0) {
        memcpy(a.ca_fp, s.cap.ca_fp, ESPOS_SK_TLS_FP_LEN);
        changed = true;
    }
    /* The SAN set is only ever written when it is whole (crt_san_set marks a
     * partial one with '!'), so a certificate we could not read fully never
     * becomes the thing later connections are held to. */
    if (s.cap.san[0] && s.cap.san[0] != '!' && strcmp(a.san, s.cap.san) != 0) {
        snprintf(a.san, sizeof(a.san), "%s", s.cap.san);
        changed = true;
    }
    if (strcmp(a.cn, s.cap.cn) != 0) {
        snprintf(a.cn, sizeof(a.cn), "%s", s.cap.cn);
        changed = true;
    }
    if (server_self && strcmp(a.server_self, server_self) != 0) {
        snprintf(a.server_self, sizeof(a.server_self), "%s", server_self);
        changed = true;
    }
    memset(&s.cap, 0, sizeof(s.cap));
    if (!changed) {
        if (s.store_pending) {
            persist_anchor();
        }
        unlock();
        return;
    }
    s.anchor = a;
    persist_anchor();
    unlock();
    ESP_LOGI(TAG, "pinned the server's %s%s%s", a.kind == ESPOS_SK_TLS_ANCHOR_CA ? "CA" : "certificate",
             a.cn[0] ? " for " : "", a.cn);
    espos_sk_tls_publish();
}

esp_err_t espos_sk_tls_reset(void)
{
    ensure_init();
    lock();
    memset(&s.anchor, 0, sizeof(s.anchor));
    memset(&s.cap, 0, sizeof(s.cap));
    s.store_pending = false;
    s.last_error[0] = '\0';
    anchor_erase();
    unlock();
    ESP_LOGI(TAG, "trust anchor cleared; the next connection pins what the server presents");
    espos_sk_tls_publish();
    return ESP_OK;
}

const char *espos_sk_tls_last_error(void)
{
    /* Read on the SK task for the health condition; the string only ever
     * grows a NUL earlier, so a torn read cannot run off the end. */
    return s.last_error;
}

/* -------------------------------------------------------- handshake gating */

esp_err_t espos_sk_tls_handshake_begin(uint32_t timeout_ms)
{
    ensure_init();
    if (!s.handshake) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s.handshake, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* Pre-flight. A handshake needs a contiguous internal block for the
     * record buffers, and the failure when it is not there is not a clean
     * "no memory" -- it is a half-built session, a leaked socket, and on a
     * bad day a heap the WiFi stack then cannot allocate from either. Better
     * to defer and say so. The pool is the one mbedTLS allocates from: with
     * CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC that is PSRAM, and measuring internal
     * RAM would refuse handshakes that never touch it. */
#if CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const char *pool = "PSRAM";
#else
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const char *pool = "internal";
#endif
    size_t largest = heap_caps_get_largest_free_block(caps);
    if (largest < (size_t)CONFIG_ESPOS_SK_TLS_MIN_FREE_BLOCK_KB * 1024) {
        char msg[ESPOS_HEALTH_MSG_MAX];
        snprintf(msg, sizeof(msg), "largest free %s block %u KB, need %d KB for a TLS handshake", pool,
                 (unsigned)(largest / 1024), CONFIG_ESPOS_SK_TLS_MIN_FREE_BLOCK_KB);
        (void)espos_health_report("tlsMemory", ESPOS_HEALTH_WARN, msg);
        xSemaphoreGive(s.handshake);
        return ESP_ERR_NO_MEM;
    }
    (void)espos_health_report("tlsMemory", ESPOS_HEALTH_NORMAL, "");
    espos_sk_tls_discard();
    return ESP_OK;
}

void espos_sk_tls_handshake_end(void)
{
    if (s.handshake) {
        xSemaphoreGive(s.handshake);
    }
}

/* -------------------------------------------------------------- the document */

char *espos_sk_tls_json(void)
{
    ensure_init();
    static const char *const kinds[] = { "none", "ca", "leaf" };
    char *out = malloc(1024);
    if (!out) {
        return NULL;
    }
    lock();
    char fp[ESPOS_SK_TLS_FP_LEN * 2 + 1];
    hex(s.anchor.kind == ESPOS_SK_TLS_ANCHOR_CA ? s.anchor.ca_fp : s.anchor.leaf_fp, ESPOS_SK_TLS_FP_LEN, fp);
    const char *trust = s.trust == ESPOS_SK_TLS_TRUST_BUNDLE ? "bundle" : s.trust == ESPOS_SK_TLS_TRUST_CA ? "ca"
                                                                                                           : "tofu";
    int n = snprintf(out, 1024, "{\"trust\":\"%s\",\"pinned\":", trust);
    if (s.anchor.kind == ESPOS_SK_TLS_ANCHOR_NONE) {
        n += snprintf(out + n, (size_t)(1024 - n), "null");
    } else {
        n += snprintf(out + n, (size_t)(1024 - n),
                      "{\"kind\":\"%s\",\"cn\":\"%.63s\",\"san\":\"%.255s\",\"fingerprint\":\"%s\",\"since\":%lld}",
                      kinds[s.anchor.kind], s.anchor.cn, s.anchor.san, fp, (long long)s.anchor.pinned_unix);
    }
    n += snprintf(out + n, (size_t)(1024 - n), ",\"last_error\":\"%.95s\",\"presented\":", s.last_error);
    if (s.have_seen) {
        n += snprintf(out + n, (size_t)(1024 - n), "{\"cn\":\"%.63s\",\"fingerprint\":\"%s\"}}", s.seen_cn, s.seen_fp);
    } else {
        n += snprintf(out + n, (size_t)(1024 - n), "null}");
    }
    unlock();
    return out;
}

void espos_sk_tls_publish(void)
{
    char *json = espos_sk_tls_json();
    if (json) {
        espos_httpd_sse_publish("sk_tls", json);
        free(json);
    }
}

#else /* !CONFIG_ESPOS_SK_TLS */

/* The two hooks a component outside espos_sk may hold (espos_sk_tls_policy.h)
 * exist in every build, so a caller guarded by its own `srv.tls` check still
 * links when TLS is compiled out -- and gets an answer that cannot be mistaken
 * for "verified". */
esp_err_t espos_sk_tls_attach(void *ssl_conf)
{
    (void)ssl_conf;
    return ESP_ERR_INVALID_STATE;
}

espos_sk_tls_trust_t espos_sk_tls_trust_mode(void)
{
    return ESPOS_SK_TLS_TRUST_BUNDLE;
}

#endif /* CONFIG_ESPOS_SK_TLS */
