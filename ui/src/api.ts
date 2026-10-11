// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
// The one place that knows the REST contract (docs/rest-api.md): typed fetch
// helpers, the SSE connection, a tiny subscribable store per event, and the
// authentication state the shell decides the login page from.
import { useEffect, useState } from "preact/hooks";

// Relative by default: the bundle is served by the device it talks to. A
// build for another origin sets VITE_ESPOS_BASE to an absolute URL.
export const BASE: string = (import.meta.env["VITE_ESPOS_BASE"] as string | undefined) ?? "/api/v1";
const ABSOLUTE = /^https?:\/\//i.test(BASE);
// The login cookie is SameSite=Strict and the device grants no CORS, so a
// page that is not served by the device — an absolute BASE, or the Vite dev
// server proxying to a device or the mock (Origin never equals Host through a
// proxy, and the device refuses cookie writes whose Origin differs) — keeps
// the key in sessionStorage and sends it as a Bearer header instead. The
// built bundle on a device uses the cookie only.
const DEV_KEY_MODE = ABSOLUTE || !!import.meta.env.DEV;
const KEY_STORAGE = "espos.apiKey";

export interface ApiError { error: string; message: string; path?: string }
export class ApiFailure extends Error {
  constructor(public status: number, public body: ApiError | null) {
    super(body?.message ?? `HTTP ${status}`);
  }
}

function devKey(): string | null {
  if (!DEV_KEY_MODE) return null;
  try { return sessionStorage.getItem(KEY_STORAGE); } catch { return null; }
}
function setDevKey(key: string | null) {
  try { if (key) sessionStorage.setItem(KEY_STORAGE, key); else sessionStorage.removeItem(KEY_STORAGE); } catch { /* storage blocked: the key lives for this page only */ }
}

/* Long enough for a slow flash read over a busy shared radio, short enough
 * that a wedged request does not look like a broken device. */
const REQUEST_TIMEOUT_MS = 15000;

async function call<T>(method: string, path: string, body?: unknown): Promise<T> {
  const headers: Record<string, string> = {};
  const init: RequestInit = { method, headers, credentials: "same-origin" };
  if (body !== undefined) {
    headers["Content-Type"] = "application/json";
    init.body = JSON.stringify(body);
  } else if (method !== "GET") {
    headers["Content-Type"] = "application/json";
    init.body = "{}";
  }
  const key = devKey();
  if (key) headers["Authorization"] = `Bearer ${key}`;
  // Bounded, because an unbounded fetch is indistinguishable from a hang and
  // the caller has no way to recover: the app renders nothing until the first
  // /auth/status settles, so one request that never answers is a permanently
  // blank page. A device on the other side of a captive-portal association is
  // exactly where that happens. AbortSignal.timeout is on every browser that
  // runs this bundle; the optional chain keeps an ancient webview working
  // (unbounded, as before) rather than throwing.
  init.signal = AbortSignal.timeout?.(REQUEST_TIMEOUT_MS);
  const r = await fetch(BASE + path, init);
  const text = await r.text();
  let js: unknown;
  try { js = text ? JSON.parse(text) : null; } catch { js = null; }
  if (!r.ok) {
    const err = js as ApiError | null;
    // The session ended (expiry, a reboot, a new key): back to the login
    // page. The auth endpoints report their own 401s to whoever called them.
    if (r.status === 401 && !path.startsWith("/auth/")) { setDevKey(null); authStore.set("login"); }
    if (r.status === 403 && err?.error === "auth_unconfigured") authStore.set("unconfigured");
    throw new ApiFailure(r.status, err);
  }
  return js as T;
}
export const get = <T,>(path: string) => call<T>("GET", path);
export const put = <T,>(path: string, body: unknown) => call<T>("PUT", path, body);
export const post = <T,>(path: string, body?: unknown) => call<T>("POST", path, body);
export const del = <T,>(path: string) => call<T>("DELETE", path);

