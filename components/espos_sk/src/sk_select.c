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
