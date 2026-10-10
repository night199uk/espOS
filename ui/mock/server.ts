// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espOS API mock for UI development: implements docs/rest-api.md well enough to
// exercise every page without a device — config with schema validation of
// the basics, a simulated WiFi state machine, SignalK discovery/token flow,
// a log ring, SSE, and the REST authentication (Bearer or the espos_sid
// cookie, once httpd.api_key is set). Zero dependencies (node:http only).
//
//   node mock/server.ts [port]       (vite dev starts it automatically)
//
// The schema is regenerated from the real descriptors when python3 is
// available (components/espos_config/tools/espos_gen_config.py); otherwise mock/schema.json is used.
import http, { type IncomingMessage, type ServerResponse } from "node:http";
import { spawnSync } from "node:child_process";
import { randomUUID } from "node:crypto";
import { readFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

// Config values are scalars: every schema property has a scalar type, and
// validate() refuses anything else before it is stored.
type Scalar = string | number | boolean | null;
type ConfigDoc = Record<string, Record<string, Scalar>>;
type Json = Scalar | Json[] | { [key: string]: Json };
type JsonObject = { [key: string]: Json };

interface SchemaProperty {
  type?: string;
  default?: Scalar;
  minimum?: number;
  maximum?: number;
  maxLength?: number;
  enum?: Scalar[];
  pattern?: string;
  "x-espos-secret"?: boolean;
  "x-espos-restartRequired"?: boolean;
}
interface Schema {
  properties: Record<string, { properties: Record<string, SchemaProperty> }>;
}

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..", "..");

function loadSchema(): Schema {
  try {
    const gen = path.join(root, "components", "espos_config", "tools", "espos_gen_config.py");
    const descs = ["main/config/app.json", "components/espos_httpd/config/httpd.json",
      "components/espos_wifi/config/wifi.json", "components/espos_sk/config/sk.json",
      "components/espos_ota/config/ota.json"].map((p) => path.join(root, p));
    const out = mkdtempSync(path.join(tmpdir(), "espos-mock-"));
    const r = spawnSync("python3", [gen, "--schema-out", path.join(out, "s.json"), "--c-out", path.join(out, "c.c"),
      "--h-out", path.join(out, "h.h"), ...descs], { stdio: "ignore" });
    if (r.status === 0) return JSON.parse(readFileSync(path.join(out, "s.json"), "utf8")) as Schema;
  } catch { /* fall through */ }
  return JSON.parse(readFileSync(path.join(here, "schema.json"), "utf8")) as Schema;
}

export function startMock(port = 8484): http.Server {
  const schema = loadSchema();
  const etag = "mock" + Math.abs(hash(JSON.stringify(schema))).toString(16).slice(0, 12);

  // ---- state
  const defaults: ConfigDoc = {};
  for (const [ns, o] of Object.entries(schema.properties)) {
    const d: Record<string, Scalar> = (defaults[ns] = {});
    for (const [k, p] of Object.entries(o.properties)) d[k] = p.default ?? null;
  }
  const stored: ConfigDoc = { wifi: { ssid0: "Marina-Guest", psk0: "hunter22" } };
  const effective = (): ConfigDoc => {
    const out: ConfigDoc = {};
    for (const ns of Object.keys(defaults)) out[ns] = { ...defaults[ns], ...(stored[ns] ?? {}) };
    return out;
  };
  // Every namespace the mock reads is in the schema, so this never falls back.
  const cfg = (ns: string): Record<string, Scalar> => effective()[ns] ?? {};
  const isSecret = (ns: string, k: string) => !!schema.properties[ns]?.properties[k]?.["x-espos-secret"];
  const redacted = (ns: string | null): ConfigDoc => {
    const doc = effective();
    const pick: ConfigDoc = ns ? { [ns]: doc[ns] ?? {} } : doc;
    for (const [n, o] of Object.entries(pick)) for (const k of Object.keys(o)) if (isSecret(n, k)) o[k] = o[k] ? "********" : "";
    return pick;
  };

  const boot = Date.now();
  const logs: { seq: number; line: string }[] = [];
  let logSeq = 1;
  const clients = new Set<ServerResponse>();
  const log = (lvl: string, tag: string, msg: string) => {
    const line = `${lvl} (${Date.now() - boot}) ${tag}: ${msg}`;
    logs.push({ seq: logSeq++, line });
    while (logs.length > 400) logs.shift();
    console.log("  [mock] " + line);
  };
  const emit = (event: string, data: unknown) => {
    const chunk = `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
    for (const c of clients) c.write(chunk);
  };
  let logsDirty = false;
  const origLog = log;
  const logAndMark = (l: string, t: string, m: string) => { origLog(l, t, m); logsDirty = true; };
  setInterval(() => { if (logsDirty) { logsDirty = false; emit("logs", { next: logSeq }); } }, 500);

  interface WifiStatus {
    state: string; sta_enabled: boolean; hostname: string; reason: { code: number; text: string };
    connect_count: number; disconnect_count: number; attempt: number;
    portal: { active: boolean; ssid: string; force: string; ip: string; clients: number };
    ssid?: Scalar | undefined; network_index?: number; backoff_ms?: number;
    ip?: string; gateway?: string; netmask?: string; rssi?: number; channel?: number; bssid?: string;
  }
  const wifi: WifiStatus = {
    state: "unconfigured", sta_enabled: true, hostname: "espos-1a2b", reason: { code: 0, text: "" },
    connect_count: 0, disconnect_count: 0, attempt: 0,
    portal: { active: true, ssid: "espOS-1a2b", force: "auto", ip: "192.168.4.1", clients: 0 },
  };
  let wifiTimer: ReturnType<typeof setTimeout> | undefined;
  const wifiEmit = () => emit("wifi", wifiStatus());
  const wifiStatus = () => ({ ...wifi });
  function wifiEval() {
    clearTimeout(wifiTimer);
    const c = cfg("wifi");
    const nets = [0, 1, 2, 3].map((i) => c[`ssid${i}`]).filter(Boolean);
    if (!c.sta_enabled) {
      Object.assign(wifi, { state: "disabled", reason: { code: 1005, text: "station disabled by configuration" } });
      wifi.portal.active = true; delete wifi.ip; return wifiEmit();
    }
    if (!nets.length) {
      Object.assign(wifi, { state: "unconfigured", reason: { code: 0, text: "" } });
      wifi.portal.active = true; delete wifi.ip; return wifiEmit();
    }
    const idx = nets.findIndex((s) => scanResults.some((r) => r.ssid === s));
    Object.assign(wifi, { state: "connecting", ssid: nets[Math.max(idx, 0)], network_index: Math.max(idx, 0), attempt: wifi.attempt + 1 });
    delete wifi.ip; wifiEmit();
    logAndMark("I", "espos_wifi", `connecting to ${wifi.ssid}`);
    wifiTimer = setTimeout(() => {
      if (idx < 0) {
        Object.assign(wifi, { state: "backoff", reason: { code: 201, text: "no AP with that SSID found (NO_AP_FOUND)" }, backoff_ms: 8000, disconnect_count: wifi.disconnect_count + 1 });
        wifi.portal.active = true; wifiEmit();
        logAndMark("W", "espos_wifi", `disconnected: NO_AP_FOUND (201), retry in 8 s`);
        wifiTimer = setTimeout(wifiEval, 8000);
        return;
      }
      Object.assign(wifi, { state: "obtaining_ip" }); wifiEmit();
      wifiTimer = setTimeout(() => {
        const r = scanResults.find((x) => x.ssid === wifi.ssid)!;
        Object.assign(wifi, { state: "connected", ip: "192.168.1.42", gateway: "192.168.1.1", netmask: "255.255.255.0",
          rssi: r.rssi, channel: r.channel, bssid: r.bssid, connect_count: wifi.connect_count + 1, reason: { code: 0, text: "" } });
        delete wifi.backoff_ms; wifi.portal.active = false; wifiEmit();
        logAndMark("I", "espos_wifi", `connected: ${wifi.ssid} ip=${wifi.ip} rssi=${wifi.rssi}`);
        skDiscover();
      }, 1200);
    }, 1500);
  }
  const scanResults = [
    { ssid: "Marina-Guest", bssid: "de:ad:be:ef:00:02", rssi: -71, channel: 11, auth: "wpa2" },
    { ssid: "Boat", bssid: "de:ad:be:ef:00:01", rssi: -48, channel: 6, auth: "wpa2/wpa3" },
    { ssid: "Harbour Cafe", bssid: "de:ad:be:ef:00:07", rssi: -84, channel: 1, auth: "open" },
  ];
  let scan: { scanning: boolean; age_s: number | null; results: typeof scanResults } = { scanning: false, age_s: null, results: [] };
  let scanAt = 0;

  // ---- SignalK
  const servers = [
    { host: "192.168.1.10", port: 80, self: "urn:mrn:signalk:uuid:0e6d1a1a-1111-4111-8111-000000000099", name: "boat", roles: "master,main", swname: "signalk-server", swvers: "2.31.1" },
    { host: "192.168.1.11", port: 3000, self: "urn:mrn:signalk:uuid:0e6d1a1a-2222-4222-8222-000000000042", name: "nav-pc", roles: "master,main", swname: "signalk-server", swvers: "2.30.0" },
  ];
  interface SkServer {
    source: string; host?: Scalar | undefined; port?: Scalar | undefined; self?: Scalar | undefined;
    name?: Scalar | undefined; swname?: Scalar | undefined; swvers?: Scalar | undefined;
  }
  interface SkToken {
    state: string; has_token: boolean; busy: boolean; last_http_status: number; last_error: string;
    counts: { requests: number; approved: number; denied: number; unauthorized: number };
    pending_s?: number; pending_href?: string; approved_s?: number;
  }
  const sk = {
    token: { state: "no_server", has_token: false, busy: false, last_http_status: 0, last_error: "", counts: { requests: 0, approved: 0, denied: 0, unauthorized: 0 } } as SkToken,
    server: { source: "none" } as SkServer, client_id: "9cf791de-aa92-4830-a958-0388a42ef72b", description: "espOS espos-1a2b", permissions: "readwrite",
    discovery: { enabled: true, count: 0, last_s: null as number | null },
    ws: { enabled: true, connected: false, reconnects: 0, sent: 0, send_errors: 0, pending: 0, buffered: 0, buffered_bytes: 0, dropped: 0, last_error: "", meta: { declared: 3, reconciled: 0 }, in: { subs: 2, frames: 0, received: 0 }, put: { pending: 0, ok: 0, failed: 0 } },
  };
  let discovered: ((typeof servers)[number] & { seen_s: number })[] = [];
  let discoverAt = 0;
  let pendingAt = 0;
  const skEmit = () => emit("sk", skStatus());
  const skStatus = () => {
    const s = structuredClone(sk);
    s.discovery.last_s = discoverAt ? Math.round((Date.now() - discoverAt) / 1000) : null;
    if (s.token.state === "pending") s.token.pending_s = Math.round((Date.now() - pendingAt) / 1000);
    return s;
  };
  function skDiscover() {
    if (wifi.state !== "connected") return;
    discovered = servers.map((s) => ({ ...s, seen_s: 0 }));
    discoverAt = Date.now();
    sk.discovery.count = discovered.length;
    logAndMark("I", "espos_sk", `discovery: ${discovered.length} server(s)`);
    emit("sk_servers", serversDoc());
    if (sk.server.source === "none") {
      const c = cfg("sk");
      const pick: SkServer = c.server_host ? { host: c.server_host, port: c.server_port, self: c.server_self || "", source: "manual", name: c.server_host }
        : { ...discovered[0], source: "discovered" };
      sk.server = { host: pick.host, port: pick.port, self: pick.self, source: pick.source, name: pick.name, swname: pick.swname ?? "", swvers: pick.swvers ?? "" };
      skRequest();
    }
  }
  const serversDoc = () => ({ servers: discovered.map((s) => ({ ...s, selected: s.self === sk.server.self })), last_s: discoverAt ? Math.round((Date.now() - discoverAt) / 1000) : null });
  let approveTimer: ReturnType<typeof setTimeout> | undefined;
  function skRequest() {
    if (sk.token.has_token) return;
    Object.assign(sk.token, { state: "requesting", busy: true }); skEmit();
    setTimeout(() => {
      Object.assign(sk.token, { state: "pending", busy: false, last_http_status: 202, pending_href: "/signalk/v1/access/requests/" + sk.client_id });
      sk.token.counts.requests++; pendingAt = Date.now(); skEmit();
      logAndMark("I", "espos_sk", "access request PENDING — approve it in the SignalK admin UI");
      // the "admin" approves after a while unless the UI pastes a token first
      clearTimeout(approveTimer);
      approveTimer = setTimeout(() => {
        if (sk.token.state !== "pending") return;
        Object.assign(sk.token, { state: "approved", has_token: true, approved_s: 0, last_http_status: 200 });
        delete sk.token.pending_href; sk.token.counts.approved++; skEmit();
        logAndMark("I", "espos_sk", "access APPROVED, token stored");
        wsConnect();
      }, 12000);
    }, 800);
  }
  let wsTimer: ReturnType<typeof setInterval> | undefined;
  function wsConnect() {
    clearTimeout(wsTimer);
    if (!sk.token.has_token || !cfg("sk").ws_enabled || wifi.state !== "connected") {
      if (sk.ws.connected) { sk.ws.connected = false; emit("sk_ws", sk.ws); }
      return;
    }
    Object.assign(sk.ws, { connected: true, connected_s: 0, reconnects: sk.ws.reconnects + 1, last_error: "" });
    sk.ws.meta.reconciled = sk.ws.meta.declared;
    emit("sk_ws", sk.ws); skEmit();
    logAndMark("I", "espos_skws", `stream connected to ${sk.server.host}:${sk.server.port}`);
    // connected_s exists from the Object.assign above on. It stays out of the
    // literal so /sk/status keeps omitting it until the stream first connects.
    const ws = sk.ws as typeof sk.ws & { connected_s: number };
    wsTimer = setInterval(() => { sk.ws.sent += 2; ws.connected_s += 5; sk.ws.in.frames += 9; sk.ws.in.received += 12; }, 5000);
  }
  function skForget() {
    Object.assign(sk.token, { state: "no_server", has_token: false, busy: false, last_http_status: 0, last_error: "" });
    delete sk.token.approved_s; delete sk.token.pending_href;
    clearInterval(wsTimer); sk.ws.connected = false; emit("sk_ws", sk.ws);
    logAndMark("W", "espos_sk", "token forgotten");
    skEmit();
    if (sk.server.source !== "none") skRequest();
  }

  // ---- OTA
  const ota = {
    state: "idle", last_error: "",
    running: { version: "0.5.0-mock", project: "espos", target: "esp32c6", slot: "ota_0", image_state: "valid", pending_verify: false, confirmed: true, other_slot: "ota_1", other_version: "0.4.9", rolled_back: false, built: "Aug 18 2026 12:00:00", idf: "v6.0.2", key_fp: "b3381b48b9cc9941" },
    manifest: { url: "" as Scalar | undefined, channel: "stable" as Scalar | undefined, auto_check: true as Scalar | undefined, auto_install: false as Scalar | undefined,
      last_check_s: null as number | null, next_check_s: null as number | null },
    progress: { received: 0, total: 0 },
    available: null as { version: string; url: string; size: number; sha256: string; notes: string; newer: boolean } | null,
  };
  let checkAt = 0;
  const otaStatus = () => { const c = cfg("ota"); const last = checkAt ? Math.round((Date.now() - checkAt) / 1000) : null; ota.manifest.url = c.manifest_url; ota.manifest.channel = c.channel; ota.manifest.auto_check = c.auto_check; ota.manifest.auto_install = c.auto_install; ota.manifest.last_check_s = last; ota.manifest.next_check_s = last !== null && c.auto_check ? Number(c.check_h) * 3600 - last : null; return ota; };
  const otaEmit = () => emit("ota", otaStatus());
  function otaCheck() {
    const c = cfg("ota");
    if (!c.manifest_url) { Object.assign(ota, { state: "failed", last_error: "no manifest URL configured" }); return otaEmit(); }
    Object.assign(ota, { state: "checking", last_error: "" }); otaEmit();
    setTimeout(() => {
      checkAt = Date.now();
      ota.available = { version: "0.5.1", url: new URL("espos-esp32c6-0.5.1.bin", String(c.manifest_url)).href, size: 1180000, sha256: "", notes: "mock: bug fixes", newer: true };
      ota.state = "available"; otaEmit();
      logAndMark("I", "espos_ota", "manifest: 0.5.1 available (running 0.5.0-mock)");
    }, 900);
  }
  function otaInstall(url: string) {
    if (url.includes("unsigned")) {
      Object.assign(ota, { state: "downloading", progress: { received: 0, total: 900000 } }); otaEmit();
      setTimeout(() => { Object.assign(ota, { state: "failed", last_error: "image rejected: bad signature or corrupt (ESP_ERR_OTA_VALIDATE_FAILED)" }); otaEmit(); }, 2500);
      return;
    }
    Object.assign(ota, { state: "downloading", last_error: "", progress: { received: 0, total: 1180000 } }); otaEmit();
    const t = setInterval(() => {
      ota.progress.received = Math.min(ota.progress.total, ota.progress.received + 120000); otaEmit();
      if (ota.progress.received >= ota.progress.total) {
        clearInterval(t);
        ota.state = "ready"; otaEmit();
        logAndMark("W", "espos_ota", "update installed, rebooting");
        setTimeout(() => { Object.assign(ota, { state: "idle", available: null, progress: { received: 0, total: 0 } }); ota.running.version = "0.5.1"; ota.running.slot = "ota_1"; ota.running.other_slot = "ota_0"; ota.running.other_version = "0.5.0-mock"; ota.running.pending_verify = true; ota.running.confirmed = false; ota.running.image_state = "pending_verify"; otaEmit();
          setTimeout(() => { ota.running.pending_verify = false; ota.running.confirmed = true; ota.running.image_state = "valid"; otaEmit(); logAndMark("I", "espos_ota", "new image confirmed (network up), rollback cancelled"); }, 5000); }, 2000);
      }
    }, 400);
  }

  // ---- authentication (docs/rest-api.md "Authentication", docs/security.md)
  // Open until httpd.api_key is set; then Bearer or the espos_sid cookie from
  // POST /auth/login. The Origin check on cookie writes accepts a localhost
  // origin of any port on top of the exact match: the Vite dev server proxies
  // the UI (origin localhost:5173) to this mock (host 127.0.0.1:8484), so the
  // strict rule the device applies would refuse every save in development.
  const auth = { sessions: new Map<string, { expires: number }>(), fails: 0, failFirst: 0, lockedUntil: 0 };
  const apiKey = () => String(effective()["httpd"]?.["api_key"] ?? "");
  const authRequired = () => apiKey() !== "";
  const cookies = (req: IncomingMessage) => Object.fromEntries((req.headers.cookie ?? "").split(";").map((c) => c.trim().split("=", 2)).filter((kv): kv is [string, string] => kv.length === 2));
  // A session id is never empty, so a missing cookie finds no session either way.
  const sessionId = (req: IncomingMessage) => cookies(req)["espos_sid"] ?? "";
  const throttled = () => Date.now() < auth.lockedUntil;
  function checkKey(k: string) {
    if (throttled()) return "throttled";
    if (k === apiKey()) { auth.fails = 0; return "ok"; }
    const now = Date.now();
    if (!auth.fails || now - auth.failFirst >= 60000) { auth.fails = 0; auth.failFirst = now; }
    if (++auth.fails >= 5) { auth.lockedUntil = now + 30000; auth.fails = 0; logAndMark("W", "espos_auth", "too many failed keys: refusing key checks for 30 s"); }
    return "bad";
  }
  const originOk = (req: IncomingMessage) => {
    const o = req.headers.origin ?? req.headers.referer;
    if (!o) return false;
    try { const a = new URL(o).host; return a === (req.headers.host ?? "") || /^(localhost|127\.0\.0\.1)(:\d+)?$/.test(a); } catch { return false; }
  };
  const throttle429 = (res: ServerResponse) => { res.setHeader("Retry-After", String(Math.max(1, Math.ceil((auth.lockedUntil - Date.now()) / 1000)))); return err(res, 429, "too_many_attempts", "too many failed keys; wait before trying again"); };
  // The method the request authenticated with ("none" when it did not), or
  // false when a refusal has been sent (never for a public endpoint).
  function authenticate(req: IncomingMessage, res: ServerResponse, isPublic: boolean): "none" | "bearer" | "cookie" | false {
    const stateChanging = !["GET", "HEAD", "OPTIONS"].includes(req.method ?? "");
    let method: "none" | "bearer" | "cookie" = "none", verdict = "allow";
    const bearer = req.headers.authorization;
    if (!authRequired()) verdict = "allow";
    else if (bearer && /^bearer /i.test(bearer)) {
      const r = checkKey(bearer.slice(7).trim());
      if (r === "ok") method = "bearer"; else verdict = r === "throttled" ? "throttled" : "unauthorized";
    } else {
      const s = auth.sessions.get(sessionId(req));
      if (s && s.expires > Date.now()) { method = "cookie"; if (stateChanging && !originOk(req)) verdict = "origin"; }
      else verdict = "unauthorized";
    }
    if (isPublic || verdict === "allow") return method;
    if (verdict === "unauthorized") { res.setHeader("WWW-Authenticate", 'Bearer realm="espOS"'); err(res, 401, "unauthorized", "authentication required: Authorization: Bearer <key>, or log in at /api/v1/auth/login"); }
    else if (verdict === "origin") err(res, 403, "forbidden", "cross-site request: Origin does not match Host");
    else throttle429(res);
    return false;
  }

  // ---- HTTP
  const json = (res: ServerResponse, status: number, body: unknown, headers: Record<string, string> = {}) => {
    const data = JSON.stringify(body);
    res.writeHead(status, { "Content-Type": "application/json", "Cache-Control": "no-store", ...headers });
    res.end(data);
  };
  const err = (res: ServerResponse, status: number, code: string, message: string, extra: Record<string, unknown> = {}) => json(res, status, { error: code, message, ...extra });
  // Health conditions. `drills` holds whatever POST /health/test has raised; a key
  // stays in the map once seen, at "normal", which is what the device does -- a
  // condition keeps its table slot for the life of the boot.
  type HealthState = "normal" | "warn" | "alarm";
  interface Condition { key: string; state: HealthState; message: string; reboot_on_alarm: boolean }
  const healthStates: readonly string[] = ["normal", "warn", "alarm"];
  const isHealthState = (s: string): s is HealthState => healthStates.includes(s);
  const drills = new Map<string, Condition>();
  // Deadlines live apart from the drill objects so they never appear in a response.
  // The device clears a drill on the policy tick once its ttl is up; a mock that
  // kept one raised for ever would teach the UI that drills are permanent.
  const drillDeadlines = new Map<string, number>();
  const expireDrills = () => {
    const now = Date.now();
    for (const [k, due] of drillDeadlines) {
      if (now >= due) {
        const c = drills.get(k);
        if (c) c.state = "normal";
        drillDeadlines.delete(k);
      }
    }
  };
  const healthBody = () => {
    expireDrills();
    const builtins: Condition[] = [
      { key: "lowMemory", state: "normal", message: "", reboot_on_alarm: true },
      { key: "memoryTrough", state: "warn",
        message: "internal RAM low-water mark below 10 KB since boot", reboot_on_alarm: false },
      { key: "netDown", state: "normal", message: "", reboot_on_alarm: false },
    ];
    const conditions = [...builtins, ...drills.values()];
    const rank: Record<HealthState, number> = { normal: 0, warn: 1, alarm: 2 };
    const worst = conditions.reduce<HealthState>((w, c) => (rank[c.state] > rank[w] ? c.state : w), "normal");
    const fatal = conditions.find((c) => c.state === "alarm" && c.reboot_on_alarm);
    return { worst, fatal: fatal ? fatal.key : null, conditions };
  };
  const body = (req: IncomingMessage) => new Promise<string>((resolve) => { let b = ""; req.on("data", (c: Buffer) => (b += c.toString())); req.on("end", () => resolve(b)); });
  // What the request body parses to, or null when it does not parse. JSON.parse
  // can only produce Json, which is what lets the handlers narrow it.
  const parseBody = async (req: IncomingMessage, empty?: string): Promise<Json> => { try { return JSON.parse((await body(req)) || (empty ?? "")) as Json; } catch { return null; } };
  // A member of a parsed body. An array has none of the keys the API uses.
  const field = (o: Json, k: string): Json | undefined => (o && typeof o === "object" && !Array.isArray(o) ? o[k] : undefined);
  const needJson = (req: IncomingMessage, res: ServerResponse) => {
    if (!/^application\/json/.test(req.headers["content-type"] ?? "")) { err(res, 415, "unsupported_media_type", "Content-Type: application/json required"); return false; }
    return true;
  };

  function validate(ns: string, k: string, v: Json) {
    const p = schema.properties[ns]?.properties[k];
    if (!p) return `unknown key ${ns}.${k}`;
    if (v === null) return null;
    if (p.type === "boolean" && typeof v !== "boolean") return "expected boolean";
    if ((p.type === "integer" || p.type === "number") && typeof v !== "number") return "expected number";
    if (p.type === "integer" && !Number.isInteger(v)) return "expected integer";
    if (p.type === "string" && typeof v !== "string") return "expected string";
    if (typeof v === "number" && ((p.minimum !== undefined && v < p.minimum) || (p.maximum !== undefined && v > p.maximum))) return `out of range [${p.minimum},${p.maximum}]`;
    if (typeof v === "string" && p.maxLength !== undefined && v.length > p.maxLength) return `longer than ${p.maxLength}`;
    if (p.enum && !p.enum.some((e) => e === v)) return `not one of ${p.enum.join(", ")}`;
    // The generator puts a pattern on string keys only, and a non-string was refused above.
    if (p.pattern && typeof v === "string" && !new RegExp(p.pattern).test(v)) return `does not match ${p.pattern}`;
    return null;
  }

  // node:http ignores what a listener returns, so the promise is dropped on
  // purpose; handle() catches everything itself.
  const server = http.createServer((req, res) => { void handle(req, res); });
  async function handle(req: IncomingMessage, res: ServerResponse): Promise<void> {
    const url = new URL(req.url ?? "", "http://x");
    const p = url.pathname;
    const m = req.method ?? "";
    if (!p.startsWith("/api/v1/")) return err(res, 404, "not_found", "no such resource");
    const r = p.slice("/api/v1".length);
    try {
      const isPublic = r.startsWith("/auth/") || r === "/system/ping";
      const method = authenticate(req, res, isPublic);
      if (method === false) return;
      // ---- auth
      if (r === "/auth/status" && m === "GET") return json(res, 200, { required: authRequired(), configured: authRequired(), authenticated: method !== "none", method, recovery_s: 0 });
      if (r === "/auth/login" && m === "POST") {
        if (!needJson(req, res)) return;
        if (!authRequired()) return err(res, 409, "auth_open", "no API key is configured; the API is open");
        const doc = await parseBody(req);
        const key = field(doc, "key");
        if (!doc || typeof key !== "string") return err(res, 400, "validation", "expected {\"key\": \"...\"}");
        const rr = checkKey(key);
        if (rr === "throttled") return throttle429(res);
        if (rr === "bad") { logAndMark("W", "espos_auth", "login refused: wrong key"); return err(res, 401, "unauthorized", "wrong key"); }
        const sid = randomUUID().replace(/-/g, "");
        const ttl = Number(cfg("httpd").session_ttl_s ?? 86400);
        if (auth.sessions.size >= 4) auth.sessions.delete(auth.sessions.keys().next().value ?? "");   // oldest first, like the device's LRU
        auth.sessions.set(sid, { expires: Date.now() + ttl * 1000 });
        logAndMark("I", "espos_auth", `login: session opened (${ttl} s)`);
        res.writeHead(204, { "Set-Cookie": `espos_sid=${sid}; HttpOnly; SameSite=Strict; Path=/; Max-Age=${ttl}`, "Cache-Control": "no-store" });
        res.end(); return;
      }
      if (r === "/auth/logout" && m === "POST") {
        if (!needJson(req, res)) return;
        if (auth.sessions.delete(sessionId(req))) logAndMark("I", "espos_auth", "logout: session closed");
        res.writeHead(204, { "Set-Cookie": "espos_sid=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0", "Cache-Control": "no-store" });
        res.end(); return;
      }
      if (r === "/system/ping" && m === "GET") return json(res, 200, { app: "espos", version: "0.5.0-mock", auth: authRequired() });
      // ---- system
      if (r === "/system/info" && m === "GET") {
        return json(res, 200, { app: "espos", version: "0.5.0-mock", idf_version: "v6.0.2", chip: "esp32c6", chip_revision: 1, cores: 1,
          uptime_s: Math.round((Date.now() - boot) / 1000), free_heap: 214000 + Math.round(Math.random() * 3000), min_free_heap: 190000,
          // The internal pair: live, then low-water. Deliberately unrelated to the
          // whole-heap pair above -- the two ask different capability sets and each
          // sums the regions matching it, so nothing guarantees an ordering between
          // them, and a mock that made them equal would teach the UI a rule ESP-IDF
          // does not honour even on this no-PSRAM C6. WITHIN the pair the ordering
          // IS guaranteed (a region's minimum cannot exceed its current free size),
          // so free_internal stays above min_internal_free here.
          free_internal: 181000 + Math.round(Math.random() * 2000), min_internal_free: 176000,
          reset_reason: "software", config_storage_reset: false, schema_etag: etag, ui_storage: true,
          // A board that declared itself, so the Board row is exercised; a C6
          // has no PSRAM, which is the "no PSRAM" branch rather than a missing
          // one. Both are states a happy-path mock would never show.
          hardware: { mac: "60:55:f9:00:1a:2b", cpu_mhz: 160, flash_bytes: 8 * 1024 * 1024,
            ram_internal_bytes: 512 * 1024, ram_psram_bytes: 0,
            features: ["wifi", "ble", "802.15.4", "embedded-flash"],
            // No hardware.coprocessor here on purpose. This mock is a C6
            // DevKit -- chip, board and /ota/status target all say so -- and a
            // C6 is its own radio, so the field cannot exist on it. Adding it
            // would also contradict healthBody(), which raises no
            // coprocessorStale. Nothing in ui/src reads the field today; when
            // something does, turn this mock into a P4 as ONE change (chip,
            // board, OTA target, the coprocessor object AND the matching
            // health condition) rather than bolting the field onto a device
            // that cannot have it.
            board: "Espressif ESP32-C6-DevKitC-1" } });
      }
      if (r === "/system/reboot" && m === "POST") { if (!needJson(req, res)) return; logAndMark("W", "espos_httpd", "restarting"); return json(res, 202, { status: "rebooting" }); }
      if (r === "/system/factory-reset" && m === "POST") { if (!needJson(req, res)) return; for (const k of Object.keys(stored)) delete stored[k]; wifiEval(); return json(res, 202, { status: "factory_reset", rebooting: true }); }
      if (r === "/system/coredump" && m === "GET") {
        return json(res, 200, { present: true, size: 18344, valid: true, task: "espos_skws", pc: "0x4200a5c2", app_elf_sha256: "3f9c1e2b7d0a44f1", version: 1,
          mcause: 7, mtval: "0x00000010", ra: "0x42009f80", sp: "0x3fc9e2d0", stackdump_bytes: 1024 });
      }
      if (r === "/system/coredump" && m === "DELETE") return json(res, 200, { status: "erased" });
      // ---- config
      if (r === "/config/schema" && m === "GET") {
        if (req.headers["if-none-match"] === `"${etag}"`) { res.writeHead(304); res.end(); return; }
        res.writeHead(200, { "Content-Type": "application/schema+json", ETag: `"${etag}"` }); res.end(JSON.stringify(schema)); return;
      }
      if (r === "/config" && m === "GET") {
        const ns = url.searchParams.get("ns");
        if (ns && !schema.properties[ns]) return err(res, 404, "unknown_namespace", `no namespace ${ns}`);
        return json(res, 200, redacted(ns));
      }
      if (r === "/config" && m === "PUT") {
        if (!needJson(req, res)) return;
        let doc: Json; try { doc = JSON.parse(await body(req)) as Json; } catch { return err(res, 400, "validation", "malformed JSON", { path: "" }); }
        if (!doc || typeof doc !== "object" || Array.isArray(doc)) return err(res, 400, "validation", "expected object of namespaces", { path: "" });
        for (const [ns, o] of Object.entries(doc)) {
          if (!schema.properties[ns]) return err(res, 400, "validation", `unknown namespace ${ns}`, { path: ns });
          if (!o || typeof o !== "object") return err(res, 400, "validation", "expected object", { path: ns });
          for (const [k, v] of Object.entries(o)) { const e = validate(ns, k, v); if (e) return err(res, 400, "validation", e, { path: `${ns}.${k}` }); }
        }
        const before = effective();
        const changed = [];
        let restart = false;
        // The loop above has checked every namespace is an object and every
        // value passes validate(), which admits scalars only.
        for (const [ns, o] of Object.entries(doc)) for (const [k, v] of Object.entries(o as JsonObject)) {
          if (isSecret(ns, k) && v === "********") continue;
          const dst = (stored[ns] ??= {});
          if (v === null) delete dst[k]; else dst[k] = v as Scalar;
          if (JSON.stringify(effective()[ns]?.[k]) !== JSON.stringify(before[ns]?.[k])) {
            changed.push(`${ns}.${k}`);
            if (schema.properties[ns]?.properties[k]?.["x-espos-restartRequired"]) restart = true;
            emit("config", { ns, key: k });
            logAndMark("I", "espos_config", `changed ${ns}.${k}`);
          }
        }
        if (changed.includes("httpd.api_key")) { auth.sessions.clear(); logAndMark(authRequired() ? "I" : "W", "espos_auth", authRequired() ? "API key set: protected endpoints need Bearer or a login" : "no API key set: the REST API is open to the network"); }
        if (changed.some((c) => c.startsWith("wifi."))) wifiEval();
        if (changed.some((c) => c.startsWith("sk."))) { if (!cfg("sk").ws_enabled) wsConnect(); else if (sk.token.has_token) wsConnect(); }
        return json(res, 200, { changed, restart_required: restart });
      }
      // ---- wifi
      if (r === "/wifi/status" && m === "GET") return json(res, 200, wifiStatus());
      if (r === "/wifi/scan" && m === "POST") {
        if (!needJson(req, res)) return;
        if (scan.scanning) return err(res, 409, "busy", "scan in progress");
        scan = { ...scan, scanning: true };
        setTimeout(() => { scanAt = Date.now(); scan = { scanning: false, age_s: 0, results: scanResults }; emit("wifi_scan", scanDoc()); logAndMark("I", "espos_wifi", `scan done: ${scanResults.length} networks`); }, 1500);
        return json(res, 202, { status: "scanning" });
      }
      const scanDoc = () => ({ ...scan, age_s: scanAt ? Math.round((Date.now() - scanAt) / 1000) : null });
      if (r === "/wifi/scan" && m === "GET") return json(res, 200, scanDoc());
      // ---- flow
      // A graph that is doing something slightly wrong, because a page that
      // has only ever been seen in its happy state is a page whose warnings
      // have never been read: the edge pool is deliberately near full and a
      // few posts have been dropped.
      // ---- health. The device's conditions, and the drill endpoint that lets a
      // hardware test exercise a buzzer/LED without causing a real fault. A
      // deliberately mixed set: one fatal alarm, one non-fatal warning and one
      // condition sitting at normal, because a key keeps its slot once raised.
      if (r === "/health" && m === "GET") return json(res, 200, healthBody());
      if (r === "/health/test" && m === "POST") {
        if (!needJson(req, res)) return;
        const b = await parseBody(req, "{}");
        if (!b || typeof b !== "object") return err(res, 400, "bad_request", "not JSON");
        const key = field(b, "key"), state = field(b, "state"), message = field(b, "message"), ttl = field(b, "ttl_s");
        if (typeof key !== "string" || typeof state !== "string") {
          return err(res, 400, "validation", "key and state are required strings");
        }
        if (!key.startsWith("test.") || key === "test.") {
          return err(res, 400, "validation", 'key must start with "test." and name something');
        }
        if (!isHealthState(state)) {
          return err(res, 400, "validation", "state must be normal, warn or alarm");
        }
        if (message !== undefined && message !== null && typeof message !== "string") {
          return err(res, 400, "validation", "message must be a string");
        }
        if (key.length >= 24 || (message ?? "").length >= 96) {
          return err(res, 400, "validation", "key must be under 24 bytes and message under 96");
        }
        if (ttl !== undefined && ttl !== null) {
          if (typeof ttl !== "number") return err(res, 400, "validation", "ttl_s must be a number");
          if (ttl < 1 || ttl > 300) return err(res, 400, "validation", "ttl_s must be 1..300");
        }
        // The table filling is the one failure a well-formed request still meets,
        // and the UI has to tell the difference between that and a server fault.
        if (key === "test.noslot") {
          return err(res, 507, "no_slot",
                     "the condition table is full; reuse a test key or raise CONFIG_ESPOS_HEALTH_MAX_CONDITIONS");
        }
        // One drill at a time: raising a second clears the first.
        for (const c of drills.values()) c.state = "normal";
        drillDeadlines.clear();
        drills.set(key, { key, state, message: message ?? "", reboot_on_alarm: false });
        if (state !== "normal") {
          drillDeadlines.set(key, Date.now() + (ttl ?? 45) * 1000);
        }
        return json(res, 200, healthBody());
      }

      if (r === "/flow" && m === "GET") return json(res, 200, {
        running: true,
        loop: { posts: 18240, dropped: 3, timers_fired: 9120, timers_live: 4,
                queue_peak: 11, edges_used: 78, edges_max: 96 },
        nodes: [
          { id: "light", title: "Analog(GPIO4)" },
          { id: "cal", title: "Linear(1.7007, -0.165)" },
          { id: "avg", title: "MovingAverage(10)" },
          { id: "out", title: "sk::Output<float>(environment.inside.illuminance)" },
        ],
      });
      // ---- signalk
      if (r === "/sk/status" && m === "GET") return json(res, 200, skStatus());
      if (r === "/sk/servers" && m === "GET") return json(res, 200, serversDoc());
      if (r === "/sk/discover" && m === "POST") { if (!needJson(req, res)) return; setTimeout(skDiscover, 600); return json(res, 202, { status: "discovering" }); }
      if (r === "/sk/request" && m === "POST") { if (!needJson(req, res)) return; skRequest(); return json(res, 202, { status: "requesting" }); }
      if (r === "/sk/forget" && m === "POST") { if (!needJson(req, res)) return; skForget(); return json(res, 202, { status: "forgotten" }); }
      if (r === "/sk/token" && m === "POST") {
        if (!needJson(req, res)) return;
        const doc = await parseBody(req);
        const token = field(doc, "token");
        if (!doc || typeof token !== "string" || !token) return err(res, 400, "validation", "expected {\"token\": \"...\"}");
        Object.assign(sk.token, { state: "verifying", busy: true }); skEmit();
        setTimeout(() => { Object.assign(sk.token, { state: "approved", has_token: true, busy: false, approved_s: 0, last_http_status: 200 }); skEmit(); wsConnect(); }, 900);
        return json(res, 202, { status: "verifying" });
      }
      if (r === "/sk/publish" && m === "POST") { if (!needJson(req, res)) return; sk.ws.pending++; return json(res, 202, { status: "queued" }); }
      if (r === "/sk/put" && m === "POST") { if (!needJson(req, res)) return; if (!sk.ws.connected) return err(res, 503, "not_connected", "stream not connected"); sk.ws.put.ok++; return json(res, 202, { status: "sent" }); }
      if (r === "/sk/put" && m === "GET") return json(res, 200, sk.ws.put.ok ? { request_id: "9cf791de-0000-4000-8000-000000000001", state: "COMPLETED", status_code: 200, message: "" } : null);
      // ---- ota
      if (r === "/ota/status" && m === "GET") return json(res, 200, otaStatus());
      if (r === "/ota/check" && m === "POST") { if (!needJson(req, res)) return; if (["checking", "downloading", "ready"].includes(ota.state)) return err(res, 409, "busy", "an update or check is in progress"); otaCheck(); return json(res, 202, { status: "checking" }); }
      if (r === "/ota" && m === "POST") {
        if (!needJson(req, res)) return;
        if (["checking", "downloading", "ready"].includes(ota.state)) return err(res, 409, "busy", "an update or check is in progress");
        const doc = await parseBody(req, "{}");
        if (!doc || typeof doc !== "object") return err(res, 400, "validation", "expected a JSON object");
        const target = field(doc, "url");
        if (target) { if (typeof target !== "string" || !/^https?:\/\//.test(target)) return err(res, 400, "validation", "expected an http(s) URL"); otaInstall(target); }
        else if (ota.available) otaInstall(ota.available.url);
        else return err(res, 404, "not_found", "no update known; check first");
        return json(res, 202, { status: "installing" });
      }
      if (r === "/ota/confirm" && m === "POST") { if (!needJson(req, res)) return; ota.running.pending_verify = false; ota.running.confirmed = true; ota.running.image_state = "valid"; otaEmit(); return json(res, 202, { status: "confirmed" }); }
      if (r === "/ota/rollback" && m === "POST") { if (!needJson(req, res)) return; logAndMark("W", "espos_ota", "rollback requested"); Object.assign(ota, { state: "failed", last_error: "rollback requested" }); otaEmit(); return json(res, 202, { status: "rolling_back" }); }
      // ---- logs
      if (r === "/logs" && m === "GET") {
        const after = Number(url.searchParams.get("after") ?? 0);
        const limit = Math.min(Number(url.searchParams.get("limit") ?? 200) || 200, 1000);
        const first = logs[0]?.seq ?? logSeq;
        const gap = after + 1 < first;
        const from = gap ? first : after + 1;
        const lines = logs.filter((l) => l.seq >= from).slice(0, limit);
        return json(res, 200, { first, next: logSeq, dropped: first - 1, size: 16384, used: logs.reduce((a, l) => a + l.line.length + 2, 0), gap, from, lines: lines.map((l) => l.line) });
      }
      if (r === "/logs/level" && m === "PUT") {
        if (!needJson(req, res)) return;
        const doc = await parseBody(req);
        const levels: readonly Json[] = ["none", "error", "warn", "info", "debug", "verbose"];
        const level = field(doc, "level"), tag = field(doc, "tag") ?? "*";
        if (!doc || typeof level !== "string" || !levels.includes(level)) return err(res, 400, "validation", "expected {\"level\": none|error|warn|info|debug|verbose[, \"tag\": \"...\"]}");
        logAndMark("I", "espos_httpd", `log level ${typeof tag === "string" ? tag : JSON.stringify(tag)} = ${level}`);
        return json(res, 200, { tag, level });
      }
      // ---- events
      if (r === "/events" && m === "GET") {
        res.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-cache", Connection: "keep-alive" });
        res.write("retry: 3000\n\n");
        res.write(`event: wifi\ndata: ${JSON.stringify(wifiStatus())}\n\n`);
        res.write(`event: sk\ndata: ${JSON.stringify(skStatus())}\n\n`);
        res.write(`event: sk_servers\ndata: ${JSON.stringify(serversDoc())}\n\n`);
        res.write(`event: ota\ndata: ${JSON.stringify(otaStatus())}\n\n`);
        clients.add(res);
        const ping = setInterval(() => res.write(": ping\n\n"), 15000);
        req.on("close", () => { clients.delete(res); clearInterval(ping); });
        return;
      }
      if (["PUT", "POST", "DELETE"].includes(m)) return err(res, 404, "not_found", "no such resource");
      return err(res, 404, "not_found", "no such resource");
    } catch (e) {
      return err(res, 500, "internal", String(e));
    }
  }
  server.listen(port, "127.0.0.1", () => console.log(`  [mock] espOS API mock on http://127.0.0.1:${port}/api/v1`));
  logAndMark("I", "espos_config", "ready: 4 namespace(s)");
  logAndMark("I", "espos_httpd", "listening on :80");
  wifiEval();
  setInterval(() => logAndMark("I", "app", `heartbeat ${Math.round((Date.now() - boot) / 1000)}`), 7000);
  return server;
}

function hash(s: string) { let h = 0; for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0; return h; }

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  startMock(Number(process.argv[2] ?? 8484));
}
