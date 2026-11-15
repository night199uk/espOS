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
