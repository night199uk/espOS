/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The trust decision for a SignalK server certificate — pure C, no mbedTLS,
 * no IDF. sk_tls.c parses the chain and hands the facts here; this file
 * decides what to do with them and is driven directly by a host test.
 *
 * The problem it exists for: a boat's signalk-server almost always presents a
 * certificate no public root signed — self-signed, or issued by a CA the owner
 * generated once. Verifying against the Mozilla bundle refuses every one of
 * them, and an "accept anything" switch would make the setting a decoration.
 * So espOS does what SSH does: trust on first use, then hold the server to it.
 *
 * Two shapes of anchor, because certificates get renewed:
 *
 *   CA anchor    the highest CA:TRUE certificate in the presented chain, plus
 *                the leaf's SAN set. A renewal signed by the same CA for the
 *                same names is accepted with no operator involvement, which is
 *                what makes a 90-day certificate survivable on a device nobody
 *                logs into. Binding the SAN set as well as the CA matters: a
 *                private CA that signs one host would otherwise vouch for any
 *                other name it ever signs.
 *   leaf anchor  the SHA-256 of the leaf certificate itself. Where a chain has
 *                no CA at all, or a leaf carries no SAN — signalk-server's own
 *                generated self-signed certificate is exactly this — there is
 *                nothing else to pin, and a renewal then needs one deliberate
 *                "trust the new certificate" from the operator.
 *
 * The decision table is the same one for tofu and ca mode: "ca" is TOFU with
 * the anchor supplied by the operator up front instead of captured on the
 * first handshake, so both walk the same code and are tested together.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sizes are the record's, so a caller can stack-allocate one of each. The
 * SAN set is stored normalised (see espos_sk_tls_san_normalise) and capped:
 * a certificate naming more hosts than fit is pinned by the names that did
 * fit plus a truncation flag, and truncation refuses to match, so a partial
 * set can never widen what is accepted. */
#define ESPOS_SK_TLS_SAN_MAX 256
#define ESPOS_SK_TLS_CN_MAX  64
#define ESPOS_SK_TLS_FP_LEN  32 /* SHA-256, raw bytes */

/* What kind of anchor is stored (persisted as tls_kind, so the values are
 * part of the on-device format and must not be renumbered). */
typedef enum {
    ESPOS_SK_TLS_ANCHOR_NONE = 0, /* nothing pinned: the next handshake captures one */
    ESPOS_SK_TLS_ANCHOR_CA = 1,   /* a CA certificate plus the SAN set it may vouch for */
    ESPOS_SK_TLS_ANCHOR_LEAF = 2, /* the leaf's own fingerprint */
} espos_sk_tls_anchor_kind_t;

/* How the connection should be verified. Persisted as the sk.tls_trust enum
 * string, not as a number. */
typedef enum {
    ESPOS_SK_TLS_TRUST_TOFU = 0,   /* pin what the first handshake presents */
    ESPOS_SK_TLS_TRUST_CA = 1,     /* the operator supplied the CA (sk.ca_pem) */
    ESPOS_SK_TLS_TRUST_BUNDLE = 2, /* the bundled Mozilla roots, nothing pinned */
} espos_sk_tls_trust_t;

/* The outcome of one decision. */
typedef enum {
    ESPOS_SK_TLS_ACCEPT = 0,  /* the chain matches what is pinned */
    ESPOS_SK_TLS_CAPTURE = 1, /* nothing pinned yet: accept and pin what was presented */
    ESPOS_SK_TLS_REJECT = 2,  /* pinned, and this is not it */
} espos_sk_tls_verdict_t;

/* Why a decision came out the way it did — the string an operator reads on
 * the SignalK page, and what the health condition carries. */
