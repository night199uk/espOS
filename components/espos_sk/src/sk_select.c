/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Which discovered server is "ours". Pure, so the host tests can drive it.
 *
 * A self URN is not unique on a real network. A second signalk-server set up
 * with the boat's MMSI -- a test VM, a spare Pi, last season's plotter --
 * advertises exactly the same URN. Matching on self alone let mDNS answer
 * order decide between the two on every discovery round, and each hop cost
 * the device its pairing: a pending request polled on the twin comes back 404
 * and is discarded, so an approval on the real server is never collected, and
 * a token tried on the twin is refused and dropped. The address the request
 * or token was issued at is therefore checked first, and self is only the
 * fallback for a server whose address changed -- which the caller decides,
 * because only it knows how long that address has been silent.
 */
#include <string.h>

#include "espos_sk_priv.h"

void espos_sk_store_anchor(const espos_sk_tok_store_t *st, espos_sk_anchor_t *out)
{
    memset(out, 0, sizeof(*out));
    if (st->token[0]) {
        out->host = st->token_host;
        out->port = st->token_port;
        out->self = st->token_self;
    } else if (st->pending_href[0]) {
        out->host = st->pending_host;
        out->port = st->pending_port;
        out->self = st->pending_self;
    } else {
        out->host = "";
        out->self = "";
    }
}

bool espos_sk_same_addr(const espos_sk_discovered_t *d, const char *host, uint16_t port)
{
    return host && host[0] && d->port == port && strcmp(d->host, host) == 0;
}

bool espos_sk_disc_before(const espos_sk_discovered_t *a, const espos_sk_discovered_t *b,
                          const char *cur_host, uint16_t cur_port)
{
    int c = strcmp(a->self, b->self);
    if (c != 0) {
        return c < 0;
    }
    /* Same self: the one in use stays in use, and otherwise the address
     * decides -- anything but answer order, which changes between rounds. */
    bool a_cur = espos_sk_same_addr(a, cur_host, cur_port);
    bool b_cur = espos_sk_same_addr(b, cur_host, cur_port);
    if (a_cur != b_cur) {
        return a_cur;
    }
    c = strcmp(a->host, b->host);
    if (c != 0) {
        return c < 0;
    }
    return a->port < b->port;
}

bool espos_sk_is_anchor(const espos_sk_discovered_t *d, const espos_sk_anchor_t *anchor)
{
    /* The address, but only while it still answers with our server's self:
     * DHCP can hand the address to a different server, and the one we want
     * is then elsewhere under its self URN. */
    bool self_ok = !anchor->self || !anchor->self[0] || !d->self[0] || strcmp(d->self, anchor->self) == 0;
    return self_ok && espos_sk_same_addr(d, anchor->host, anchor->port);
}

int espos_sk_find_anchored(const espos_sk_discovered_t *list, size_t n, const espos_sk_anchor_t *anchor,
                           const char *cur_host, uint16_t cur_port, bool self_fallback)
{
    for (size_t i = 0; i < n; i++) {
        if (espos_sk_is_anchor(&list[i], anchor)) {
            return (int)i;
        }
    }
    /* No address recorded (a token from older firmware): self is all there
     * is, as it always was. */
    if (!anchor->self || !anchor->self[0] || (anchor->host[0] && !self_fallback)) {
        return -1;
    }
    int idx = -1;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i].self, anchor->self) != 0) {
            continue;
        }
        if (idx < 0 || espos_sk_disc_before(&list[i], &list[idx], cur_host, cur_port)) {
            idx = (int)i;
        }
    }
    return idx;
}

void espos_sk_disc_add(espos_sk_discovered_t *out, size_t *n, size_t max, const espos_sk_discovered_t *d,
                       const espos_sk_keep_t *keep)
{
    for (size_t i = 0; i < *n; i++) {
        if (espos_sk_same_addr(&out[i], d->host, d->port)) {
            out[i] = *d;
            return;
        }
    }
    if (*n < max) {
        out[(*n)++] = *d;
        return;
    }
    if (max == 0 || !keep || !keep->anchor) {
        return;
    }
    /* One rule for both: whatever find_anchored prefers between the entry it
     * picks now and the newcomer stays. */
    int cur = espos_sk_find_anchored(out, *n, keep->anchor, keep->cur_host, keep->cur_port, keep->self_fallback);
    espos_sk_discovered_t pair[2];
    size_t np = 0;
    if (cur >= 0) {
        pair[np++] = out[cur];
    }
    pair[np++] = *d;
    int pick = espos_sk_find_anchored(pair, np, keep->anchor, keep->cur_host, keep->cur_port, keep->self_fallback);
    if (pick != (int)np - 1) {
        return;
    }
    out[cur >= 0 ? (size_t)cur : max - 1] = *d;
}

