/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The trust decision and the SAN normalisation (see espos_sk_tls_policy.h).
 * Pure C: no mbedTLS, no IDF, no allocation — sk_tls.c collects the facts,
 * this decides, and a Unity host test drives it directly.
 */
#include <stdio.h>
#include <string.h>

#include "espos_sk_tls_policy.h"

/* A truncated SAN set is stored with this prefix so the flag survives a
 * round trip through NVS as part of the one string. It is not a legal
 * character in a dNSName, so it can never collide with a real name. */
#define SAN_TRUNC_MARK '!'

espos_sk_tls_decision_t espos_sk_tls_decide(const espos_sk_tls_facts_t *f)
{
    espos_sk_tls_decision_t d = {
        .verdict = ESPOS_SK_TLS_REJECT,
        .reason = ESPOS_SK_TLS_R_LEAF_CHANGED,
        .capture_as = ESPOS_SK_TLS_ANCHOR_NONE,
    };
    if (!f) {
        return d;
    }
    if (!f->has_anchor) {
        /* First use. Anchor at the CA when there is one AND the leaf names
         * itself, because only then can a renewal be recognised: the CA says
         * who signed it and the SAN set says for whom. A CA with an anonymous
         * leaf would vouch for every future name it signs, which is a wider
         * trust than the operator asked for, so that case pins the leaf. */
        d.verdict = ESPOS_SK_TLS_CAPTURE;
        d.reason = ESPOS_SK_TLS_R_FIRST_USE;
        /* A CA that did not sign the chain vouches for nothing, so a forged
         * link pins the leaf, as if there were no CA at all. */
        d.capture_as = (f->ca_present && f->leaf_has_san && !f->chain_forged) ? ESPOS_SK_TLS_ANCHOR_CA
                                                                              : ESPOS_SK_TLS_ANCHOR_LEAF;
        return d;
    }
    if (f->anchor_is_leaf) {
        /* Nothing to negotiate: this exact certificate, or an operator has to
         * say otherwise (DELETE /api/v1/sk/tls). */
        if (f->leaf_matches) {
            d.verdict = ESPOS_SK_TLS_ACCEPT;
            d.reason = ESPOS_SK_TLS_R_OK;
        } else {
            d.reason = ESPOS_SK_TLS_R_LEAF_CHANGED;
        }
        return d;
    }
    /* CA anchor. The leaf may be brand new — that is the point — but it must
     * be signed by the pinned CA and name the same hosts. */
    if (!f->ca_present || !f->ca_matches) {
        d.reason = ESPOS_SK_TLS_R_CA_CHANGED;
        return d;
    }
    if (f->chain_forged) {
        /* The pinned CA's certificate is public: anyone can hang it over a
         * leaf of their own. Said apart from CA_CHANGED, because that one
         * reads like a renewal and invites "Trust the new certificate". */
        d.reason = ESPOS_SK_TLS_R_CHAIN_FORGED;
        return d;
    }
    if (!f->leaf_has_san) {
        /* The CA is right but this leaf says nothing about who it is, so
         * there is no way to tell "our server, renewed" from "some other host
         * the same CA signed". Refuse rather than widen the anchor. */
        d.reason = ESPOS_SK_TLS_R_NO_IDENTITY;
        return d;
    }
    if (!f->san_matches) {
        d.reason = ESPOS_SK_TLS_R_SAN_CHANGED;
        return d;
    }
    d.verdict = ESPOS_SK_TLS_ACCEPT;
    d.reason = ESPOS_SK_TLS_R_OK;
    return d;
}

espos_sk_tls_ca_load_t espos_sk_tls_ca_load(espos_sk_tls_anchor_kind_t kind, const uint8_t *anchor_ca_fp,
                                            const uint8_t *ca_fp)
{
    if (kind == ESPOS_SK_TLS_ANCHOR_CA && anchor_ca_fp && ca_fp &&
        memcmp(anchor_ca_fp, ca_fp, ESPOS_SK_TLS_FP_LEN) == 0) {
        return ESPOS_SK_TLS_CA_KEEP;
    }
    return ESPOS_SK_TLS_CA_REANCHOR;
}

const char *espos_sk_tls_reason_str(espos_sk_tls_reason_t r)
{
    switch (r) {
    case ESPOS_SK_TLS_R_OK: return "certificate matches the pinned one";
    case ESPOS_SK_TLS_R_FIRST_USE: return "first use: pinning this certificate";
    case ESPOS_SK_TLS_R_LEAF_CHANGED: return "the server certificate changed";
    case ESPOS_SK_TLS_R_CA_CHANGED: return "the server certificate was issued by a different CA";
    case ESPOS_SK_TLS_R_SAN_CHANGED: return "the server certificate now names different hosts";
    case ESPOS_SK_TLS_R_NO_IDENTITY: return "the server certificate carries no host name to check";
    case ESPOS_SK_TLS_R_CHAIN_FORGED: return "the server certificate is not signed by the pinned CA";
    }
    return "certificate rejected";
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* strcasecmp is in strings.h, which the linux host has and a bare-metal
 * toolchain need not; the sets are short, so compare them here. */
static int cmp_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = lower(*a), y = lower(*b);
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return *a ? 1 : (*b ? -1 : 0);
}