typedef enum {
    ESPOS_SK_TLS_R_OK = 0,
    ESPOS_SK_TLS_R_FIRST_USE,     /* captured: nothing was pinned */
    ESPOS_SK_TLS_R_LEAF_CHANGED,  /* leaf anchor, and the fingerprint differs */
    ESPOS_SK_TLS_R_CA_CHANGED,    /* CA anchor, and the presented CA differs */
    ESPOS_SK_TLS_R_SAN_CHANGED,   /* CA matches but the leaf now names other hosts */
    ESPOS_SK_TLS_R_NO_IDENTITY,   /* CA anchored, presented leaf has no SAN to compare */
    ESPOS_SK_TLS_R_CHAIN_FORGED,  /* CA anchored, but a link in the chain is not signed by its parent */
} espos_sk_tls_reason_t;

/* Everything the decision depends on, filled in by the verify callback.
 * Deliberately booleans and not certificates: the parsing lives in sk_tls.c,
 * the judgement lives here, and a host test writes these fields by hand. */
typedef struct {
    bool has_anchor;       /* an anchor of some kind is stored */
    bool anchor_is_leaf;   /* ... and it is a leaf fingerprint (else a CA) */
    bool leaf_matches;     /* presented leaf fingerprint == stored one */
    bool ca_present;       /* the presented chain contains a CA:TRUE certificate */
    bool ca_matches;       /* ... and its fingerprint == the stored CA's */
    bool leaf_has_san;     /* the presented leaf carries at least one dNSName/IP SAN */
    bool san_matches;      /* ... and the normalised set equals the stored one */
    bool chain_forged;     /* a certificate below the top fails its parent's signature */
} espos_sk_tls_facts_t;

typedef struct {
    espos_sk_tls_verdict_t verdict;
    espos_sk_tls_reason_t reason;
    /* What to pin when the verdict is CAPTURE. A chain with a CA and a leaf
     * that names itself is anchored at the CA (renewals survive); anything
     * else falls back to the leaf, which is stricter and always available. */
    espos_sk_tls_anchor_kind_t capture_as;
} espos_sk_tls_decision_t;

/**
 * The whole trust decision, as a table. No allocation, no clock, no I/O:
 * the same call runs on the device inside the mbedTLS verify callback and on
 * the host inside Unity.
 *
 * Not consulted at all in bundle mode — there mbedTLS's own chain validation
 * against the Mozilla roots is the decision, and nothing is pinned.
 */
espos_sk_tls_decision_t espos_sk_tls_decide(const espos_sk_tls_facts_t *f);

/** A human sentence for a reason code; never NULL, never empty. */
const char *espos_sk_tls_reason_str(espos_sk_tls_reason_t r);

/* What loading the configured CA (sk.ca_pem) does to the stored anchor. */
typedef enum {
    ESPOS_SK_TLS_CA_KEEP = 0,     /* the stored anchor already is this CA: keep it, bound names and all */
    ESPOS_SK_TLS_CA_REANCHOR = 1, /* replace it with a fresh, unbound anchor on this CA */
} espos_sk_tls_ca_load_t;

/**
 * Decide what loading the configured CA does, given the stored anchor's kind
 * and CA fingerprint and the configured CA's fingerprint. The PEM is loaded
 * again at every boot, so KEEP is the normal answer: the SAN set and leaf a
 * CA anchor has bound are part of what it stands for, and a fresh anchor
 * would let the next leaf the CA signed, for any name, bind again. Anything
 * but a CA anchor on the same fingerprint is REANCHOR.
 */
espos_sk_tls_ca_load_t espos_sk_tls_ca_load(espos_sk_tls_anchor_kind_t kind, const uint8_t *anchor_ca_fp,
                                            const uint8_t *ca_fp);

/**
 * Normalise a SAN set into the stored form: each name lower-cased, the set
 * sorted and de-duplicated, entries joined with a single ',' and no spaces —
 * so that two certificates naming the same hosts in a different order compare
 * equal with strcmp() and the record stays one flat NVS string.
 *
 * `in` is the raw set as the callback collected it, one name per entry.
 * Returns ESP_OK, or ESP_ERR_INVALID_SIZE when the joined set does not fit in
 * `out_size` — in which case `out` holds the names that did fit, still
 * normalised, and *truncated is set. A truncated set never compares equal to
 * anything (espos_sk_tls_san_equal), so it can only ever narrow what is
 * accepted, never widen it.
 */