static void carry_avoid(const espos_sk_merge_in_t *in, const espos_sk_discovered_t *d, uint32_t *avoid)
{
    for (size_t k = 0; k < in->table_n; k++) {
        if (espos_sk_same_addr(&in->table[k], d->host, d->port)) {
            *avoid = in->table_avoid[k];
        }
    }
}

size_t espos_sk_merge_servers(const espos_sk_merge_in_t *in, espos_sk_discovered_t *out, uint32_t *out_avoid,
                              size_t cap)
{
    memset(out_avoid, 0, cap * sizeof(*out_avoid));
    if (cap == 0) {
        return 0;
    }
    size_t m = 0;
    /* The server our token belongs to gets the first slot.
     *
     * Fresh results used to fill the array first, so on a network
     * advertising cap or more the carry-over loop below -- the whole defence
     * against mDNS being lossy -- could not run at all. One missed answer
     * then evicted the anchored server, the anchor lookup in select_server()
     * found nothing, and a perfectly reachable server was dropped with "no
     * server". Reserving its slot first means a full list can never cost us
     * the one entry that matters.
     *
     * Reserved by address, not by self URN: a second server advertising the
     * same self would otherwise take the slot whenever mDNS answered for it
     * first. And only while it is as fresh as any carried-over entry: a
     * server that moved must age out at its old address, or the stale entry
     * would keep winning select_server()'s address match and the device would
     * never follow it. */
    int keep = espos_sk_find_anchored(in->table, in->table_n, in->anchor, in->cur_host, in->cur_port,
                                      in->self_fallback);
    bool keep_reserved = false;
    if (keep >= 0) {
        /* Prefer this round's fresher copy if mDNS did answer for it. */
        for (size_t k = 0; k < in->found_n; k++) {
            if (espos_sk_same_addr(&in->found[k], in->table[keep].host, in->table[keep].port)) {
                out[m] = in->found[k];
                out[m].seen_ms = in->now_ms;
                keep_reserved = true;
                break;
            }
        }
        if (!keep_reserved && (int32_t)(in->now_ms - in->table[keep].seen_ms) < (int32_t)in->carry_ms) {
            out[m] = in->table[keep];
            keep_reserved = true;
        }
        if (keep_reserved) {
            out_avoid[m] = in->table_avoid[keep];
        }
    }
    if (!keep_reserved) {
        /* Not in the table (a fresh boot with a restored token) but answering
         * now: reserve it from this round, or a small table could fill up
         * before reaching it. */
        int k = espos_sk_find_anchored(in->found, in->found_n, in->anchor, in->cur_host, in->cur_port,
                                       in->self_fallback);
        if (k >= 0) {
            out[m] = in->found[k];
            out[m].seen_ms = in->now_ms;
            carry_avoid(in, &in->found[k], &out_avoid[m]);
            keep_reserved = true;
        }
    }
    if (keep_reserved) {
        m++;
    }
    for (size_t i = 0; i < in->found_n && m < cap; i++) {
        /* Skip only what the reservation above actually took. Testing m > 0
         * instead would drop the anchored server whenever the reservation did
         * NOT run. */
        if (keep_reserved && espos_sk_same_addr(&in->found[i], out[0].host, out[0].port)) {
            continue;
        }
        out[m] = in->found[i];
        out[m].seen_ms = in->now_ms;
        carry_avoid(in, &in->found[i], &out_avoid[m]);
        m++;
    }
    for (size_t i = 0; i < in->table_n && m < cap; i++) {
        bool dup = false;
        for (size_t k = 0; k < m; k++) { /* against what we kept, including the reserved anchor */
            if (espos_sk_same_addr(&out[k], in->table[i].host, in->table[i].port)) {
                dup = true;
                break;
            }
        }
        if (!dup && (int32_t)(in->now_ms - in->table[i].seen_ms) < (int32_t)in->carry_ms) {
            out_avoid[m] = in->table_avoid[i];
            out[m++] = in->table[i];
        }
    }
    return m;
}