esp_err_t espos_sk_tls_san_normalise(const char *const *in, size_t n, char *out, size_t out_size,
                                     bool *truncated)
{
    if (truncated) {
        *truncated = false;
    }
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!in || n == 0) {
        return ESP_OK;
    }
    /* Selection sort over the input by index: n is a certificate's SAN count
     * (single digits in practice, and bounded by the caller's array), and this
     * needs neither a scratch array nor a comparator closure. Each pass picks
     * the smallest name not yet emitted, which also collapses duplicates
     * because an equal name compares 0 against the one just written. */
    size_t written = 0;
    const char *prev = NULL;
    for (size_t pass = 0; pass < n; pass++) {
        const char *best = NULL;
        for (size_t i = 0; i < n; i++) {
            const char *cand = in[i];
            if (!cand || !cand[0]) {
                continue;
            }
            if (prev && cmp_ci(cand, prev) <= 0) {
                continue; /* already emitted (or a duplicate of it) */
            }
            if (!best || cmp_ci(cand, best) < 0) {
                best = cand;
            }
        }
        if (!best) {
            break;
        }
        size_t len = strlen(best);
        size_t need = len + (written ? 1 : 0);
        if (written + need + 1 > out_size) {
            /* Does not fit. Mark the set truncated rather than silently
             * storing a subset that would then match a certificate naming
             * fewer hosts than the real one does. */
            if (truncated) {
                *truncated = true;
            }
            return ESP_ERR_INVALID_SIZE;
        }
        if (written) {
            out[written++] = ',';
        }
        for (size_t k = 0; k < len; k++) {
            out[written++] = lower(best[k]);
        }
        out[written] = '\0';
        prev = best;
    }
    return ESP_OK;
}

bool espos_sk_tls_san_equal(const char *a, const char *b)
{
    if (!a || !b || !a[0] || !b[0]) {
        return false;
    }
    if (a[0] == SAN_TRUNC_MARK || b[0] == SAN_TRUNC_MARK) {
        return false; /* an incomplete set is never proof of sameness */
    }
    return strcmp(a, b) == 0;
}

void espos_sk_tls_san_add(espos_sk_tls_san_builder_t *b, unsigned tag, const uint8_t *p, size_t len)
{
    if (!b || !p || len == 0) {
        return;
    }
    bool is_name = tag == ESPOS_SK_TLS_SAN_TAG_DNS || (tag == ESPOS_SK_TLS_SAN_TAG_IP && (len == 4 || len == 16));
    if (!is_name || b->cut) {
        return;
    }
    if (b->n >= ESPOS_SK_TLS_SAN_ENTRY_MAX) {
        b->cut = true;
        return;
    }
    char *name = b->names[b->n];
    if (tag == ESPOS_SK_TLS_SAN_TAG_DNS) {
        if (len > ESPOS_SK_TLS_SAN_NAME_MAX - 1 || memchr(p, '\0', len)) {
            b->cut = true;
            return;
        }
        memcpy(name, p, len);
        name[len] = '\0';
    } else if (len == 4) {
        snprintf(name, ESPOS_SK_TLS_SAN_NAME_MAX, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    } else {
        char *w = name;
        for (int i = 0; i < 16; i += 2) {
            w += snprintf(w, (size_t)(name + ESPOS_SK_TLS_SAN_NAME_MAX - w), "%s%02x%02x", i ? ":" : "", p[i],
                          p[i + 1]);
        }
    }
    b->ptrs[b->n] = name;
    b->n++;
}

void espos_sk_tls_san_finish(espos_sk_tls_san_builder_t *b, char *out, size_t out_size, bool *truncated)
{
    if (truncated) {
        *truncated = false;
    }
    if (!b || !out || out_size == 0) {
        return;
    }
    bool cut = false;
    if (espos_sk_tls_san_normalise(b->ptrs, b->n, out, out_size, &cut) != ESP_OK || cut || b->cut) {
        /* A set we could not record whole is marked, and a marked set never
         * compares equal (espos_sk_tls_san_equal), so it can only ever refuse
         * -- never accept a certificate on the strength of the part we saw. */
        if (out_size > 1) {
            memmove(out + 1, out, out_size - 2);
            out[0] = SAN_TRUNC_MARK;
            out[out_size - 1] = '\0';
        }
        if (truncated) {
            *truncated = true;
        }
    }
}

bool espos_sk_tls_san_matches_anchor(const char *stored, const char *presented)
{
    /* A CA supplied by configuration is anchored before any handshake has
     * named the server, so its SAN set starts empty. The first leaf this CA
     * signs binds the set at commit; once bound, a different set is refused. */
    if (!stored || !stored[0]) {
        return true;
    }
    return espos_sk_tls_san_equal(stored, presented);
}