esp_err_t espos_sk_tls_san_normalise(const char *const *in, size_t n, char *out, size_t out_size,
                                     bool *truncated);

/* How many names, and how long each, a SAN set can hold while being built.
 * A certificate past either limit gives a cut set, which never matches. */
#define ESPOS_SK_TLS_SAN_ENTRY_MAX 8
#define ESPOS_SK_TLS_SAN_NAME_MAX  64

/* The SAN entry types that name a host (the context-specific tag numbers of
 * RFC 5280 GeneralName); every other type is skipped. */
#define ESPOS_SK_TLS_SAN_TAG_DNS 2
#define ESPOS_SK_TLS_SAN_TAG_IP  7

/* Collects a leaf's SAN entries into a set, one entry at a time, so the
 * caller can walk mbedTLS's sequence without allocating. Zero it to start. */
typedef struct {
    char names[ESPOS_SK_TLS_SAN_ENTRY_MAX][ESPOS_SK_TLS_SAN_NAME_MAX];
    const char *ptrs[ESPOS_SK_TLS_SAN_ENTRY_MAX];
    size_t n;
    bool cut; /* a name did not fit, could not be read whole, or there were too many */
} espos_sk_tls_san_builder_t;

/**
 * Add one raw SAN entry: a dNSName (string) or an iPAddress (4 or 16 bytes).
 * Other tags and other IP lengths are skipped. A dNSName longer than 63 bytes
 * or holding a NUL marks the set cut instead of being stored as its prefix,
 * which would compare equal to a different name sharing that prefix.
 */
void espos_sk_tls_san_add(espos_sk_tls_san_builder_t *b, unsigned tag, const uint8_t *p, size_t len);

/**
 * Write the collected set in the stored form (espos_sk_tls_san_normalise).
 * A cut set is written with a leading '!', so it never compares equal, and
 * *truncated is set.
 */
void espos_sk_tls_san_finish(espos_sk_tls_san_builder_t *b, char *out, size_t out_size, bool *truncated);

/**
 * Compare two normalised SAN sets. False when either is empty or either was
 * truncated (marked by a leading '!'): "we could not see the whole set" must
 * never read as "the sets are the same".
 */
bool espos_sk_tls_san_equal(const char *a, const char *b);

/**
 * Whether a presented SAN set satisfies the stored anchor. An empty stored set
 * means a CA was supplied by configuration before any handshake named the
 * server: this connection binds the set, so anything the pinned CA signed is
 * acceptable here. Once a set is stored, the two must compare equal.
 */
bool espos_sk_tls_san_matches_anchor(const char *stored, const char *presented);

/**
 * The esp-tls attach hook, in the shape esp_http_client's `crt_bundle_attach`
 * and esp_transport_ssl_crt_bundle_attach() both want. In bundle mode it is
 * esp_crt_bundle_attach(); otherwise it installs the pinning verify callback,
 * so a socket that uses it trusts exactly what the SignalK stream trusts.
 *
 * Any component that opens its own TLS connection to the same server should
 * attach this rather than the certificate bundle directly -- otherwise that
 * one socket refuses the self-signed certificate the rest of the device has
 * already accepted. Pair it with skipping the common-name check whenever
 * espos_sk_tls_trust_mode() is not ESPOS_SK_TLS_TRUST_BUNDLE: a pinned
 * anchor identifies the server, and the CN rarely matches an IP address.
 *
 * Returns ESP_ERR_INVALID_STATE in a build without CONFIG_ESPOS_SK_TLS.
 */
esp_err_t espos_sk_tls_attach(void *ssl_conf);

/** The trust mode in force, for call sites deciding about the CN check. */
espos_sk_tls_trust_t espos_sk_tls_trust_mode(void);

#ifdef __cplusplus
}
#endif
