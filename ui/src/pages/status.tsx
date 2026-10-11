// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
import { useEffect, useState } from "preact/hooks";
import { get, post, del, useStore, wifiStore, skStore, skWsStore, fmtDuration, fmtBytes, errText, type SystemInfo, type Coredump, type AuthStatus } from "../api";
import { Badge, Row, Msg, useAsync, navigate } from "../app";

export function StatusPage() {
  const wifi = useStore(wifiStore);
  const sk = useStore(skStore);
  const ws = useStore(skWsStore) ?? sk?.ws;
  const info = useAsync(() => get<SystemInfo>("/system/info"));
  const [msg, setMsg] = useState("");
  const [tick, setTick] = useState(0);
  /* Empty deps on purpose: `info` comes from useAsync and is a new object every
   * render, so listing it would clear and recreate this interval on each one --
   * and a 10 s poll that restarts more often than it fires never fires. */
  // eslint-disable-next-line react-hooks/exhaustive-deps
  useEffect(() => { const t = setInterval(() => { info.reload(); setTick((n) => n + 1); }, 10000); return () => clearInterval(t); }, []);
  void tick;
  const i = info.data;
  const wifiKind = wifi?.state === "connected" ? "ok" : wifi?.state === "backoff" || (wifi?.reason.code ?? 0) ? "bad" : "warn";
  const tokKind = sk?.token.state === "approved" ? "ok" : sk?.token.state === "denied" || sk?.token.state === "error" ? "bad" : "warn";

  async function reboot() {
    if (!confirm("Reboot the device now?")) return;
    try { await post("/system/reboot"); setMsg("Rebooting… the page reconnects by itself."); } catch (e) { setMsg(errText(e)); }
  }
  async function factoryReset() {
    if (!confirm("Erase ALL settings (WiFi, SignalK token, config) and reboot into the setup portal?")) return;
    try { await post("/system/factory-reset"); setMsg("Factory reset requested — the device reboots into the setup portal."); } catch (e) { setMsg(errText(e)); }
  }

  return (
    <>
      <h1>Status</h1>
      <Msg text={msg} kind="warn" />
      <div class="grid">
        <section class="card">
          <h2>WiFi <Badge kind={wifiKind}>{wifi?.state ?? "…"}</Badge></h2>
          {wifi && (
            <>
              {wifi.ssid && <Row k="Network">{wifi.ssid}{wifi.rssi !== undefined && <span class="muted"> · {wifi.rssi} dBm · ch {wifi.channel}</span>}</Row>}
              {wifi.ip && <Row k="IP">{wifi.ip} <span class="muted">via {wifi.gateway}</span></Row>}
              <Row k="Hostname">{wifi.hostname}</Row>
              {wifi.reason.code !== 0 && <Row k="Last reason"><span class="mono">{wifi.reason.text} [{wifi.reason.code}]</span></Row>}
              {wifi.backoff_ms !== undefined && <Row k="Retry in">{Math.ceil(wifi.backoff_ms / 1000)} s</Row>}
              {wifi.portal.active && <Row k="Setup portal"><Badge kind="warn">on</Badge> {wifi.portal.ssid} {wifi.portal.ip && <span class="muted">· {wifi.portal.ip}</span>}</Row>}
              <div class="row"><a href="/wifi" onClick={(e) => { e.preventDefault(); navigate("/wifi"); }}>WiFi setup →</a></div>
            </>
          )}
        </section>

        <section class="card">
          <h2>SignalK <Badge kind={tokKind}>{sk?.token.state ?? "…"}</Badge></h2>
          {sk && (
            <>
              <Row k="Server">{sk.server.source === "none" ? <span class="muted">none selected</span> : <>{sk.server.name || sk.server.host} <span class="muted">· {sk.server.host}:{sk.server.port} · {sk.server.source}</span></>}</Row>
              <Row k="Stream">
                {ws ? (
                  ws.connected ? <><Badge kind="ok">connected</Badge> <span class="muted">{fmtDuration(ws.connected_s)} · {ws.sent} sent</span></>
                    : ws.enabled ? <><Badge kind="warn">offline</Badge> <span class="muted">{ws.buffered} buffered{ws.next_retry_s ? ` · retry in ${ws.next_retry_s}s` : ""}</span></>
                      : <Badge kind="muted">disabled</Badge>
                ) : "–"}
              </Row>
              {ws?.last_error && <Row k="Stream error"><span class="mono">{ws.last_error}</span></Row>}
              {sk.token.last_error && <Row k="Token error"><span class="mono">{sk.token.last_error}</span></Row>}
              <div class="row"><a href="/signalk" onClick={(e) => { e.preventDefault(); navigate("/signalk"); }}>SignalK setup →</a></div>
            </>
          )}
        </section>

        <section class="card">
          <h2>Device {i && <Badge kind={i.reset_reason === "panic" || i.reset_reason.includes("wdt") ? "bad" : "muted"}>{i.reset_reason}</Badge>}</h2>
          {info.error && <Msg text={info.error} />}
          {i && (
            <>
              <Row k="Firmware">{i.app} {i.version}</Row>
              <Row k="ESP-IDF">{i.idf_version}</Row>
              <Row k="Chip">{i.chip} <span class="muted">rev {i.chip_revision} · {i.cores} core{i.cores > 1 ? "s" : ""}</span></Row>
              <Row k="Uptime">{fmtDuration(i.uptime_s)}</Row>
              <TimeRow t={(i as SystemInfo & { time?: TimeInfo }).time} />
              <Row k="Free heap">{fmtBytes(i.free_heap)} <span class="muted">min {fmtBytes(i.min_free_heap)}</span></Row>
              {i.config_storage_reset && <Msg text="Config storage was reset at boot — all values are defaults." kind="warn" />}
              {(i.ui ? i.ui === "placeholder" : i.ui_storage === false) && <Msg text="No web UI in the firmware or the storage partition; serving the placeholder page." kind="warn" />}
              <div class="row" style="margin-top:.75rem">
                <button onClick={reboot}>Reboot</button>
                <button class="danger" onClick={factoryReset}>Factory reset…</button>
              </div>
            </>
          )}
        </section>

        <HardwareCard i={i} />
        <AccessCard />
        <CoredumpCard />
      </div>
    </>
  );
}