// ---- documents (shapes per docs/rest-api.md; only the fields the UI reads)
export interface WifiStatus {
  state: "disabled" | "unconfigured" | "connecting" | "obtaining_ip" | "connected" | "backoff";
  sta_enabled: boolean; hostname: string; reason: { code: number; text: string };
  ssid?: string; bssid?: string; rssi?: number; channel?: number; ip?: string; gateway?: string; netmask?: string;
  network_index?: number; backoff_ms?: number; attempt?: number; connect_count?: number; disconnect_count?: number;
  portal: {
    active: boolean;
    ssid: string;
    ip?: string;
    clients?: number;
    /** what espos_wifi_portal_open() last asked for; absent on firmware before 0.13 */
    force?: "auto" | "up" | "down";
  };
}
export interface ScanResult { ssid: string; bssid: string; rssi: number; channel: number; auth: string }
export interface ScanDoc { scanning: boolean; age_s: number | null; results: ScanResult[] }
export interface SkServer { host: string; port: number; self: string; name: string; roles?: string; swname?: string; swvers?: string; scheme?: "http" | "https"; seen_s?: number; selected?: boolean }
/** GET /sk/tls — what certificate is trusted, and what the server last showed.
 * `pinned` is null before anything is anchored; `presented` before the first
 * handshake. Absent (404) in a build without CONFIG_ESPOS_SK_TLS. */
