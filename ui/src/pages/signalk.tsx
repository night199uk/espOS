// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
import { useEffect, useState } from "preact/hooks";
import { get, put, post, del, useStore, skStore, skServersStore, skWsStore, skTlsStore, fmtDuration, fmtBytes, errText, type SkServersDoc, type SkTls, type ConfigDoc, type PutResult } from "../api";
import { Badge, Row, Msg } from "../app";

const TOKEN_HELP: Record<string, string> = {
  no_server: "No server selected yet — waiting for discovery or a manual host.",
  requesting: "Sending an access request to the server…",
  pending: "Waiting for approval. Open the SignalK admin UI → Security → Access Requests and approve this device.",
  verifying: "Checking the token against the server…",
  approved: "Access granted; the token is stored on the device.",
  denied: "The request was denied. Fix it on the server, then request again.",
  open: "The server has security disabled: no token needed.",
  error: "The last request failed; the device retries with backoff.",
  cert_error: "The server's certificate is not the one this device trusts. The access token is kept — it is the connection that is refused, not the credential. If the certificate was legitimately replaced, compare the fingerprints below and trust the new one.",
};

/** Short enough to read at a glance, long enough to tell two apart. */
const shortFp = (fp: string) => (fp ? fp.replace(/(.{4})/g, "$1 ").trim().slice(0, 24) + "…" : "–");