// Which board is this? — the question with a drawer of dev boards, and until
// now a curl away. Everything here is measured (esp_chip_info's feature bits,
// which were already being fetched and thrown away, plus one call each for
// flash and the RAM totals) except `board`, which only the firmware can know
// and therefore only shows when it said.
//
// Absent on a device that predates the field, so the card hides rather than
// rendering a row of dashes.
function HardwareCard({ i }: { i: SystemInfo | undefined }) {
  const hw = i?.hardware;
  if (!hw) return null;
  const psram = hw.ram_psram_bytes ?? 0;
  return (
    <section class="card">
      <h2>Hardware</h2>
      {hw.board && <Row k="Board">{hw.board}</Row>}
      {hw.mac && <Row k="MAC"><span class="mono">{hw.mac}</span></Row>}
      {hw.cpu_mhz !== undefined && <Row k="CPU">{hw.cpu_mhz} MHz</Row>}
      {hw.flash_bytes !== undefined && (
        <Row k="Flash">
          {fmtBytes(hw.flash_bytes)}
          {/* The chip next to what the partition table reaches: a 32 MB panel
              running a 16 MB layout is room only a USB flash can claim. */}
          {hw.flash_partitioned_bytes !== undefined && (
            <span class="muted"> · {fmtBytes(hw.flash_partitioned_bytes)} partitioned</span>
          )}
        </Row>
      )}
      {hw.ram_internal_bytes !== undefined && (
        <Row k="RAM">
          {fmtBytes(hw.ram_internal_bytes)} internal
          {/* No PSRAM at all is worth saying rather than omitting: on a P4 it
              is the difference between a board that can hold a framebuffer and
              one that cannot. */}
          {psram > 0
            ? <span class="muted"> · {fmtBytes(psram)} PSRAM</span>
            : <span class="muted"> · no PSRAM</span>}
        </Row>
      )}
      {hw.features && hw.features.length > 0 && (
        <Row k="Radios">
          {hw.features.filter((f) => !f.startsWith("embedded-")).join(", ") || <span class="muted">none</span>}
        </Row>
      )}
    </section>
  );
}

// The wall clock, and where the device got it. Worth a line on the status page
// because an unsynced clock is the reason a delta arrives without a timestamp
// and the server stamps it on receipt — a silent, confusing data problem
// otherwise (docs/time.md). `time` is absent only against a firmware older
// than this field, so the row simply does not render there.
// Declared here rather than on SystemInfo in api.ts: this is the only page
// that reads the clock, and api.ts belongs to another change in flight. Move
// it onto SystemInfo when a second consumer appears.
interface TimeInfo { synced: boolean; source: string; now: number }