export interface SkTls {
  trust: "tofu" | "ca" | "bundle";
  pinned: { kind: "ca" | "leaf"; cn: string; san: string; fingerprint: string; since: number } | null;
  last_error: string;
  presented: { cn: string; fingerprint: string } | null;
}
export interface SkServersDoc { servers: SkServer[]; last_s: number | null }
export interface SkWs {
  enabled: boolean; connected: boolean; connected_s?: number; reconnects: number; sent: number; send_errors: number;
  next_retry_s?: number; pending: number; buffered: number; buffered_bytes: number; dropped: number; last_error: string;
  meta: { declared: number; reconciled: number };
  in?: { subs: number; frames: number; received: number };
  put?: { pending: number; ok: number; failed: number };
}
export interface BleStatus {
  enabled: boolean; scanning: boolean; mac: string;
  scan_hits: number; adv_received: number; adv_posted: number; adv_dropped: number; adv_pending: number;
  post_success: number; post_fail: number; ws_connected: boolean;
  gatt_sessions: number; gatt_max: number;
}
export interface FlowNode { id: string; title?: string }
export interface FlowStatus {
  running: boolean;
  loop: { posts: number; dropped: number; timers_fired: number; timers_live: number;
    queue_peak: number; edges_used: number; edges_max: number };
  /** null when the firmware drives the loop from C and never built a Graph --
   *  which is different from an empty array, and the C side sends null on
   *  purpose so a client can tell them apart. */
  nodes: FlowNode[] | null;
}
export interface SkStatus {
  token: { state: string; has_token: boolean; busy: boolean; approved_s?: number; pending_s?: number; pending_href?: string;
    next_action_s?: number; last_check_s?: number; last_http_status: number; last_error: string;
    counts: { requests: number; approved: number; denied: number; unauthorized: number; cert_errors?: number } };
  server: { host?: string; port?: number; self?: string; source: "discovered" | "manual" | "pinned" | "none"; scheme?: "http" | "https"; name?: string; swname?: string; swvers?: string; by_self?: boolean };
  ws?: SkWs; client_id: string; description: string; permissions: string;
  discovery: { enabled: boolean; count: number; last_s: number | null };
}
export interface SystemInfo {
  app: string; version: string; idf_version: string; chip: string; chip_revision: number; cores: number;
  uptime_s: number; free_heap: number; min_free_heap: number; reset_reason: string; config_storage_reset: boolean;
  schema_etag: string; ui_storage?: boolean; ui?: "embedded" | "storage" | "placeholder";
  /** Absent on a device older than the field; every member optional because
   *  each comes from a call that can fail independently. */
  hardware?: {
    mac?: string; cpu_mhz?: number; flash_bytes?: number;
    ram_internal_bytes?: number; ram_psram_bytes?: number;
    /** "wifi" | "ble" | "bt-classic" | "802.15.4" | "embedded-flash" | "embedded-psram" */
    features?: string[];
    /** Only when the firmware declared one (espos_start_opts_t.board). */
    board?: string;
  };
}
export interface PingDoc { app: string; version: string; auth: boolean }
export interface AuthStatus {
  /** protected endpoints need a credential: a key is set, or the build requires one */
  required: boolean;
  /** httpd.api_key is set */
  configured: boolean;
  /** this request carried a valid credential (or came from an exempt setup portal) */
  authenticated: boolean;
  method: "none" | "bearer" | "cookie" | "portal";
  /** seconds left of a portal recovery window; 0 when none is open. Optional
   * because `npm run dev` may be pointed at a device older than this field. */
  recovery_s?: number;
}
export interface LogsDoc { first: number; next: number; dropped: number; size: number; used: number; gap: boolean; from: number; lines: string[] }
export interface Coredump {
  present: boolean; size: number; valid: boolean; task?: string; pc?: string; app_elf_sha256?: string; version?: number;
  exc_cause?: number; exc_vaddr?: string; backtrace?: string[]; backtrace_corrupted?: boolean;
  mcause?: number; mtval?: string; ra?: string; sp?: string; stackdump_bytes?: number; summary_error?: string;
}
export interface OtaStatus {
  state: "idle" | "checking" | "available" | "downloading" | "verifying" | "ready" | "failed";
  last_error: string;
  running: { version: string; project: string; target: string; slot: string; image_state: string; pending_verify: boolean; confirmed: boolean;
    other_slot: string; other_version: string; rolled_back: boolean; built: string; idf: string };
  manifest: { url: string; channel: string; auto_check: boolean; auto_install: boolean; last_check_s: number | null; next_check_s: number | null };
  progress: { received: number; total: number };
  available: { version: string; url: string; size: number; sha256: string; notes: string; newer: boolean } | null;
}
export type ConfigDoc = Record<string, Record<string, unknown>>;
export interface JsonSchemaProp {
  title?: string; description?: string; type: "string" | "integer" | "number" | "boolean";
  default?: unknown; minimum?: number; maximum?: number; maxLength?: number; enum?: string[]; pattern?: string;
  "x-espos-secret"?: boolean; "x-espos-restartRequired"?: boolean; "x-espos-unit"?: string; "x-espos-type"?: string; "x-espos-maxBytes"?: number;
  // Presentation only. The device stores SI; the UI converts on the way in
  // and out, so a value shown in degrees is written back in radians.
  readOnly?: boolean;
  "x-espos-group"?: string;
  "x-espos-displayMultiplier"?: number;
  "x-espos-displayOffset"?: number;
  "x-espos-format"?: "table";
  "x-espos-columns"?: string[];
}
export interface JsonSchemaNs {
  title?: string; description?: string; "x-espos-version"?: number;
  properties: Record<string, JsonSchemaProp>;
  // true for a namespace a graph node registered at run time rather than one
  // a build-time descriptor declared.
  "x-espos-runtime"?: boolean;
}

// display = stored * multiplier + offset, and the exact inverse on write.
// A field that declares neither is untouched, so the identity path costs
// nothing and cannot introduce rounding.
/* A schema value as display text.
 *
 * Config values arrive as JSON, so their type is `unknown` and nothing stops one
 * being an object or an array -- the table format already stores a JSON string,
 * and a future field could hold the parsed thing. `String(v)` renders those as
 * "[object Object]", which is the kind of defect a user reports as "the page is
 * broken" and nobody can reproduce. JSON at least says what the value is. */