export function SignalKPage() {
  const sk = useStore(skStore);
  const servers = useStore(skServersStore);
  const ws = useStore(skWsStore) ?? sk?.ws;
  const [cfg, setCfg] = useState<Record<string, unknown> | null>(null);
  const [msg, setMsg] = useState("");
  const [ok, setOk] = useState("");
  const [token, setToken] = useState("");
  const [manual, setManual] = useState({ host: "", port: 80 });
  const [busy, setBusy] = useState("");

  const tls = useStore(skTlsStore);
  // A build without CONFIG_ESPOS_SK_TLS answers 404 here and has no trust to
  // show; the section simply does not appear.
  const loadTls = () => get<SkTls>("/sk/tls").then((d) => skTlsStore.set(d), () => undefined);

  const load = () => get<ConfigDoc>("/config?ns=sk").then((d) => {
    const c = d["sk"] ?? {};
    setCfg(c);
    setManual({ host: (c["server_host"] as string) || "", port: (c["server_port"] as number) || 80 });
  }, (e: unknown) => setMsg(errText(e)));
  useEffect(() => {
    void load();
    void loadTls();
    void get<SkServersDoc>("/sk/servers").then((d) => skServersStore.set(d), () => undefined);
  }, []);

  async function act(name: string, fn: () => Promise<unknown>, done: string) {
    setBusy(name); setMsg(""); setOk("");
    try { await fn(); setOk(done); } catch (e) { setMsg(errText(e)); } finally { setBusy(""); }
  }
  const save = (patch: Record<string, unknown>, done: string) => act("save", async () => { const r = await put<PutResult>("/config", { sk: patch }); if (!r.changed.length) done = "No change."; await load(); }, done);

  const st = sk?.token.state ?? "";
  const kind = st === "approved" || st === "open" ? "ok"
    : st === "denied" || st === "error" || st === "cert_error" ? "bad" : "warn";
  const selfSel = (cfg?.["server_self"] as string) || "";
  const hostSel = (cfg?.["server_host"] as string) || "";

  return (
    <>
      <h1>SignalK</h1>
      <Msg text={msg} />
      <Msg text={ok} kind="ok" />
      <div class="grid">
        <section class="card">
          <h2>Access <Badge kind={kind}>{st || "…"}</Badge></h2>
          {sk && (
            <>
              <p class="small">{TOKEN_HELP[st] ?? ""}</p>
              <Row k="Server">{sk.server.source === "none" ? <span class="muted">none</span> : <>{sk.server.name || sk.server.host} <span class="muted">· {sk.server.scheme ?? "http"}://{sk.server.host}:{sk.server.port} · {sk.server.source}{sk.server.swvers ? ` · ${sk.server.swname} ${sk.server.swvers}` : ""}</span></>}</Row>
              <Row k="Device">{sk.description} <span class="muted">· {sk.permissions}</span></Row>
              <Row k="Client id"><span class="mono small">{sk.client_id}</span></Row>
              {st === "pending" && <Row k="Pending for">{fmtDuration(sk.token.pending_s)}</Row>}
              {st === "approved" && <Row k="Approved">{fmtDuration(sk.token.approved_s)} ago{sk.token.next_action_s !== undefined ? <span class="muted"> · next check in {sk.token.next_action_s}s</span> : null}</Row>}
              {sk.token.last_error && <Row k="Last error"><span class="mono">{sk.token.last_error} (HTTP {sk.token.last_http_status})</span></Row>}
              <Row k="Counters">{sk.token.counts.requests} requests · {sk.token.counts.approved} approved · {sk.token.counts.denied} denied · {sk.token.counts.unauthorized} unauthorized</Row>
              <div class="row" style="margin-top:.5rem">
                <button disabled={!!busy || sk.token.busy || sk.server.source === "none"} onClick={() => act("request", () => post("/sk/request"), "Access requested.")}>Request access</button>
                <button class="danger" disabled={!!busy || !sk.token.has_token} onClick={() => { if (confirm("Forget the stored token? The device will request access again.")) void act("forget", () => post("/sk/forget"), "Token forgotten."); }}>Forget token</button>
              </div>
              <form class="row" onSubmit={(e) => { e.preventDefault(); if (token) void act("token", () => post("/sk/token", { token }), "Token submitted; verifying…").then(() => setToken("")); }}>
                <input type="password" placeholder="Paste a token instead" style="min-width:16rem" value={token} onInput={(e) => setToken((e.target as HTMLInputElement).value)} />
                <button type="submit" disabled={!token || !!busy}>Use token</button>
              </form>
            </>
          )}
        </section>

        {tls && (
          <section class="card">
            <h2>Connection security <Badge kind={st === "cert_error" ? "bad" : tls.pinned ? "ok" : "muted"}>
              {st === "cert_error" ? "not trusted" : tls.pinned ? "pinned" : "nothing pinned"}
            </Badge></h2>
            <Row k="Scheme">
              <select value={(cfg?.["scheme"] as string) || "auto"}
                      onChange={(e) => void save({ scheme: (e.target as HTMLSelectElement).value }, "Scheme saved.")}>
                <option value="auto">auto — ask the server</option>
                <option value="http">http — never TLS</option>
                <option value="https">https — always TLS</option>
              </select>
              <span class="muted small"> Currently {sk?.server.scheme ?? "–"}.</span>
            </Row>
            <Row k="Trust">
              <select value={(cfg?.["tls_trust"] as string) || "tofu"}
                      onChange={(e) => void save({ tls_trust: (e.target as HTMLSelectElement).value }, "Trust setting saved.")}>
                <option value="tofu">first use — pin what this server shows</option>
                <option value="ca">own CA — hold it to the certificate below</option>
                <option value="bundle">public roots only</option>
              </select>
            </Row>
            {tls.pinned
              ? <>
                  <Row k="Pinned">{tls.pinned.kind === "ca" ? "issuing CA" : "this certificate"}
                    {tls.pinned.cn ? <span class="muted"> · {tls.pinned.cn}</span> : null}</Row>
                  <Row k="Fingerprint"><span class="mono small">{shortFp(tls.pinned.fingerprint)}</span></Row>
                  {tls.pinned.san && <Row k="Names"><span class="mono small">{tls.pinned.san}</span></Row>}
                </>
              : <p class="muted small">Nothing is pinned yet. The first connection that works records what
                  the server presented, and every later one has to match it.</p>}
            {st === "cert_error" && (
              <>
                <Msg text={sk?.token.last_error ?? ""} />
                {tls.presented && <>
                  <Row k="Now showing">{tls.presented.cn || <span class="muted">no name</span>}</Row>
                  <Row k="Its fingerprint"><span class="mono small">{shortFp(tls.presented.fingerprint)}</span></Row>
                </>}
                <p class="muted small">
                  If you replaced the server's certificate yourself, this is expected and the button below
                  accepts the new one. If you did not, something else is answering as your server and you
                  should find out what before trusting it.
                </p>
                <div class="row">
                  <button class="danger" disabled={!!busy}
                          onClick={() => { if (confirm("Trust the certificate the server is showing now? Do this only if you know why it changed.")) void act("trust", () => del("/sk/tls").then(loadTls), "Trusting the new certificate."); }}>
                    Trust the new certificate
                  </button>
                </div>
              </>
            )}
            {st !== "cert_error" && tls.pinned && (
              <div class="row" style="margin-top:.5rem">
                <button disabled={!!busy}
                        onClick={() => { if (confirm("Forget the pinned certificate? The next connection pins whatever the server presents.")) void act("trust", () => del("/sk/tls").then(loadTls), "Pinned certificate forgotten."); }}>
                  Forget pinned certificate
                </button>
              </div>
            )}
          </section>
        )}

        <section class="card">
          <h2>Delta stream {ws && <Badge kind={ws.connected ? "ok" : ws.enabled ? "warn" : "muted"}>{ws.connected ? "connected" : ws.enabled ? "offline" : "disabled"}</Badge>}</h2>
          {ws && (
            <>
              {ws.connected && <Row k="Up for">{fmtDuration(ws.connected_s)}</Row>}
              {!ws.connected && ws.next_retry_s !== undefined && ws.next_retry_s > 0 && <Row k="Retry in">{ws.next_retry_s} s</Row>}
              <Row k="Sent">{ws.sent} messages <span class="muted">· {ws.reconnects} connects · {ws.send_errors} send errors</span></Row>
              <Row k="Buffered">{ws.buffered} messages <span class="muted">({fmtBytes(ws.buffered_bytes)}) · {ws.pending} pending · {ws.dropped} dropped</span></Row>
              <Row k="Metadata">{ws.meta.reconciled}/{ws.meta.declared} reconciled</Row>
              {ws.in && <Row k="Inbound">{ws.in.subs} subscription{ws.in.subs === 1 ? "" : "s"} <span class="muted">· {ws.in.received} values received · {ws.in.frames} frames</span></Row>}
              {ws.put && (ws.put.ok + ws.put.failed + ws.put.pending > 0) && <Row k="PUT requests">{ws.put.ok} ok <span class="muted">· {ws.put.failed} failed · {ws.put.pending} pending</span></Row>}
              {ws.last_error && <Row k="Last error"><span class="mono">{ws.last_error}</span></Row>}
              <div class="row" style="margin-top:.5rem">
                <label><input type="checkbox" checked={!!cfg?.["ws_enabled"]} onChange={(e) => void save({ ws_enabled: (e.target as HTMLInputElement).checked }, "Stream setting saved.")} /> stream deltas</label>
              </div>
            </>
          )}
        </section>

        <section class="card wide">
          <h2>Servers <button disabled={busy === "discover"} onClick={() => act("discover", () => post("/sk/discover"), "Discovery started.")}>Discover</button></h2>
          <p class="muted small">
            {hostSel ? <>Manual server <b>{hostSel}:{String(cfg?.["server_port"])}</b> is pinned. </> : selfSel ? <>Pinned to server <span class="mono">{selfSel}</span>. </> : "Automatic: the first master server found is used and kept. "}
            {servers?.last_s !== null && servers?.last_s !== undefined && <>Last discovery {servers.last_s}s ago.</>}
          </p>
          {!hostSel && !selfSel && (
            <div class="row">
              <label>
                <input type="checkbox" checked={!!cfg?.["server_pin"]}
                       onChange={(e) => void save({ server_pin: (e.target as HTMLInputElement).checked }, "Server selection saved.")} />
                {" "}stay on the chosen server
              </label>
              <span class="muted small">
                Keep using the server in use even if discovery stops seeing it, instead of
                moving to another one. Useful where several vessels' servers are visible.
              </span>
            </div>
          )}
          <table>
            <thead><tr><th>Name</th><th>Address</th><th>Version</th><th></th></tr></thead>
            <tbody>
              {(servers?.servers ?? []).map((s, _i, all) => {
                /* Two servers with one self URN (a test VM with the boat's
                   MMSI): pinning the self cannot tell them apart, so pin the
                   address instead. */
                const twin = all.some((o) => o !== s && o.self === s.self);
                const hostPinned = hostSel === s.host && (Number(cfg?.["server_port"]) || 80) === s.port;
                return (
                <tr key={s.self + s.host + ":" + String(s.port)} class={s.selected ? "sel" : ""}>
                  <td>{s.name}{s.roles?.includes("master") ? "" : <span class="muted small"> ({s.roles})</span>}<div class="mono small muted">{s.self}</div></td>
                  <td>{s.scheme === "https" ? <b>https</b> : "http"}://{s.host}:{s.port}</td>
                  <td>{s.swname} {s.swvers}</td>
                  <td>{hostPinned ? <button onClick={() => void save({ server_host: null }, "Back to automatic selection.")}>Unpin</button>
                    : s.selected && selfSel === s.self ? <button onClick={() => void save({ server_self: null }, "Back to automatic selection.")}>Unpin</button>
                    : twin ? <button onClick={() => void save({ server_host: s.host, server_port: s.port, server_self: null }, `Using ${s.name} at ${s.host}.`)}>Use</button>
                    : <button onClick={() => void save({ server_self: s.self, server_host: null }, `Pinned ${s.name}.`)}>Use</button>}</td>
                </tr>
                );
              })}
              {!(servers?.servers.length) && <tr><td colSpan={4} class="muted">No servers discovered yet (mDNS <span class="mono">_signalk-http._tcp</span>).</td></tr>}
            </tbody>
          </table>
          <h3>Manual server</h3>
          <form class="row" onSubmit={(e) => { e.preventDefault(); void save({ server_host: manual.host || null, server_port: manual.port, server_self: null }, manual.host ? `Using ${manual.host}:${manual.port}.` : "Manual server cleared."); }}>
            <input type="text" placeholder="host or IP" value={manual.host} onInput={(e) => setManual({ ...manual, host: (e.target as HTMLInputElement).value })} />
            <input type="number" min={1} max={65535} style="width:6rem" value={manual.port} onInput={(e) => setManual({ ...manual, port: Number((e.target as HTMLInputElement).value) })} />
            <button type="submit">Set</button>
            {hostSel && <button type="button" onClick={() => void save({ server_host: null }, "Manual server cleared.")}>Clear</button>}
          </form>
          <p class="muted small">For networks without mDNS. Batching, buffer size and health interval live under Config → SignalK.</p>
        </section>
      </div>
    </>
  );
}
