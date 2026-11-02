// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
// BLE gateway status. The component has had /api/v1/ble/status since it
// landed, but no page — so the one number that says whether a gateway is
// keeping up (adv_dropped) was only visible with curl.
import { get, useStore, bleStore, errText, type BleStatus, type ConfigDoc, type PutResult } from "../api";
import { useEffect, useState } from "preact/hooks";
import { Badge, Row, Msg } from "../app";
import { put } from "../api";

export function BlePage() {
  const ble = useStore(bleStore);
  const [msg, setMsg] = useState("");
  const [ok, setOk] = useState("");
  const [cfg, setCfg] = useState<Record<string, unknown> | null>(null);

  const load = () => get<ConfigDoc>("/config?ns=ble").then(
    (d) => setCfg(d["ble"] ?? {}),
    (e: unknown) => setMsg(errText(e)));

  useEffect(() => {
    void load();
    if (!bleStore.value) void get<BleStatus>("/ble/status").then((d) => bleStore.set(d), () => undefined);
  }, []);

  async function save(patch: Record<string, unknown>, done: string) {
    setMsg(""); setOk("");
    try {
      const r = await put<PutResult>("/config", { ble: patch });
      setOk(r.changed.length ? done : "No change.");
      await load();
    } catch (e) { setMsg(errText(e)); }
  }

  const enabled = (cfg?.["enabled"] as boolean) ?? false;
  // A gap between what the radio saw and what the gateway was handed means
  // the intake callback is being starved; a growing dropped count means
  // advertisements arrive faster than they reach the server: the link is
  // down, POSTs fail, or the server is slow (a full batch goes out without
  // waiting for the interval). Both are the operator's
  // business, which is why they are the first thing on the page.
  const starved = ble ? ble.scan_hits - ble.adv_received : 0;

  return (
    <>
      <h1>BLE gateway</h1>
      <Msg text={msg} />
      <Msg text={ok} kind="ok" />
      <div class="grid">
        <section class="card">
          <h2>
            Radio{" "}
            {ble && (ble.scanning ? <Badge kind="ok">scanning</Badge>
              : ble.enabled ? <Badge kind="warn">idle</Badge>
                : <Badge kind="muted">off</Badge>)}
          </h2>
          {ble ? (
            <>
              <Row k="Adapter">{ble.mac || "—"}</Row>
              <Row k="Advertisements">{ble.adv_received} received <span class="muted">· {ble.scan_hits} seen by the radio</span></Row>
              {starved > 0 && <Msg kind="warn" text={`${starved} advertisements never reached the gateway — the intake callback is being starved.`} />}
              <Row k="Buffer">{ble.adv_pending} pending{ble.adv_dropped > 0 && <> · <span class="bad">{ble.adv_dropped} dropped</span></>}</Row>
              {ble.adv_dropped > 0 && <Msg kind="warn" text="Advertisements were dropped since the gateway started: the buffer was full, or busy when they came in. If the count keeps rising, check the link to the server and the failures; a bigger buffer only bridges a short outage." />}
            </>
          ) : <p class="muted">No status yet.</p>}
        </section>

        <section class="card">
          <h2>To the server {ble && (ble.ws_connected ? <Badge kind="ok">connected</Badge> : <Badge kind="bad">disconnected</Badge>)}</h2>
          {ble && (
            <>
              <Row k="Posted">{ble.adv_posted} advertisements <span class="muted">· {ble.post_success} POSTs</span></Row>
              <Row k="Failures">{ble.post_fail > 0 ? <span class="bad">{ble.post_fail}</span> : "0"}</Row>
              <Row k="GATT sessions">{ble.gatt_sessions} / {ble.gatt_max}</Row>
            </>
          )}
          <p class="muted">
            Sessions are opened by the server over the control WebSocket; there is
            nothing to start from here.
          </p>
        </section>

        <section class="card">
          <h2>Settings</h2>
          {cfg ? (
            <>
              <Row k="Gateway">
                <label>
                  <input type="checkbox" checked={enabled}
                    onChange={(e) => void save({ enabled: (e.target as HTMLInputElement).checked },
                      (e.target as HTMLInputElement).checked ? "Gateway enabled." : "Gateway disabled.")} />
                  {" "}enabled
                </label>
              </Row>
              <p class="muted">
                The rest of the <code>ble</code> namespace — scan window, POST
                interval, buffer depth — is on the Config page.
              </p>
            </>
          ) : <p class="muted">Loading…</p>}
        </section>
      </div>
    </>
  );
}