export function text(v: unknown): string {
  if (v === null || v === undefined) return "";
  if (typeof v === "string") return v;
  if (typeof v === "number" || typeof v === "boolean" || typeof v === "bigint") return String(v);
  /* Objects and arrays as JSON. Anything else -- a function, a symbol -- is not
   * something a config value can be, but String() on a symbol THROWS, and a
   * rendering helper that can throw is worse than one that renders nothing.
   * JSON.stringify returns undefined for those rather than raising. */
  try {
    return JSON.stringify(v) ?? "";
  } catch {
    return "";
  }
}

export function toDisplay(p: JsonSchemaProp, v: unknown): unknown {
  const m = p["x-espos-displayMultiplier"] ?? 1;
  const o = p["x-espos-displayOffset"] ?? 0;
  if (typeof v !== "number" || (m === 1 && o === 0)) return v;
  return v * m + o;
}
export function fromDisplay(p: JsonSchemaProp, v: unknown): unknown {
  const m = p["x-espos-displayMultiplier"] ?? 1;
  const o = p["x-espos-displayOffset"] ?? 0;
  if (typeof v !== "number" || (m === 1 && o === 0)) return v;
  const stored = (v - o) / m;
  // An integer key must stay an integer after the round trip, or the device
  // rejects the document it just served.
  return p.type === "integer" ? Math.round(stored) : stored;
}
export interface ConfigSchema { properties: Record<string, JsonSchemaNs> }
export interface PutResult { changed: string[]; restart_required: boolean }

// ---- live state over SSE
type Listener = () => void;
class Store<T> {
  private v: T | undefined;
  private ls = new Set<Listener>();
  get value(): T | undefined { return this.v; }
  set(v: T) { this.v = v; for (const l of this.ls) l(); }
  subscribe(l: Listener) { this.ls.add(l); return () => { this.ls.delete(l); }; }
}
export const wifiStore = new Store<WifiStatus>();
export const scanStore = new Store<ScanDoc>();
export const skStore = new Store<SkStatus>();
export const skServersStore = new Store<SkServersDoc>();
export const skWsStore = new Store<SkWs>();
export const skTlsStore = new Store<SkTls>();
export const logsSeqStore = new Store<number>();
export const otaStore = new Store<OtaStatus>();
export const bleStore = new Store<BleStatus>();
export const configChangeStore = new Store<{ ns: string; key: string; n: number }>();
export const linkStore = new Store<"connecting" | "open" | "lost">();

// ---- authentication (docs/security.md)
//   open          no key configured: the API answers everyone, no login page
//   ok            a key is set and this browser holds a credential
//   login         a key is set and this browser has none (or it expired)
//   unconfigured  the build requires a key and none is set: nothing works from
//                 here; the key is set from the setup portal
export type AuthState = "open" | "ok" | "login" | "unconfigured";
export const authStore = new Store<AuthState>();

export function useStore<T>(s: Store<T>): T | undefined {
  const [, tick] = useState(0);
  useEffect(() => {
    const unsub = s.subscribe(() => tick((n) => n + 1));
    // Re-read on subscribe. Effects run after paint, so a store written
    // between render and here -- bootstrapAuth() resolving fast, an SSE
    // snapshot arriving at once -- would have notified nobody, and nothing
    // would ever notify again: the component keeps rendering the value it
    // first read, for ever.
    //
    // That is not hypothetical. Over WiFi /auth/status answers in about 8 ms
    // and wins this race, so the app renders; on the setup portal it is
    // slower, loses, and the page sits on "Loading..." until reloaded.
    if (s.value !== undefined) tick((n) => n + 1);
    return unsub;
  }, [s]);
  return s.value;
}

/** Ask the device where we stand, once at startup and whenever the stream dies. */
export async function bootstrapAuth(): Promise<void> {
  try {
    const st = await get<AuthStatus>("/auth/status");
    authStore.set(!st.required ? "open" : st.authenticated ? "ok" : st.configured ? "login" : "unconfigured");
  } catch (e) {
    // A firmware without /auth/status has no authentication either; anything
    // else (device unreachable) is the stream's problem, not the login page's.
    if (e instanceof ApiFailure && e.status === 404) authStore.set("open");
    else if (authStore.value === undefined) authStore.set("open");
  }
}