function TimeRow({ t }: { t: TimeInfo | undefined }) {
  if (!t) return null;
  const kind = t.synced ? "ok" : t.source === "none" ? "warn" : "muted";
  const label = t.source === "none" ? "not set" : t.synced ? t.source : `${t.source} (stale)`;
  return (
    <Row k="Clock">
      <Badge kind={kind}>{label}</Badge>{" "}
      {t.now > 0
        ? <span class="muted">{new Date(t.now).toISOString().replace("T", " ").slice(0, 19)} UTC</span>
        : <span class="muted">deltas are stamped by the server on arrival</span>}
    </Row>
  );
}

// Who may talk to this device (docs/security.md): open, or behind the API
// key — and how this very page got in.
function AccessCard() {
  const st = useAsync(() => get<AuthStatus>("/auth/status"));
  const a = st.data;
  const kind = !a ? "muted" : !a.required ? "warn" : a.configured ? "ok" : "bad";
  const label = !a ? "…" : !a.required ? "open" : a.configured ? "API key" : "key missing";
  const session = a?.method === "cookie" ? "logged in (session cookie)"
    : a?.method === "bearer" ? "API key (Bearer header)"
      : a?.method === "portal" ? "setup portal — no key needed on this network"
        : a?.required ? "not authenticated" : "—";
  return (
    <section class="card">
      <h2>Access <Badge kind={kind}>{label}</Badge></h2>
      {st.error && <Msg text={st.error} />}
      {a && (
        <>
          <Row k="Authentication">
            {!a.required ? <>Off — anyone on the network can read and change settings, reboot, or reset the device.</>
              : a.configured ? <>On — the REST API and this UI need the API key (<span class="mono">httpd.api_key</span>).</>
                : <>Required by this firmware, but no key is set: the API refuses everything but the setup portal.</>}
          </Row>
          <Row k="This session">{session}</Row>
          <div class="row">
            <a href="/config#httpd" onClick={(e) => { e.preventDefault(); navigate("/config#httpd"); }}>{a.configured ? "Change the key →" : "Set an API key →"}</a>
          </div>
        </>
      )}
    </section>
  );
}

function CoredumpCard() {
  const [dump, setDump] = useState<Coredump | null | undefined>(undefined);
  const [msg, setMsg] = useState("");
  const load = () => get<Coredump>("/system/coredump").then(setDump, (e: unknown) => {
    if (e instanceof Error && "status" in e && (e as { status: number }).status === 404) setDump(null); else setMsg(errText(e));
  });
  useEffect(() => { void load(); }, []);
  if (dump === undefined) return null;
  return (
    <section class="card">
      <h2>Last crash {dump ? <Badge kind="bad">core dump</Badge> : <Badge kind="ok">none</Badge>}</h2>
      <Msg text={msg} />
      {dump ? (
        <>
          <Row k="Task">{dump.task ?? "?"}</Row>
          <Row k="PC"><span class="mono">{dump.pc}</span></Row>
          {dump.exc_cause !== undefined && <Row k="Exception">cause {dump.exc_cause} <span class="mono muted">@ {dump.exc_vaddr}</span></Row>}
          {dump.mcause !== undefined && <Row k="Trap">mcause {dump.mcause} <span class="mono muted">mtval {dump.mtval} · ra {dump.ra}</span></Row>}
          {dump.backtrace && <Row k="Backtrace"><span class="mono small">{dump.backtrace.join(" ")}{dump.backtrace_corrupted ? " (corrupted)" : ""}</span></Row>}
          <Row k="Image">{fmtBytes(dump.size)} {dump.valid ? "" : "(checksum invalid)"} <span class="muted">· ELF {dump.app_elf_sha256}</span></Row>
          {dump.summary_error && <Row k="Summary"><span class="mono">{dump.summary_error}</span></Row>}
          <div class="row" style="margin-top:.75rem">
            <a class="mono small" href="/api/v1/system/coredump/raw" download="coredump.bin">download raw</a>
            <button onClick={async () => { try { await del("/system/coredump"); setDump(null); } catch (e) { setMsg(errText(e)); } }}>Erase</button>
          </div>
          <p class="muted small">Decode with <code>espcoredump.py info_corefile -c coredump.bin -t raw build/espos.elf</code> from the matching build.</p>
        </>
      ) : <p class="muted small">No core dump stored. After a panic the summary appears here.</p>}
    </section>
  );
}