/** Exchange the key for a session. Throws ApiFailure (401 wrong, 429 throttled). */
export async function login(key: string): Promise<void> {
  if (DEV_KEY_MODE) setDevKey(key);
  try {
    // The cookie is what EventSource can send; the Bearer (dev mode) is what
    // gets writes past the Origin check behind a proxy. Both, then.
    await post("/auth/login", { key });
  } catch (e) {
    setDevKey(null);
    throw e;
  }
  resetEvents();
  authStore.set("ok");
}

export async function logout(): Promise<void> {
  try { await post("/auth/logout"); } catch { /* the session may already be gone; the outcome is the same */ }
  setDevKey(null);
  closeEvents();
  authStore.set("login");
}

let es: EventSource | null = null;
let changes = 0;
let retry: ReturnType<typeof setTimeout> | null = null;
export function connectEvents() {
  if (es) return;
  linkStore.set("connecting");
  es = new EventSource(BASE + "/events", { withCredentials: ABSOLUTE });
  const on = <T,>(name: string, store: Store<T>, map?: (d: T) => void) =>
    es!.addEventListener(name, (e: MessageEvent<string>) => {
      const d = JSON.parse(e.data) as T;
      store.set(d);
      map?.(d);
    });
  es.onopen = () => linkStore.set("open");
  es.onerror = () => {
    linkStore.set("lost");
    // A dropped connection reconnects by itself (retry: 3000). A refused one
    // — 401 after the session expired or the device rebooted — does not: the
    // browser closes the stream for good. Find out which, then reopen.
    if (es && es.readyState === EventSource.CLOSED) {
      es = null;
      if (retry) clearTimeout(retry);
      retry = setTimeout(() => {
        retry = null;
        void bootstrapAuth().then(() => { const a = authStore.value; if (a === "ok" || a === "open") connectEvents(); });
      }, 3000);
    }
  };
  on("wifi", wifiStore);
  on("wifi_scan", scanStore);
  on("sk", skStore, (d) => { if (d.ws) skWsStore.set(d.ws); });
  on("sk_servers", skServersStore);
  on("sk_ws", skWsStore);
  on("sk_tls", skTlsStore);
  on("ota", otaStore);
  on("ble", bleStore);
  es.addEventListener("logs", (e: MessageEvent<string>) =>
    logsSeqStore.set((JSON.parse(e.data) as { next: number }).next));
  es.addEventListener("config", (e: MessageEvent<string>) => {
    const d = JSON.parse(e.data) as { ns: string; key: string };
    configChangeStore.set({ ...d, n: ++changes });
  });
}
function closeEvents() {
  if (retry) { clearTimeout(retry); retry = null; }
  es?.close();
  es = null;
}
/** Drop the stream and open a fresh one — after a login, so it carries the new cookie. */
export function resetEvents() {
  closeEvents();
  connectEvents();
}

// ---- helpers
export function fmtDuration(s: number | undefined | null): string {
  if (s === undefined || s === null) return "–";
  if (s < 60) return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m ${s % 60}s`;
  if (s < 86400) return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
  return `${Math.floor(s / 86400)}d ${Math.floor((s % 86400) / 3600)}h`;
}
export function fmtBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  return `${(n / 1024 / 1024).toFixed(2)} MiB`;
}
export function errText(e: unknown): string {
  if (e instanceof ApiFailure) return e.body ? `${e.body.message}${e.body.path ? ` (${e.body.path})` : ""}` : `HTTP ${e.status}`;
  return e instanceof Error ? e.message : String(e);
}
/** A random API key: 20 characters from an alphabet without look-alikes (~114 bits). */
export function randomKey(n = 20): string {
  const alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
  const buf = new Uint32Array(n);
  crypto.getRandomValues(buf);
  return Array.from(buf, (x) => alphabet[x % alphabet.length]!).join("");
}
