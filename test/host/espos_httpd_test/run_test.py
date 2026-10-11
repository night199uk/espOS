#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
"""
Drives build/espos_httpd_test.elf through the REST API contract in docs/rest-api.md.
Standard library only. Exit code 0 == all checks passed.
"""
import base64
import gzip
import http.client
import itertools
import json
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ELF = os.path.join(HERE, "build", "espos_httpd_test.elf")
PORT = 0  # chosen per harness instance, see pick_port()
SENTINEL = "********"


def free_port_check(port):
    with socket.socket() as s:
        return s.connect_ex(("127.0.0.1", port)) != 0


def pick_port():
    """A fresh ephemeral port per harness instance. esp_http_server does not
    set SO_REUSEADDR, so re-binding the previous port right after a restart
    can fail while old connections sit in TIME_WAIT."""
    forced = os.environ.get("ESPOS_TEST_PORT")
    if forced:
        return int(forced)
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Harness:
    def __init__(self, fresh=True, extra_env=None):
        global PORT
        PORT = pick_port()
        if not free_port_check(PORT):
            raise RuntimeError(f"port {PORT} is already in use; set ESPOS_TEST_PORT to a free port")
        env = dict(os.environ, ESPOS_TEST_PORT=str(PORT))
        env.update(extra_env or {})
        if fresh:
            env["ESPOS_TEST_FRESH"] = "1"
        self.proc = subprocess.Popen([ELF], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, env=env, cwd=HERE)
        deadline = time.time() + 20
        self.log = []
        ready = False
        # Ready ONLY when the harness says so.
        #
        # An open port is not readiness. espos_httpd_start() calls httpd_start()
        # — which binds and listens — and registers its URI handlers afterwards,
        # static (the one serving "/") second to last. main.c writes
        # ESPOS_HARNESS_READY after all of that, so the line is the ground truth
        # and the port is merely the first step towards it.
        #
        # Accepting the port here raced: under load the window between the bind
        # and the last handler widened, the first request of a fresh harness hit
        # a server whose route table was incomplete, and the reply was a
        # perfectly well-formed 404 rather than a connection error. That looks
        # exactly like a routing bug, which is an expensive thing to chase.
        # Observed as UiAndLogsTests.test_01 failing "404 != 200" on "/" while
        # every endpoint registered before static passed in the same run.
        while time.time() < deadline:
            if self.proc.poll() is not None:
                break
            r, _, _ = select.select([self.proc.stdout], [], [], 0.2)
            if r:
                line = self.proc.stdout.readline()
                if not line:
                    break
                self.log.append(line)
                if "ESPOS_HARNESS_READY" in line:
                    ready = True
                    break
        if not ready:
            # Distinguish "never started" from "started but never announced" —
            # the second means main.c changed, not that the machine was slow.
            hint = ""
            if self.proc.poll() is None and not free_port_check(PORT):
                hint = (f" (port {PORT} is open but ESPOS_HARNESS_READY never arrived: "
                        "does main.c still write it after espos_httpd_start()?)")
            self.stop()
            raise RuntimeError(f"harness did not become ready{hint}:\n" + "".join(self.log))
        # Keep draining stdout so the harness never blocks on a full pipe and
        # tests can grep self.log.
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()
        for _ in range(100):
            if not free_port_check(PORT):
                return
            time.sleep(0.05)
        self.stop()
        raise RuntimeError("harness port never opened:\n" + "".join(self.log))

    def _drain(self):
        try:
            for line in self.proc.stdout:
                self.log.append(line)
        except (ValueError, OSError):
            pass

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        try:
            self.proc.stdout.close()
        except Exception:
            pass
        self.proc.wait()

    def wait_exit(self, timeout=5.0):
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None
        self.proc.stdout.close()
        return code


def req(method, path, body=None, headers=None, content_type="application/json"):
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
    data = None
    hdrs = dict(headers or {})
    if body is not None:
        data = body if isinstance(body, (bytes, str)) else json.dumps(body)
    if method in ("PUT", "POST") and content_type:
        hdrs.setdefault("Content-Type", content_type)
    conn.request(method, path, body=data, headers=hdrs)
    r = conn.getresponse()
    raw = r.read()
    conn.close()
    parsed = None
    ctype = r.getheader("Content-Type", "")
    if "json" in ctype and raw:
        parsed = json.loads(raw)
    return r.status, dict(r.getheaders()), raw, parsed


class ApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness(fresh=True)

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()

    # ---- schema
    def test_01_schema(self):
        st, hd, raw, js = req("GET", "/api/v1/config/schema")
        self.assertEqual(st, 200)
        self.assertEqual(hd.get("Content-Type"), "application/schema+json")
        etag = hd.get("ETag")
        self.assertTrue(etag and etag.startswith('"') and etag.endswith('"'))
        self.assertEqual(js["$schema"], "https://json-schema.org/draft/2020-12/schema")
        self.assertIn("app", js["properties"])
        self.assertIn("httpd", js["properties"])
        self.assertEqual(js["properties"]["httpd"]["properties"]["port"]["default"], 80)
        self.assertTrue(js["properties"]["app"]["properties"]["api_key"]["writeOnly"])
        # conditional GET
        st2, hd2, raw2, _ = req("GET", "/api/v1/config/schema", headers={"If-None-Match": etag})
        self.assertEqual(st2, 304)
        self.assertEqual(raw2, b"")
        st3, _, _, _ = req("GET", "/api/v1/config/schema", headers={"If-None-Match": '"stale"'})
        self.assertEqual(st3, 200)

    # ---- system
    def test_02_system_info(self):
        st, hd, raw, js = req("GET", "/api/v1/system/info")
        self.assertEqual(st, 200)
        self.assertEqual(hd.get("Content-Type"), "application/json")
        for k in ("app", "version", "idf_version", "chip", "uptime_s", "free_heap", "min_free_heap",
                  "free_internal", "min_internal_free", "reset_reason",
                  "config_storage_reset", "schema_etag"):
            self.assertIn(k, js, k)
        # The whole "hardware" object is absent on the host while the
        # co-processor hook has nothing to report: nothing else in it is
        # meaningful there and an object full of zeros would be worse than its
        # absence (add_hardware() in api_system.c). The harness hook stays
        # silent until a test sets a report; test_02b covers the other side.
        self.assertNotIn("hardware", js)
        # free_internal/min_internal_free are the live and low-water halves of one
        # measurement, and min_internal_free is what memoryTrough is raised from;
        # until they were added a device could not be asked for the number behind
        # that warning, nor whether internal RAM had recovered.
        # Deliberately NOT compared with free_heap/min_free_heap: the
        # two ask different capability sets (INTERNAL vs DEFAULT), those bits are
        # independent flags rather than a hierarchy, and
        # heap_caps_get_minimum_free_size() sums each matching region's own
        # minimum -- so neither figure is reliably the larger and an ordering
        # assertion ACROSS the pairs would encode a guarantee ESP-IDF does not make.
        #
        # WITHIN a pair it is provable, so it is asserted: the low-water mark and the
        # live figure sum over the same region set, and each region's minimum is at
        # most its current free size, so min <= live term by term and therefore in
        # total. (On the linux target every heap query returns one constant, so these
        # hold with equality here -- they would still catch a swapped pair.)
        self.assertIsInstance(js["min_internal_free"], int)
        self.assertIsInstance(js["free_internal"], int)
        self.assertLessEqual(js["min_free_heap"], js["free_heap"])
        self.assertLessEqual(js["min_internal_free"], js["free_internal"])
        self.assertEqual(js["app"], "espos_httpd_test")
        self.assertIsInstance(js["uptime_s"], int)
        self.assertFalse(js["config_storage_reset"])
        # "time" is always present, whether or not the firmware has espos_time.
        # This harness does not link it, so it exercises the weak default: the
        # honest "this device has not been told the time" a client must handle.
        self.assertIn("time", js)
        self.assertEqual(set(js["time"]), {"synced", "source", "now"})
        self.assertFalse(js["time"]["synced"])
        self.assertEqual(js["time"]["source"], "none")
        self.assertEqual(js["time"]["now"], 0)
        # schema_etag matches the ETag header served with the schema
        _, hd2, _, _ = req("GET", "/api/v1/config/schema")
        self.assertEqual(hd2.get("ETag"), '"%s"' % js["schema_etag"])

    # ---- config GET
    def test_02b_system_info_coprocessor(self):
        # The harness's main.c defines espos_httpd_coprocessor_hook strongly,
        # overriding the weak stub in espos_httpd. A report arriving here at all
        # proves the override linked -- a hook whose object the linker never
        # pulled in resolves to the stub in silence, which is how espos_time's
        # wallclock hook shipped broken (#49). The production override in
        # hosted_watchdog.c still needs `nm` on a device image (T, not W).
        def coprocessor():
            st, _, _, js = req("GET", "/api/v1/system/info")
            self.assertEqual(st, 200)
            # On the host the co-processor is the only thing "hardware" holds.
            self.assertEqual(set(js.get("hardware", {})), {"coprocessor"}, js.get("hardware"))
            return js["hardware"]["coprocessor"]

        try:
            st, _, _, js = req("POST", "/__harness/coproc", {
                "version": "2.12.3", "host_version": "3.0.9", "target": "esp32c6", "stale": True})
            self.assertEqual(st, 200, js)
            self.assertEqual(coprocessor(), {
                "version": "2.12.3", "host_version": "3.0.9", "target": "esp32c6", "stale": True})

            # An unknown chip id leaves target empty, and the field is then
            # omitted rather than sent blank; "0.0.0" is a co-processor that
            # named no version, which is still a report.
            st, _, _, js = req("POST", "/__harness/coproc", {
                "version": "0.0.0", "host_version": "3.0.9", "target": "", "stale": False})
            self.assertEqual(st, 200, js)
            co = coprocessor()
            self.assertEqual(co, {"version": "0.0.0", "host_version": "3.0.9", "stale": False})
            self.assertIs(co["stale"], False)
        finally:
            req("DELETE", "/__harness/coproc")

        # Back to nothing to report: no "hardware" at all, not an empty one.
        _, _, _, js = req("GET", "/api/v1/system/info")
        self.assertNotIn("hardware", js)

    def test_03_config_get_defaults(self):
        st, hd, raw, js = req("GET", "/api/v1/config")
        self.assertEqual(st, 200)
        self.assertEqual(hd.get("Cache-Control"), "no-store")
        self.assertTrue({"app", "httpd", "wifi"} <= set(js.keys()))
        app = js["app"]
        self.assertEqual(app["label"], "espOS device")
        self.assertTrue(app["enabled"])
        self.assertEqual(app["interval_ms"], 1000)
        self.assertEqual(app["scale"], 1.0)
        self.assertEqual(app["mode"], "auto")
        self.assertEqual(app["api_key"], "")       # unset secret is "", not the sentinel
        self.assertEqual(app["cal_table"], "")
        self.assertEqual(js["httpd"]["port"], PORT)  # harness overrode it
        # namespace filter
        st, _, _, js2 = req("GET", "/api/v1/config?ns=app")
        self.assertEqual(st, 200)
        self.assertEqual(list(js2.keys()), ["app"])
        st, _, _, js3 = req("GET", "/api/v1/config?ns=nope")
        self.assertEqual(st, 404)
        self.assertEqual(js3["error"], "unknown_namespace")

    # ---- config PUT
    def test_04_config_put_roundtrip(self):
        body = {"app": {"label": "boat", "interval_ms": 250, "scale": 0.1, "mode": "manual",
                        "api_key": "s3cret", "cal_table": "AQIDBA==", "enabled": False}}
        st, _, _, js = req("PUT", "/api/v1/config", body)
        self.assertEqual(st, 200, js)
        self.assertEqual(sorted(js["changed"]),
                         sorted(["app.label", "app.interval_ms", "app.scale", "app.mode",
                                 "app.api_key", "app.cal_table", "app.enabled"]))
        self.assertFalse(js["restart_required"])
        st, _, raw, js = req("GET", "/api/v1/config?ns=app")
        self.assertEqual(js["app"]["label"], "boat")
        self.assertEqual(js["app"]["interval_ms"], 250)
        self.assertEqual(js["app"]["scale"], 0.1)
        self.assertIn(b'"scale":0.1,', raw)  # canonical float rendering
        self.assertEqual(js["app"]["mode"], "manual")
        self.assertEqual(js["app"]["api_key"], SENTINEL)
        self.assertEqual(js["app"]["cal_table"], "AQIDBA==")
        self.assertFalse(js["app"]["enabled"])
        # idempotent re-PUT of what we read back: sentinel ignored, nothing changes
        st, _, _, js2 = req("PUT", "/api/v1/config", js)
        self.assertEqual(st, 200)
        self.assertEqual(js2["changed"], [])
        # null resets, restart_required surfaces for httpd.port
        st, _, _, js3 = req("PUT", "/api/v1/config", {"app": {"label": None}, "httpd": {"port": PORT + 1}})
        self.assertEqual(st, 200)
        self.assertEqual(sorted(js3["changed"]), ["app.label", "httpd.port"])
        self.assertTrue(js3["restart_required"])
        st, _, _, js4 = req("GET", "/api/v1/config")
        self.assertEqual(js4["app"]["label"], "espOS device")
        self.assertEqual(js4["httpd"]["port"], PORT + 1)
        # put it back so a restart of the harness stays reachable
        req("PUT", "/api/v1/config", {"httpd": {"port": PORT}})

    def test_05_config_put_validation(self):
        cases = [
            ({"app": {"interval_ms": 5}}, "app.interval_ms", "out of range"),
            ({"app": {"interval_ms": "5"}}, "app.interval_ms", "expected integer"),
            ({"app": {"mode": "off"}}, "app.mode", "not an allowed"),
            ({"app": {"mode": "automatic"}}, "app.mode", "longer than"),  # enum maxLength = longest value
            ({"app": {"cal_table": "###"}}, "app.cal_table", "invalid base64"),
            ({"app": {"nope": 1}}, "app.nope", "unknown key"),
            ({"nope": {}}, "nope", "unknown namespace"),
            ({"app": 3}, "app", "expected object"),
            ({"app": {"scale": 1000}}, "app.scale", "above maximum"),
        ]
        for body, path, msg in cases:
            st, _, _, js = req("PUT", "/api/v1/config", body)
            self.assertEqual(st, 400, (body, js))
            self.assertEqual(js["error"], "validation")
            self.assertEqual(js["path"], path)
            self.assertIn(msg, js["message"])
        # malformed JSON
        st, _, _, js = req("PUT", "/api/v1/config", "{not json")
        self.assertEqual(st, 400)
        self.assertEqual(js["error"], "validation")
        self.assertIn("malformed", js["message"])
        # all-or-nothing: valid + invalid in one document writes nothing
        st, _, _, _ = req("PUT", "/api/v1/config", {"app": {"label": "partial", "interval_ms": 1}})
        self.assertEqual(st, 400)
        _, _, _, js = req("GET", "/api/v1/config?ns=app")
        self.assertNotEqual(js["app"]["label"], "partial")

    def test_06_body_too_large(self):
        big = {"app": {"label": "x" * 5000}}
        st, _, _, js = req("PUT", "/api/v1/config", big)
        self.assertEqual(st, 413)
        self.assertEqual(js["error"], "too_large")

    def test_07_not_found_and_methods(self):
        st, _, _, js = req("GET", "/api/v1/nope")
        self.assertEqual(st, 404)
        self.assertEqual(js["error"], "not_found")
        # wrong method on a known resource: still a JSON error body
        st, hd, _, js = req("DELETE", "/api/v1/config")
        self.assertIn(st, (404, 405))
        self.assertEqual(hd.get("Content-Type"), "application/json")
        self.assertIn(js["error"], ("not_found", "method_not_allowed"))
        st, hd, raw, _ = req("GET", "/")
        self.assertEqual(st, 200)
        self.assertTrue(hd.get("Content-Type", "").startswith("text/html"))
        self.assertIn(b"espOS", raw)
        self.assertFalse(raw.endswith(b"\x00"), "embedded NUL must not be served")
        self.assertEqual(int(hd.get("Content-Length")), len(raw))
        st, _, _, _ = req("GET", "/index.html")
        self.assertEqual(st, 200)
        # over-long ?ns= is an unknown namespace, not "everything"
        st, _, _, js = req("GET", "/api/v1/config?ns=" + "x" * 40)
        self.assertEqual(st, 404)
        self.assertEqual(js["error"], "unknown_namespace")
        st, _, _, js = req("GET", "/api/v1/config?" + "a" * 300)
        self.assertEqual(st, 414)
        # empty PUT body, non-object roots, empty object
        st, _, _, js = req("PUT", "/api/v1/config", "")
        self.assertEqual(st, 400)
        self.assertEqual(js["error"], "validation")
        st, _, _, js = req("PUT", "/api/v1/config", "[]")
        self.assertEqual(st, 400)
        self.assertIn("expected object", js["message"])
        st, _, _, js = req("PUT", "/api/v1/config", "{}")
        self.assertEqual(st, 200)
        self.assertEqual(js["changed"], [])
        # CSRF guard: state-changing requests without a JSON content type are refused
        for m, path, body in (("PUT", "/api/v1/config", "{}"),
                              ("POST", "/api/v1/system/reboot", None),
                              ("POST", "/api/v1/system/factory-reset", None)):
            st, _, _, js = req(m, path, body, content_type=None)
            self.assertEqual(st, 415, (m, path))
            self.assertEqual(js["error"], "unsupported_media_type")
            st, _, _, js = req(m, path, body, content_type="text/plain")
            self.assertEqual(st, 415, (m, path))
        # ...and a charset suffix is fine
        st, _, _, js = req("PUT", "/api/v1/config", "{}", content_type="application/json; charset=utf-8")
        self.assertEqual(st, 200)

    def test_08_persistence_across_restart(self):
        # values written above must survive a process restart (emulated NVS
        # lives in a file only for the process lifetime, so re-plant then
        # restart WITHOUT the fresh flag against a preserved flash file).
        # The emulated flash file is per process → persistence is covered by
        # the espos_config_test NVS cases; here we just check reboot semantics.
        st, _, _, js = req("POST", "/api/v1/system/reboot")
        self.assertEqual(st, 202)
        self.assertEqual(js["status"], "rebooting")
        # writes are refused while the restart is pending
        st, _, _, js = req("PUT", "/api/v1/config", {"app": {"label": "late"}})
        self.assertEqual(st, 503)
        self.assertEqual(js["error"], "restarting")
        code = self.h.wait_exit(timeout=5.0)
        self.assertEqual(code, 0, "harness must exit cleanly on reboot (esp_restart → exit(0) on linux)")
        # start a new instance for the remaining tests
        type(self).h = Harness(fresh=True)

    def test_09_factory_reset(self):
        st, _, _, js = req("PUT", "/api/v1/config", {"app": {"label": "wipe-me"}})
        self.assertEqual(st, 200)
        st, _, _, js = req("POST", "/api/v1/system/factory-reset")
        self.assertEqual(st, 202)
        self.assertEqual(js["status"], "factory_reset")
        self.assertTrue(js["rebooting"])
        # before the restart lands the store already serves defaults
        _, _, _, js = req("GET", "/api/v1/config?ns=app")
        self.assertEqual(js["app"]["label"], "espOS device")
        code = self.h.wait_exit(timeout=5.0)
        self.assertEqual(code, 0, "harness must exit cleanly on factory reset")
        type(self).h = Harness(fresh=True)

    def test_10_concurrent_requests(self):
        # interleaved PUT/GET from several threads: every response must be
        # well-formed and every GET must observe a value some PUT wrote
        errors = []
        labels = [f"w{i}" for i in range(6)]

        def worker(label):
            try:
                for _ in range(8):
                    st, _, _, js = req("PUT", "/api/v1/config", {"app": {"label": label}})
                    if st != 200:
                        errors.append(("put", st, js))
                    st, _, _, js = req("GET", "/api/v1/config?ns=app")
                    if st != 200 or js["app"]["label"] not in labels + ["espOS device"]:
                        errors.append(("get", st, js))
            except Exception as e:  # noqa: BLE001
                errors.append(("exc", repr(e)))

        threads = [threading.Thread(target=worker, args=(l,)) for l in labels]
        for t in threads:
            t.start()
        for t in threads:
            t.join(60)
        self.assertEqual(errors, [])
        # more sockets than max_open_sockets in quick succession (LRU purge)
        for _ in range(30):
            st, _, _, _ = req("GET", "/api/v1/system/info")
            self.assertEqual(st, 200)

    # ---- health (espOS #137)
    def test_11_health_get(self):
        st, hd, _, js = req("GET", "/api/v1/health")
        self.assertEqual(st, 200)
        self.assertEqual(hd.get("Content-Type"), "application/json")
        # Before this endpoint the conditions were unreadable: they reached the
        # outside world only as SignalK notifications, so "why is the LED red"
        # needed the device's log.
        for k in ("worst", "fatal", "conditions"):
            self.assertIn(k, js, k)
        self.assertIn(js["worst"], ("normal", "warn", "alarm"))
        self.assertIsInstance(js["conditions"], list)
        for c in js["conditions"]:
            for k in ("key", "state", "message", "reboot_on_alarm"):
                self.assertIn(k, c, k)
            self.assertIsInstance(c["reboot_on_alarm"], bool)

    def test_12_health_test_drill_is_never_fatal(self):
        """A drill must reach the sinks without being able to reboot the board."""
        st, _, _, js = req("POST", "/api/v1/health/test",
                           {"key": "test.buzzer", "state": "alarm", "message": "G4", "ttl_s": 60})
        self.assertEqual(st, 200)
        self.assertEqual(js["worst"], "alarm")
        # The property the whole issue turns on: every built-in alarm carries
        # ESPOS_HEALTH_F_REBOOT_ON_ALARM, so raising one to watch a buzzer ends
        # the observation. This one is structurally incapable of it.
        self.assertIsNone(js["fatal"])
        drill = [c for c in js["conditions"] if c["key"] == "test.buzzer"]
        self.assertEqual(len(drill), 1)
        self.assertEqual(drill[0]["state"], "alarm")
        self.assertEqual(drill[0]["message"], "G4")
        self.assertFalse(drill[0]["reboot_on_alarm"])

        # And an explicit clear is the normal way to end it.
        st, _, _, js = req("POST", "/api/v1/health/test",
                           {"key": "test.buzzer", "state": "normal"})
        self.assertEqual(st, 200)
        drill = [c for c in js["conditions"] if c["key"] == "test.buzzer"]
        self.assertEqual(drill[0]["state"], "normal")
        self.assertEqual(js["worst"], "normal")

    def test_13_health_test_validation(self):
        # Unprefixed keys are refused, so a drill cannot impersonate a real
        # fault or clobber a consumer's own condition.
        for body, why in (
            ({"key": "relayExpander", "state": "alarm", "ttl_s": 5}, "no reserved prefix"),
            ({"key": "test.", "state": "alarm", "ttl_s": 5}, "prefix names nothing"),
            ({"key": "test.x", "state": "broken", "ttl_s": 5}, "bad state"),
            ({"key": "test.x"}, "no state"),
            ({"state": "alarm"}, "no key"),
            # Rejected rather than clamped: silently substituting a different ttl
            # is how a test ends up proving the wrong thing.
            ({"key": "test.x", "state": "alarm", "ttl_s": 0}, "ttl too small"),
            ({"key": "test.x", "state": "alarm", "ttl_s": 301}, "ttl too large"),
            ({"key": "test.x", "state": "alarm", "ttl_s": "45"}, "ttl not a number"),
            # Present but the wrong type: silently substituting "" would hide a
            # caller's mistake, and ttl_s above is already strict about this.
            ({"key": "test.x", "state": "alarm", "message": 42}, "message not a string"),
            # espos_health refuses an over-long key or message rather than
            # truncating, and that is the caller's error to fix -- a 400, not the
            # 500 that esp_err_to_name() would have produced.
            ({"key": "test.x", "state": "alarm", "message": "m" * 96}, "message too long"),
            ({"key": "test." + "k" * 24, "state": "alarm"}, "key too long"),
        ):
            st, _, _, js = req("POST", "/api/v1/health/test", body)
            self.assertEqual(st, 400, why)
            self.assertEqual(js.get("error"), "validation", why)

        # The other side of each bound, or the rejections above would pass just as
        # well against a handler that refused everything. ESPOS_HEALTH_MSG_MAX is
        # 96 and KEY_MAX 24, both exclusive of the NUL, so 95 and 23 are legal.
        longest = {"key": "test." + "k" * 18, "state": "alarm", "message": "m" * 95, "ttl_s": 300}
        self.assertEqual(len(longest["key"]), 23)
        st, _, _, js = req("POST", "/api/v1/health/test", longest)
        self.assertEqual(st, 200)
        accepted = [c for c in js["conditions"] if c["key"] == longest["key"]]
        self.assertEqual(len(accepted), 1)
        self.assertEqual(accepted[0]["message"], longest["message"])
        req("POST", "/api/v1/health/test", {"key": longest["key"], "state": "normal"})

        # None of the rejected attempts reached the table, and nothing is raised.
        # Note the assertion is not "no test.* keys exist": a key keeps its slot
        # for the life of the boot once seen, so an earlier test's cleared drill
        # is still listed as normal. That is the documented cost of a new key.
        st, _, _, js = req("GET", "/api/v1/health")
        self.assertEqual([c["key"] for c in js["conditions"] if c["key"] == "test.x"], [])
        self.assertEqual([c["key"] for c in js["conditions"] if c["state"] != "normal"], [])

    def test_14_health_test_one_drill_at_a_time(self):
        st, _, _, _ = req("POST", "/api/v1/health/test",
                          {"key": "test.one", "state": "alarm", "ttl_s": 60})
        self.assertEqual(st, 200)
        st, _, _, js = req("POST", "/api/v1/health/test",
                           {"key": "test.two", "state": "alarm", "ttl_s": 60})
        self.assertEqual(st, 200)
        by_key = {c["key"]: c for c in js["conditions"]}
        # Bounded blast radius: a script that loops cannot leave a trail of fake
        # alarms on a live boat.
        self.assertEqual(by_key["test.one"]["state"], "normal")
        self.assertEqual(by_key["test.two"]["state"], "alarm")
        req("POST", "/api/v1/health/test", {"key": "test.two", "state": "normal"})

    def test_15_health_test_ttl_expires(self):
        st, _, _, js = req("POST", "/api/v1/health/test",
                           {"key": "test.buzzer", "state": "alarm", "ttl_s": 1})
        self.assertEqual(st, 200)
        self.assertEqual(js["worst"], "alarm")
        # The backstop for a test session that goes away. GET drives it too, so
        # this holds even though the policy tick does not run on this target.
        time.sleep(1.2)
        st, _, _, js = req("GET", "/api/v1/health")
        self.assertEqual(st, 200)
        self.assertEqual(js["worst"], "normal")
        drill = [c for c in js["conditions"] if c["key"] == "test.buzzer"]
        self.assertEqual(drill[0]["state"], "normal")


# ------------------------------------------------------------ authentication


class AuthTests(unittest.TestCase):
    """REST authentication (docs/security.md, docs/rest-api.md "Authentication"):
    open until httpd.api_key is set, then Bearer or the espos_sid cookie from
    POST /auth/login; cookie writes need a matching Origin; a new key ends every
    session; logout; the session table's eviction. The linux harness has no
    soft-AP, so the portal exemption never applies here. The failure throttle
    has a harness of its own (AuthThrottleTests): its lockout is global and
    would colour every test after it."""

    KEY = "correct-horse-battery"
    BEARER = {"Authorization": "Bearer " + KEY}

    @classmethod
    def setUpClass(cls):
        cls.h = Harness(fresh=True)

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()

    @staticmethod
    def origin():
        return f"http://127.0.0.1:{PORT}"

    def login(self, key=None):
        st, hd, _, js = req("POST", "/api/v1/auth/login", {"key": self.KEY if key is None else key})
        return st, hd, js

    @staticmethod
    def cookie_of(headers):
        return {"Cookie": headers["Set-Cookie"].split(";")[0]}

    def test_01_open_by_default(self):
        st, _, _, js = req("GET", "/api/v1/auth/status")
        self.assertEqual(st, 200)
        self.assertEqual(
            js, {"required": False, "configured": False, "authenticated": False, "method": "none", "recovery_s": 0}
        )
        st, _, _, _ = req("GET", "/api/v1/config")
        self.assertEqual(st, 200)
        st, _, _, js = req("GET", "/api/v1/system/ping")
        self.assertEqual(st, 200)
        self.assertEqual(js["app"], "espos_httpd_test")
        self.assertIn("version", js)
        self.assertFalse(js["auth"])
        # a stray Bearer on an open device is not judged
        st, _, _, _ = req("GET", "/api/v1/config", headers={"Authorization": "Bearer whatever"})
        self.assertEqual(st, 200)
        # login makes no sense while open
        st, _, _, js = req("POST", "/api/v1/auth/login", {"key": "x"})
        self.assertEqual(st, 409)
        self.assertEqual(js["error"], "auth_open")
        # the key is a secret with the usual sentinel behaviour; the lifetime has its default
        _, _, _, cfg = req("GET", "/api/v1/config?ns=httpd")
        self.assertEqual(cfg["httpd"]["api_key"], "")
        self.assertEqual(cfg["httpd"]["session_ttl_s"], 86400)
        st, _, _, js = req("PUT", "/api/v1/config", {"httpd": {"api_key": "x" * 65}})
        self.assertEqual(st, 400)
        self.assertEqual(js["path"], "httpd.api_key")
        st, _, _, js = req("PUT", "/api/v1/config", {"httpd": {"session_ttl_s": 59}})
        self.assertEqual(st, 400)

    def test_02_setting_a_key_closes_the_api(self):
        st, _, _, js = req("PUT", "/api/v1/config", {"httpd": {"api_key": self.KEY}})
        self.assertEqual(st, 200, js)
        self.assertEqual(js["changed"], ["httpd.api_key"])
        self.assertFalse(js["restart_required"])
        # everything registered through espos_httpd_register(): espOS's own
        # endpoints, another component's (wifi) and the harness's own probe
        for path in ("/api/v1/config", "/api/v1/config/schema", "/api/v1/system/info", "/api/v1/logs",
                     "/api/v1/system/coredump", "/api/v1/wifi/status", "/__harness/sk/rx"):
            st, hd, _, js = req("GET", path)
            self.assertEqual((path, st), (path, 401))
            self.assertEqual(js["error"], "unauthorized")
            self.assertTrue(hd.get("WWW-Authenticate", "").startswith("Bearer"), hd)
            self.assertEqual(hd.get("Content-Type"), "application/json")
        st, _, _, _ = req("PUT", "/api/v1/config", {"app": {"label": "nope"}})
        self.assertEqual(st, 401)
        st, _, _, _ = req("POST", "/api/v1/system/reboot")
        self.assertEqual(st, 401)
        st, _, _, _ = req("DELETE", "/api/v1/system/coredump")
        self.assertEqual(st, 401)
        # the check comes before the handler: no content-type guard, no body parsing
        st, _, _, js = req("PUT", "/api/v1/config", "{not json", content_type=None)
        self.assertEqual(st, 401)
        # public: the UI (and its SPA fallback), liveness, the auth endpoints
        st, _, raw, _ = req("GET", "/")
        self.assertEqual(st, 200)
        self.assertIn(b"espOS", raw)
        st, _, _, _ = req("GET", "/wifi")
        self.assertEqual(st, 200)
        st, _, _, js = req("GET", "/api/v1/system/ping")
        self.assertEqual(st, 200)
        self.assertTrue(js["auth"])
        st, _, _, js = req("GET", "/api/v1/auth/status")
        self.assertEqual(
            js, {"required": True, "configured": True, "authenticated": False, "method": "none", "recovery_s": 0}
        )
        # the stream is protected too
        sse = SseReader()
        self.assertIn("401", sse.status)
        sse.close()
        # an unknown API path is still 404, not 401 (nothing there to protect)
        st, _, _, js = req("GET", "/api/v1/nope")
        self.assertEqual(st, 404)
        # and the key still reads back as the sentinel, never itself
        st, _, _, cfg = req("GET", "/api/v1/config?ns=httpd", headers=self.BEARER)
        self.assertEqual(cfg["httpd"]["api_key"], SENTINEL)

    def test_03_bearer(self):
        st, _, _, js = req("GET", "/api/v1/config", headers=self.BEARER)
        self.assertEqual(st, 200)
        st, _, _, js = req("GET", "/api/v1/auth/status", headers=self.BEARER)
        self.assertEqual((js["authenticated"], js["method"]), (True, "bearer"))
        # a state change with Bearer needs no Origin: it is not a browser credential
        st, _, _, js = req("PUT", "/api/v1/config", {"app": {"label": "bearer"}}, headers=self.BEARER)
        self.assertEqual(st, 200, js)
        self.assertEqual(js["changed"], ["app.label"])
        # the content-type guard still applies behind the check
        st, _, _, js = req("PUT", "/api/v1/config", "{}", headers=self.BEARER, content_type=None)
        self.assertEqual(st, 415)
        # case of the scheme word does not matter; anything else is not a bearer
        st, _, _, _ = req("GET", "/api/v1/config", headers={"Authorization": "bearer " + self.KEY})
        self.assertEqual(st, 200)
        st, _, _, _ = req("GET", "/api/v1/config", headers={"Authorization": "Bearer nope"})
        self.assertEqual(st, 401)
        st, _, _, _ = req("GET", "/api/v1/config", headers={"Authorization": "Bearer " + self.KEY + "x"})
        self.assertEqual(st, 401)
        st, _, _, _ = req("GET", "/api/v1/config", headers={"Authorization": "Basic Y29ycmVjdA=="})
        self.assertEqual(st, 401)
        sse = SseReader(headers=self.BEARER)
        self.assertIn("200", sse.status)
        sse.close()
        st, _, _, js = req("GET", "/__harness/sk/rx", headers=self.BEARER)
        self.assertEqual(st, 200)

    def test_04_login_cookie(self):
        st, _, js = self.login("wrong")
        self.assertEqual(st, 401)
        self.assertEqual(js["error"], "unauthorized")
        st, _, _, js = req("POST", "/api/v1/auth/login", {"nope": 1})
        self.assertEqual(st, 400)
        self.assertEqual(js["error"], "validation")
        st, _, _, js = req("POST", "/api/v1/auth/login", {"key": self.KEY}, content_type=None)
        self.assertEqual(st, 415)
        st, hd, js = self.login()
        self.assertEqual(st, 204, js)
        self.assertEqual(hd.get("Cache-Control"), "no-store")
        m = re.fullmatch(r"espos_sid=([0-9a-f]{32}); HttpOnly; SameSite=Strict; Path=/; Max-Age=86400", hd.get("Set-Cookie", ""))
        self.assertIsNotNone(m, hd.get("Set-Cookie"))
        type(self).cookie = self.cookie_of(hd)
        st, _, _, _ = req("GET", "/api/v1/config", headers=self.cookie)
        self.assertEqual(st, 200)
        st, _, _, js = req("GET", "/api/v1/auth/status", headers=self.cookie)
        self.assertEqual((js["authenticated"], js["method"]), (True, "cookie"))
        # a harness endpoint registered with plain espos_httpd_register(): protected, and the cookie opens it
        st, _, _, _ = req("GET", "/__harness/sk/rx", headers=self.cookie)
        self.assertEqual(st, 200)
        # EventSource sends the cookie by itself; the stream must take it
        sse = SseReader(headers=self.cookie)
        self.assertIn("200", sse.status)
        first = list(itertools.islice(sse.events(timeout=3), 2))
        self.assertEqual(first[0][0], "retry")
        self.assertEqual(len(first), 2, "a snapshot event must follow the hello")   # which one comes first is the components' business
        sse.close()
        # a made-up id, a stale one, other cookies around ours
        st, _, _, _ = req("GET", "/api/v1/config", headers={"Cookie": "espos_sid=" + "0" * 32})
        self.assertEqual(st, 401)
        st, _, _, _ = req("GET", "/api/v1/config", headers={"Cookie": "other=1; " + self.cookie["Cookie"] + "; theme=dark"})
        self.assertEqual(st, 200)
        # a session lifetime change applies to the next login only
        st, _, _, _ = req("PUT", "/api/v1/config", {"httpd": {"session_ttl_s": 600}}, headers=self.BEARER)
        self.assertEqual(st, 200)
        st, hd, _ = self.login()
        self.assertIn("Max-Age=600", hd["Set-Cookie"])
        req("PUT", "/api/v1/config", {"httpd": {"session_ttl_s": None}}, headers=self.BEARER)

    def test_05_cookie_writes_need_a_matching_origin(self):
        c = self.cookie
        # no Origin, no Referer: a browser would have sent one
        st, _, _, js = req("PUT", "/api/v1/config", {"app": {"label": "x"}}, headers=c)
        self.assertEqual(st, 403)
        self.assertEqual(js["error"], "forbidden")
        for bad in ("http://evil.example", "http://127.0.0.1:1", "null", "http://127.0.0.1"):
            st, _, _, js = req("PUT", "/api/v1/config", {"app": {"label": "x"}}, headers={**c, "Origin": bad})
            self.assertEqual((bad, st), (bad, 403))
        st, _, _, js = req("POST", "/api/v1/system/reboot", headers={**c, "Origin": "http://evil.example"})
        self.assertEqual(st, 403)
        st, _, _, js = req("DELETE", "/api/v1/system/coredump", headers={**c, "Origin": "http://evil.example"})
        self.assertEqual(st, 403)
        # the label is untouched by all of that
        _, _, _, cfg = req("GET", "/api/v1/config?ns=app", headers=c)
        self.assertEqual(cfg["app"]["label"], "bearer")
        st, _, _, js = req("PUT", "/api/v1/config", {"app": {"label": "cookie"}}, headers={**c, "Origin": self.origin()})
        self.assertEqual(st, 200, js)
        self.assertEqual(js["changed"], ["app.label"])
        # Referer serves when there is no Origin
        st, _, _, js = req("PUT", "/api/v1/config", {"app": {"label": "referer"}}, headers={**c, "Referer": self.origin() + "/config#app"})
        self.assertEqual(st, 200, js)
        # reads never need it
        st, _, _, _ = req("GET", "/api/v1/config", headers=c)
        self.assertEqual(st, 200)
        req("PUT", "/api/v1/config", {"app": {"label": None}}, headers={**c, "Origin": self.origin()})

    def test_06_changing_the_key_logs_everyone_out(self):
        c = {**self.cookie, "Origin": self.origin()}
        new = self.KEY + "-2"
        st, _, _, js = req("PUT", "/api/v1/config", {"httpd": {"api_key": new}}, headers=c)
        self.assertEqual(st, 200, js)
        st, _, _, _ = req("GET", "/api/v1/config", headers=self.cookie)
        self.assertEqual(st, 401)
        st, _, _, _ = req("GET", "/api/v1/config", headers=self.BEARER)
        self.assertEqual(st, 401)
        st, _, _, _ = req("GET", "/api/v1/config", headers={"Authorization": "Bearer " + new})
        self.assertEqual(st, 200)
        # writing the sentinel leaves the key (and the sessions) alone
        st, hd, _ = self.login(new)
        self.assertEqual(st, 204)
        c2 = self.cookie_of(hd)
        st, _, _, js = req("PUT", "/api/v1/config", {"httpd": {"api_key": SENTINEL}}, headers={"Authorization": "Bearer " + new})
        self.assertEqual(st, 200)
        self.assertEqual(js["changed"], [])
        st, _, _, _ = req("GET", "/api/v1/config", headers=c2)
        self.assertEqual(st, 200)
        # back to the first key, via Bearer
        st, _, _, _ = req("PUT", "/api/v1/config", {"httpd": {"api_key": self.KEY}}, headers={"Authorization": "Bearer " + new})
        self.assertEqual(st, 200)
        st, _, _, _ = req("GET", "/api/v1/config", headers=c2)
        self.assertEqual(st, 401)
        st, hd, _ = self.login()
        self.assertEqual(st, 204)
        type(self).cookie = self.cookie_of(hd)

    def test_07_logout(self):
        st, hd, _, _ = req("POST", "/api/v1/auth/logout", headers=self.cookie)
        self.assertEqual(st, 204)
        self.assertIn("espos_sid=;", hd.get("Set-Cookie", ""))
        self.assertIn("Max-Age=0", hd.get("Set-Cookie", ""))
        st, _, _, _ = req("GET", "/api/v1/config", headers=self.cookie)
        self.assertEqual(st, 401)
        # idempotent, and fine without any cookie at all
        st, _, _, _ = req("POST", "/api/v1/auth/logout", headers=self.cookie)
        self.assertEqual(st, 204)
        st, _, _, _ = req("POST", "/api/v1/auth/logout")
        self.assertEqual(st, 204)
        st, _, _, _ = req("POST", "/api/v1/auth/logout", content_type=None)
        self.assertEqual(st, 415)

    def test_08_session_table_evicts_the_oldest(self):
        # CONFIG_ESPOS_HTTPD_MAX_SESSIONS (4) logins plus one: the first dies, the rest live
        cookies = []
        for _ in range(5):
            st, hd, _ = self.login()
            self.assertEqual(st, 204)
            cookies.append(self.cookie_of(hd))
        self.assertEqual(len({c["Cookie"] for c in cookies}), 5)
        st, _, _, _ = req("GET", "/api/v1/config", headers=cookies[0])
        self.assertEqual(st, 401)
        for c in cookies[1:]:
            st, _, _, _ = req("GET", "/api/v1/config", headers=c)
            self.assertEqual(st, 200)

    def test_09_clearing_the_key_reopens(self):
        st, _, _, js = req("PUT", "/api/v1/config", {"httpd": {"api_key": None}}, headers=self.BEARER)
        self.assertEqual(st, 200, js)
        self.assertEqual(js["changed"], ["httpd.api_key"])
        st, _, _, _ = req("GET", "/api/v1/config")
        self.assertEqual(st, 200)
        st, _, _, js = req("GET", "/api/v1/auth/status")
        self.assertEqual(
            js, {"required": False, "configured": False, "authenticated": False, "method": "none", "recovery_s": 0}
        )
        st, _, _, js = req("GET", "/api/v1/system/ping")
        self.assertFalse(js["auth"])


class AuthThrottleTests(unittest.TestCase):
    """Five wrong keys within a minute lock every key check out for 30 s; live
    cookies keep working. Its own harness: the lockout is global."""

    KEY = "correct-horse-battery"

    @classmethod
    def setUpClass(cls):
        cls.h = Harness(fresh=True)
        st, _, _, _ = req("PUT", "/api/v1/config", {"httpd": {"api_key": cls.KEY}})
        assert st == 200

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()

    def test_failures_throttle(self):
        st, hd, _, _ = req("POST", "/api/v1/auth/login", {"key": self.KEY})
        self.assertEqual(st, 204)
        cookie = {"Cookie": hd["Set-Cookie"].split(";")[0]}
        # one wrong Bearer and four wrong logins: each a plain 401
        st, _, _, _ = req("GET", "/api/v1/config", headers={"Authorization": "Bearer wrong-0"})
        self.assertEqual(st, 401)
        for i in range(1, 5):
            st, _, _, js = req("POST", "/api/v1/auth/login", {"key": f"wrong-{i}"})
            self.assertEqual((i, st), (i, 401))
            self.assertEqual(js["error"], "unauthorized")
        # the sixth key check, right or wrong, is refused with 429 for a while
        st, hd, _, js = req("POST", "/api/v1/auth/login", {"key": "wrong-5"})
        self.assertEqual(st, 429)
        self.assertEqual(js["error"], "too_many_attempts")
        self.assertTrue(1 <= int(hd.get("Retry-After", "0")) <= 30, hd)
        st, _, _, _ = req("POST", "/api/v1/auth/login", {"key": self.KEY})
        self.assertEqual(st, 429)
        st, _, _, _ = req("GET", "/api/v1/config", headers={"Authorization": "Bearer " + self.KEY})
        self.assertEqual(st, 429)
        # no credential at all is still a 401, not a 429
        st, _, _, _ = req("GET", "/api/v1/config")
        self.assertEqual(st, 401)
        # a live session is not a key check
        st, _, _, _ = req("GET", "/api/v1/config", headers=cookie)
        self.assertEqual(st, 200)
        st, _, _, js = req("GET", "/api/v1/auth/status", headers=cookie)
        self.assertEqual(js["method"], "cookie")
        # public endpoints are untouched by the lockout
        st, _, _, _ = req("GET", "/api/v1/system/ping")
        self.assertEqual(st, 200)


# ------------------------------------------------------------ SignalK mock

import http.server
import uuid as _uuid


class MockSignalK:
    """Just enough of signalk-server's security API for the token flow, plus
    /__test/* controls. Runs in a thread on an ephemeral port."""

    @staticmethod
    def ws_send(sock, doc):
        import struct
        data = json.dumps(doc).encode()
        if len(data) < 126:
            hdr = bytes([0x81, len(data)])
        elif len(data) < 65536:
            hdr = bytes([0x81, 126]) + struct.pack(">H", len(data))
        else:
            hdr = bytes([0x81, 127]) + struct.pack(">Q", len(data))
        try:
            sock.sendall(hdr + data)
        except OSError:
            pass

    def push(self, doc):
        """Send a frame (delta or anything) to every open stream client."""
        for sock in list(self.ws_clients):
            MockSignalK.ws_send(sock, doc)

    def push_delta(self, values=None, meta=None, source="mock.1", context=None):
        upd = {"$source": source, "timestamp": "2026-08-18T10:00:00.000Z"}
        if values:
            upd["values"] = [{"path": p, "value": v} for p, v in values.items()]
        if meta:
            upd["meta"] = [{"path": p, "value": v} for p, v in meta.items()]
        self.push({"context": context or ("vessels." + self.self_urn), "updates": [upd]})

    def __init__(self, self_urn="urn:mrn:signalk:uuid:0e6d1a1a-1111-4111-8111-000000000099",
                 redirect_to=None, unauth_once=False, port=0):
        # port: 0 for an ephemeral one; a fixed port lets a test bring a
        #   server up at an address the device was already failing to reach.
        # redirect_to: answer every request with 302 to this URL — the shape
        #   signalk-server takes when ssl is on and something still knocks on
        #   the plain port, which is what sk.scheme = auto probes for.
        # unauth_once: refuse exactly the first stream upgrade with 401, then
        #   behave. The plaintext second-opinion rule says the device must keep
        #   its token through that.
        #
        # No TLS mode: the IDF linux target has no mbedTLS entropy source, so
        # a handshake cannot complete here at all. See the note above SkTlsTests.
        self.self_urn = self_urn
        self.redirect_to = redirect_to
        self.unauth_once = unauth_once
        self.ws_unauth_count = 0
        self.security = True
        self.device_requests = True
        self.requests = {}      # requestId -> dict(clientId, state, permission, token)
        self.tokens = {}        # token -> clientId (valid)
        self.log = []
        self.deltas = []        # decoded delta documents received on the stream
        self.ws_open = 0
        self.ws_accept = True   # False → refuse the upgrade (simulates an unreachable stream)
        self.ws_auth = None     # last Authorization header seen on the stream
        self.meta = {}          # path -> meta dict
        self.meta_puts = []
        self.values = {}        # path -> current value, served as the REST node {"value": …}
        self.subs = []          # subscribe frames received on the stream
        self.unsubs = []
        self.puts = []          # {"requestId","path","value"} received via the stream
        self.put_reply = {"state": "COMPLETED", "statusCode": 200}   # what the mock answers a PUT with
        self.raw_frames = []    # non-delta, non-put frames from the client
        self.ws_clients = []    # live stream sockets (for pushing deltas)
        mock = self

        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def _send(self, code, obj=None, raw=None, ctype="application/json"):
                body = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b"")
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def _ws(self):
                import base64, hashlib, struct
                mock.ws_auth = self.headers.get("Authorization")
                if mock.unauth_once and mock.ws_unauth_count == 0:
                    mock.ws_unauth_count += 1
                    return self._send(401, raw=b"Unauthorized", ctype="text/plain")
                if not mock.ws_accept:
                    return self._send(503, raw=b"no", ctype="text/plain")
                key = self.headers.get("Sec-WebSocket-Key", "")
                acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
                self.send_response(101)
                self.send_header("Upgrade", "websocket")
                self.send_header("Connection", "Upgrade")
                self.send_header("Sec-WebSocket-Accept", acc)
                self.end_headers()
                sock = self.connection
                sock.settimeout(1.0)
                hello = json.dumps({"name": "signalk-server", "version": "2.31.1", "self": "vessels." + mock.self_urn}).encode()
                sock.sendall(bytes([0x81, len(hello)]) + hello)
                mock.ws_open += 1
                mock.ws_clients.append(sock)
                buf = b""
                try:
                    while mock.ws_accept:
                        try:
                            chunk = sock.recv(4096)
                        except socket.timeout:
                            continue
                        if not chunk:
                            break
                        buf += chunk
                        while len(buf) >= 2:
                            fin_op, m_len = buf[0], buf[1]
                            masked = m_len & 0x80
                            ln = m_len & 0x7F
                            off = 2
                            if ln == 126:
                                if len(buf) < 4:
                                    break
                                ln = struct.unpack(">H", buf[2:4])[0]
                                off = 4
                            elif ln == 127:
                                if len(buf) < 10:
                                    break
                                ln = struct.unpack(">Q", buf[2:10])[0]
                                off = 10
                            need = off + (4 if masked else 0) + ln
                            if len(buf) < need:
                                break
                            mask = buf[off:off + 4] if masked else b""
                            payload = buf[off + (4 if masked else 0):need]
                            if masked:
                                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
                            buf = buf[need:]
                            op = fin_op & 0x0F
                            if op == 0x1:
                                try:
                                    doc = json.loads(payload.decode())
                                except Exception:
                                    doc = {"raw": payload.decode(errors="replace")}
                                if "subscribe" in doc:
                                    mock.subs.append(doc)
                                elif "unsubscribe" in doc:
                                    mock.unsubs.append(doc)
                                elif "put" in doc:
                                    mock.puts.append({"requestId": doc.get("requestId"), **doc["put"]})
                                    if mock.put_reply is not None:
                                        rep = dict(mock.put_reply, requestId=doc.get("requestId"))
                                        MockSignalK.ws_send(sock, rep)
                                elif "updates" in doc:
                                    mock.deltas.append(doc)
                                else:
                                    mock.raw_frames.append(doc)
                            elif op == 0x8:
                                raise ConnectionError("close")
                            elif op == 0x9:
                                sock.sendall(bytes([0x8A, len(payload)]) + payload)
                except (ConnectionError, OSError):
                    pass
                finally:
                    mock.ws_open -= 1
                    if sock in mock.ws_clients:
                        mock.ws_clients.remove(sock)
                    try:
                        sock.close()
                    except OSError:
                        pass
                self.close_connection = True

            def do_PUT(self):
                n = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(n) or b"{}")
                if self.path.startswith("/signalk/v1/api/vessels/self/") and self.path.endswith("/meta"):
                    auth = self.headers.get("Authorization", "")
                    tok = auth[7:] if auth.startswith("Bearer ") else ""
                    if mock.security and tok not in mock.tokens:
                        return self._send(401, raw=b"Unauthorized", ctype="text/plain")
                    path = self.path[len("/signalk/v1/api/vessels/self/"):-len("/meta")].replace("/", ".")
                    mock.meta[path] = body.get("value")
                    mock.meta_puts.append(path)
                    return self._send(200, {"state": "COMPLETED", "statusCode": 200})
                return self._send(404, {"error": "nope"})

            def do_GET(self):
                mock.log.append(("GET", self.path, self.headers.get("Authorization")))
                if mock.redirect_to and not self.path.startswith("/__test/"):
                    # What signalk-server does to a plain request once ssl is
                    # on. The probe must read the scheme out of this and must
                    # not have sent a token to get it. /__test/ stays reachable
                    # so the runner can still read the request log back.
                    self.send_response(302)
                    self.send_header("Location", mock.redirect_to + self.path)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if self.path.startswith("/signalk/v1/stream"):
                    return self._ws()
                if self.path.startswith("/signalk/v1/api/vessels/self/") and self.path.endswith("/meta"):
                    path = self.path[len("/signalk/v1/api/vessels/self/"):-len("/meta")].replace("/", ".")
                    if path in mock.meta:
                        return self._send(200, mock.meta[path])
                    return self._send(404, {"error": "no meta"})
                if self.path.startswith("/signalk/v1/api/vessels/self/"):
                    # The REST node of a leaf path, as signalk-server serves it: the
                    # reading sits under "value" next to its source and timestamp.
                    path = self.path[len("/signalk/v1/api/vessels/self/"):].replace("/", ".")
                    if path in mock.values:
                        return self._send(200, {"value": mock.values[path], "$source": "mock.1",
                                                "timestamp": "2026-08-18T10:00:00.000Z"})
                    return self._send(404, {"error": "no such path"})
                if self.path == "/signalk":
                    return self._send(200, {"endpoints": {"v1": {"version": "2.31.1"}}})
                if self.path.startswith("/signalk/v1/requests/"):
                    rid = self.path.rsplit("/", 1)[1]
                    r = mock.requests.get(rid)
                    if not r:
                        return self._send(500, raw=b"Unable to check request: not found", ctype="text/plain")
                    reply = {"state": r["state"], "requestId": rid, "statusCode": 202 if r["state"] == "PENDING" else 200,
                             "href": "/signalk/v1/requests/" + rid}
                    if r["state"] == "COMPLETED":
                        reply["accessRequest"] = {"permission": r["permission"]}
                        if r.get("token"):
                            reply["accessRequest"]["token"] = r["token"]
                    return self._send(200, reply)
                if self.path == "/signalk/v1/api/self":
                    if not mock.security:
                        return self._send(200, "vessels." + mock.self_urn)
                    auth = self.headers.get("Authorization", "")
                    tok = auth[7:] if auth.startswith("Bearer ") else ""
                    if tok and tok in mock.tokens:
                        return self._send(200, "vessels." + mock.self_urn)
                    return self._send(401, raw=b"Unauthorized", ctype="text/plain")
                if self.path.startswith("/__test/"):
                    parts = self.path.split("/")
                    cmd, arg = parts[2], (parts[3] if len(parts) > 3 else "")
                    if cmd == "approve":
                        for rid, r in mock.requests.items():
                            if r["clientId"] == arg and r["state"] == "PENDING":
                                tok = "mock." + _uuid.uuid4().hex
                                r.update(state="COMPLETED", permission="APPROVED", token=tok)
                                mock.tokens[tok] = arg
                                return self._send(200, {"token": tok})
                        return self._send(404, {"error": "no pending"})
                    if cmd == "deny":
                        for rid, r in mock.requests.items():
                            if r["clientId"] == arg and r["state"] == "PENDING":
                                r.update(state="COMPLETED", permission="DENIED")
                                return self._send(200, {})
                        return self._send(404, {"error": "no pending"})
                    if cmd == "revoke":
                        mock.tokens = {t: c for t, c in mock.tokens.items() if c != arg}
                        return self._send(200, {})
                    if cmd == "forget":
                        mock.requests.clear()
                        return self._send(200, {})
                    if cmd == "issue":       # mint a token without a request (manual paste)
                        tok = "manual." + _uuid.uuid4().hex
                        mock.tokens[tok] = arg
                        return self._send(200, {"token": tok})
                    if cmd == "security":
                        mock.security = arg == "on"
                        return self._send(200, {})
                    if cmd == "devreq":
                        mock.device_requests = arg == "on"
                        return self._send(200, {})
                    if cmd == "log":
                        return self._send(200, mock.log)
                    if cmd == "blob":        # n bytes of body, for the client's size cap
                        return self._send(200, raw=b"x" * int(arg), ctype="text/plain")
                return self._send(404, {"error": "nope"})

            def do_POST(self):
                n = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(n) or b"{}")
                mock.log.append(("POST", self.path, body))
                if self.path == "/signalk/v1/access/requests":
                    if not mock.security:
                        return self._send(404, {"message": "Access requests not available. Server security is not enabled."})
                    if not mock.device_requests:
                        return self._send(403, {"state": "COMPLETED", "statusCode": 403})
                    cid = body.get("clientId")
                    for rid, r in mock.requests.items():
                        if r["clientId"] == cid and r["state"] == "PENDING":
                            return self._send(400, {"state": "COMPLETED", "statusCode": 400,
                                                    "message": f"A device with clientId '{cid}' has already requested access"})
                    rid = str(_uuid.uuid4())
                    mock.requests[rid] = {"clientId": cid, "state": "PENDING", "permission": "", "description": body.get("description")}
                    return self._send(202, {"state": "PENDING", "requestId": rid, "statusCode": 202,
                                            "href": "/signalk/v1/requests/" + rid})
                return self._send(404, {"error": "nope"})

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def ctl(self, cmd, arg=""):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        c.request("GET", f"/__test/{cmd}/{arg}")
        r = c.getresponse()
        raw = r.read()
        c.close()
        return r.status, (json.loads(raw) if raw else None)

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class SseReader:
    """Minimal text/event-stream client on a raw socket (stdlib only)."""

    def __init__(self, path="/api/v1/events", timeout=5.0, headers=None):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
        extra = "".join(f"{k}: {v}\r\n" for k, v in (headers or {}).items())
        self.sock.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n{extra}\r\n".encode())
        self.buf = b""
        # headers
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("closed during headers")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        self.status = head.split(b"\r\n")[0].decode()
        self.headers = dict(l.decode().split(": ", 1) for l in head.split(b"\r\n")[1:] if b": " in l)

    def _dechunk(self):
        # Transfer-Encoding: chunked; we only ever get whole small chunks
        out = b""
        while True:
            if b"\r\n" not in self.buf:
                break
            size_line, rest = self.buf.split(b"\r\n", 1)
            try:
                n = int(size_line.strip(), 16)
            except ValueError:
                # not a chunk header — treat buffer as raw data
                out += self.buf
                self.buf = b""
                break
            if len(rest) < n + 2:
                break
            out += rest[:n]
            self.buf = rest[n + 2:]
        return out

    def events(self, timeout=5.0):
        """Yield (event, data) tuples until timeout."""
        deadline = time.time() + timeout
        data_acc = b""
        while time.time() < deadline:
            data_acc += self._dechunk()
            while b"\n\n" in data_acc:
                block, data_acc = data_acc.split(b"\n\n", 1)
                ev, dat = None, []
                for line in block.decode().split("\n"):
                    if line.startswith("event: "):
                        ev = line[7:]
                    elif line.startswith("data: "):
                        dat.append(line[6:])
                    elif line.startswith("retry: ") or line.startswith(":"):
                        ev = ev or ("retry" if line.startswith("retry") else "comment")
                yield ev, "\n".join(dat)
            self.sock.settimeout(max(0.05, deadline - time.time()))
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                return
            self.buf += chunk

    def close(self):
        self.sock.close()


def wait_for(pred, timeout=5.0, step=0.1):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = pred()
        if last:
            return last
        time.sleep(step)
    return last


class WifiTests(unittest.TestCase):
    """WiFi state machine over HTTP with the simulated driver (port_sim.c)."""

    @classmethod
    def setUpClass(cls):
        cls.h = Harness(fresh=True)

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()

    def test_01_unconfigured_brings_the_portal_up(self):
        st, _, _, js = req("GET", "/api/v1/wifi/status")
        self.assertEqual(st, 200)
        self.assertEqual(js["state"], "unconfigured")
        self.assertTrue(js["sta_enabled"])
        self.assertEqual(js["hostname"], "espos-1a2b")
        self.assertTrue(js["portal"]["active"])
        self.assertEqual(js["portal"]["ssid"], "espOS-1a2b")
        self.assertEqual(js["portal"]["ip"], "192.168.4.1")
        # Served over HTTP, not merely held in the state machine: "active" alone
        # cannot distinguish the policy declining from an application saying no.
        self.assertEqual(js["portal"]["force"], "auto")
        self.assertEqual(js["reason"], {"code": 0, "text": ""})
        self.assertNotIn("ip", js)

    def test_01b_short_psk_slot_is_skipped(self):
        st, _, _, js = req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "short"}})
        self.assertEqual(st, 200)
        time.sleep(0.5)
        st, _, _, js = req("GET", "/api/v1/wifi/status")
        self.assertEqual(js["state"], "unconfigured")   # 1..7 char WPA passwords are not valid
        req("PUT", "/api/v1/config", {"wifi": {"ssid0": None, "psk0": None}})
        time.sleep(0.5)

    def test_02_scan(self):
        st, _, _, js = req("POST", "/api/v1/wifi/scan", None, content_type=None)
        self.assertEqual(st, 415)
        st, _, _, js = req("POST", "/api/v1/wifi/scan", None)
        self.assertEqual(st, 202)
        st, _, _, js = req("GET", "/api/v1/wifi/scan")
        self.assertEqual(st, 200)
        self.assertFalse(js["scanning"])
        names = sorted(r["ssid"] for r in js["results"])
        self.assertEqual(names, ["Boat", "Marina-Guest"])
        boat = [r for r in js["results"] if r["ssid"] == "Boat"][0]
        self.assertEqual(boat["auth"], "wpa2")
        self.assertEqual(boat["bssid"], "de:ad:be:ef:00:01")

    def test_03_configure_and_connect_with_sse(self):
        sse = SseReader()
        self.assertIn("200", sse.status)
        self.assertEqual(sse.headers.get("Content-Type"), "text/event-stream")
        # hello: retry, then every component's snapshot (in whatever order they connected)
        hello = list(itertools.islice(sse.events(timeout=3), 4))
        self.assertEqual(hello[0][0], "retry")
        snap = [json.loads(d) for e, d in hello if e == "wifi"]
        self.assertTrue(snap, [e for e, _ in hello])
        self.assertEqual(snap[0]["state"], "unconfigured")

        st, _, _, js = req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "secret12"}})
        self.assertEqual(st, 200)
        seen = []
        for ev, data in sse.events(timeout=4):
            seen.append((ev, data))
            if ev == "wifi" and json.loads(data)["state"] == "connected":
                break
        kinds = [e for e, _ in seen]
        self.assertIn("config", kinds)               # config change was broadcast
        states = [json.loads(d)["state"] for e, d in seen if e == "wifi"]
        self.assertIn("connecting", states)
        self.assertIn("obtaining_ip", states)
        self.assertEqual(states[-1], "connected")
        sse.close()

        st, _, _, js = req("GET", "/api/v1/wifi/status")
        self.assertEqual(js["state"], "connected")
        self.assertEqual(js["ssid"], "Boat")
        self.assertEqual(js["ip"], "10.0.0.2")
        self.assertEqual(js["gateway"], "10.0.0.1")
        self.assertEqual(js["rssi"], -55)
        self.assertEqual(js["network_index"], 0)
        self.assertEqual(js["connect_count"], 1)
        self.assertFalse(js["portal"]["active"])      # portal goes down once connected
        self.assertEqual(js["portal"]["force"], "auto")  # by the policy, not by a request
        self.assertEqual(js["reason"]["code"], 0)
        # secrets stay secret
        _, _, _, cfg = req("GET", "/api/v1/config?ns=wifi")
        self.assertEqual(cfg["wifi"]["psk0"], SENTINEL)
        self.assertEqual(cfg["wifi"]["ssid0"], "Boat")

    def test_03b_sse_eviction_when_full(self):
        readers = [SseReader() for _ in range(3)]
        for r in readers:
            self.assertIn("200", r.status)
        # a 4th stream evicts the oldest instead of failing
        r4 = SseReader()
        self.assertIn("200", r4.status)
        hello = list(itertools.islice(r4.events(timeout=3), 4))
        self.assertIn("wifi", [e for e, _ in hello])
        # the oldest reader now sees EOF (its socket was shut down)
        got = list(readers[0].events(timeout=2))
        # "probe" is the harness's own publisher (see harness_sse_probe_init):
        # it registers after espOS's components, like a consumer firmware's.
        self.assertTrue(all(e in ("retry", "net", "wifi", "sk", "sk_servers", "sk_ws", "sk_tls", "ota", "logs", "probe", "comment") for e, _ in got), got)
        readers[0].sock.settimeout(1.0)
        try:
            eof = readers[0].sock.recv(10) == b""
        except socket.timeout:
            eof = False
        self.assertTrue(eof, "evicted stream must be closed by the server")
        for r in readers + [r4]:
            r.close()
        time.sleep(0.5)

    def test_03c_every_registered_connect_callback_reaches_a_new_client(self):
        """A fresh SSE client gets one snapshot per registered component.

        The on-connect table used to be a hard-coded 4 while espOS shipped six
        publishers, so on a BLE gateway the BLE snapshot silently never
        arrived: the stream opened, every other component's hello turned up,
        and the missing one read as an idle component rather than a failed
        registration. The harness registers one past the configured limit, so
        this also pins down that the overflow is refused rather than
        overrunning the array.
        """
        st, _, _, js = req("GET", "/__harness/sse/cbs")
        self.assertEqual(st, 200)
        # espOS's own publishers (net, wifi, sk, ota) have already taken slots
        # by the time the harness registers, which is the position a consumer
        # firmware's publisher is in -- and where the BLE gateway lost its own.
        self.assertGreaterEqual(js["limit"], 8, "default ceiling must fit the components espOS ships")
        self.assertGreater(js["registered"], 0, "a consumer must still get slots after espOS took its own")
        self.assertGreaterEqual(js["rejected"], 1, "past the limit must be refused, not written past the end")
        self.assertEqual(js["registered"] + js["rejected"], js["limit"] + 1,
                         "every attempt is either taken or refused")

        r = SseReader()
        self.assertIn("200", r.status)
        # Enough events to cover every hello the app registers.
        seen = [e for e, _ in itertools.islice(r.events(timeout=4), js["limit"] + 6)]
        r.close()
        probes = seen.count("probe")
        self.assertEqual(probes, js["registered"],
                         f"expected one probe hello per registered callback, got {probes} in {seen}")

    def test_04_disable_and_reenable(self):
        st, _, _, js = req("PUT", "/api/v1/config", {"wifi": {"sta_enabled": False}})
        self.assertEqual(st, 200)
        js = wait_for(lambda: (lambda r: r[3] if r[3]["state"] == "disabled" else None)(req("GET", "/api/v1/wifi/status")))
        self.assertIsNotNone(js)
        self.assertEqual(js["reason"]["code"], 1005)
        self.assertTrue(js["portal"]["active"])
        req("PUT", "/api/v1/config", {"wifi": {"sta_enabled": True}})
        js = wait_for(lambda: (lambda r: r[3] if r[3]["state"] == "connected" else None)(req("GET", "/api/v1/wifi/status")))
        self.assertIsNotNone(js)
        self.assertEqual(js["connect_count"], 2)


class UiAndLogsTests(unittest.TestCase):
    """M5 firmware side: static UI from a directory, log ring, coredump (absent on host)."""

    @classmethod
    def setUpClass(cls):
        cls.www = tempfile.mkdtemp(prefix="espos-www-")
        os.makedirs(os.path.join(cls.www, "assets"))
        with open(os.path.join(cls.www, "index.html"), "wb") as f:
            f.write(b"<html>SPA</html>")
        with gzip.open(os.path.join(cls.www, "assets", "app-abc123.js.gz"), "wb") as f:
            f.write(b"console.log('hi')")
        with open(os.path.join(cls.www, "plain.txt"), "wb") as f:
            f.write(b"plain")
        cls.h = Harness(fresh=True, extra_env={"ESPOS_WWW_DIR": cls.www})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()
        shutil.rmtree(cls.www, ignore_errors=True)

    def test_01_index_and_spa_fallback(self):
        st, hd, raw, _ = req("GET", "/")
        self.assertEqual(st, 200)
        self.assertEqual(raw, b"<html>SPA</html>")
        self.assertIn("text/html", hd["Content-Type"])
        self.assertEqual(hd["Cache-Control"], "no-cache")
        for route in ("/wifi", "/config/sk", "/index.html"):
            st, hd, raw, _ = req("GET", route)
            self.assertEqual((route, st), (route, 200))
            self.assertEqual(raw, b"<html>SPA</html>")
        # a missing file with an extension is a real 404 (JSON, per contract)
        st, hd, raw, js = req("GET", "/missing.png")
        self.assertEqual(st, 404)
        self.assertEqual(js["error"], "not_found")
        # the API namespace never falls back to the SPA
        st, hd, raw, js = req("GET", "/api/v1/nope")
        self.assertEqual(st, 404)
        self.assertEqual(js["error"], "not_found")
        # no directory traversal
        st, _, _, _ = req("GET", "/../etc/passwd")
        self.assertEqual(st, 404)

    def test_02_gzip_asset_and_plain(self):
        st, hd, raw, _ = req("GET", "/assets/app-abc123.js")
        self.assertEqual(st, 200)
        self.assertEqual(hd.get("Content-Encoding"), "gzip")
        self.assertIn("javascript", hd["Content-Type"])
        self.assertIn("immutable", hd["Cache-Control"])
        self.assertEqual(gzip.decompress(raw), b"console.log('hi')")
        st, hd, raw, _ = req("GET", "/plain.txt")
        self.assertEqual(st, 200)
        self.assertNotIn("Content-Encoding", hd)
        self.assertEqual(raw, b"plain")
        st, _, _, js = req("GET", "/api/v1/system/info")
        self.assertTrue(js["ui_storage"])

    def test_03_logs_ring_and_paging(self):
        st, _, _, js = req("GET", "/api/v1/logs")
        self.assertEqual(st, 200)
        self.assertGreater(js["next"], js["first"])
        self.assertEqual(js["from"], js["first"])
        self.assertEqual(len(js["lines"]), js["next"] - js["first"])
        self.assertTrue(any("espos_httpd" in l for l in js["lines"]))
        self.assertFalse(any("\x1b" in l for l in js["lines"]))     # no colour codes
        # paging: after + limit
        st, _, _, page = req("GET", f"/api/v1/logs?after={js['first']}&limit=2")
        self.assertEqual(page["from"], js["first"] + 1)
        self.assertEqual(page["lines"], js["lines"][1:3])
        # a config change is logged → shows up after the last seq
        req("PUT", "/api/v1/config", {"httpd": {"port": PORT}})
        js2 = wait_for(lambda: (lambda r: r[3] if r[3]["next"] > js["next"] else None)(req("GET", f"/api/v1/logs?after={js['next'] - 1}")))
        self.assertIsNotNone(js2)
        self.assertEqual(js2["from"], js["next"])
        # a too-old "after" is reported as a gap and clamped
        st, _, _, g = req("GET", "/api/v1/logs?after=0")
        self.assertFalse(g["gap"])       # 0 is "from the start", not a gap
        self.assertEqual(g["from"], g["first"])

    def test_04_logs_level(self):
        st, _, _, js = req("PUT", "/api/v1/logs/level", {"tag": "espos_httpd", "level": "debug"})
        self.assertEqual(st, 200)
        self.assertEqual(js, {"tag": "espos_httpd", "level": "debug"})
        st, _, _, js = req("PUT", "/api/v1/logs/level", {"level": "bogus"})
        self.assertEqual(st, 400)
        self.assertEqual(js["error"], "validation")
        st, _, _, js = req("PUT", "/api/v1/logs/level", {"tag": "espos_httpd", "level": "info"})
        self.assertEqual(st, 200)

    def test_05_logs_sse_event(self):
        sse = SseReader()
        req("PUT", "/api/v1/logs/level", {"tag": "espos_sse_probe", "level": "info"})   # any request that logs
        req("PUT", "/api/v1/config", {"httpd": {"port": PORT}})
        seen = [e for e, _ in sse.events(timeout=3)]
        sse.close()
        self.assertIn("logs", seen)

    def test_06_coredump_absent_on_host(self):
        st, _, _, js = req("GET", "/api/v1/system/coredump")
        self.assertEqual(st, 404)
        self.assertEqual(js["error"], "not_found")
        st, _, _, js = req("GET", "/api/v1/system/coredump/raw")
        self.assertEqual(st, 404)
        st, _, _, js = req("DELETE", "/api/v1/system/coredump")
        self.assertEqual(st, 200)


class FirmwareServer:
    """Serves a manifest and fake images (sim format: first line 'ESPOS-IMAGE <project> <version>')."""

    def __init__(self):
        self.files = {}
        srv = self

        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def handle(self):
                try:
                    super().handle()
                except ConnectionResetError:
                    pass            # keep-alive socket reset by an aborting client

            def do_GET(self):
                body = srv.files.get(self.path)
                if body is None:
                    self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass            # the device aborts rejected images mid-download

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        self.base = f"http://127.0.0.1:{self.port}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def image(self, path, project="espos", version="0.7.0", size=40000, badsig=False):
        head = f"ESPOS-IMAGE {project} {version}{' BADSIG' if badsig else ''}\n".encode()
        self.files[path] = head + b"x" * (size - len(head))

    def manifest(self, path, builds, app="espos"):
        self.files[path] = json.dumps({"schema": 1, "app": app, "builds": builds}).encode()

    def stop(self):
        self.httpd.shutdown()


class OtaTests(unittest.TestCase):
    """espos_ota against the sim port: manifest check, install from manifest/URL,
    rejected images, SSE, confirm-on-network policy."""

    @classmethod
    def setUpClass(cls):
        cls.fw = FirmwareServer()
        cls.fw.image("/fw/espos-0.7.0.bin", version="0.7.0")
        cls.fw.image("/fw/other.bin", project="otherapp")
        cls.fw.image("/fw/badsig.bin", badsig=True)
        cls.fw.files["/fw/notimage.bin"] = b"<html>nope</html>"
        cls.fw.manifest("/fw/manifest.json", [
            {"version": "0.7.0", "target": "linux", "url": "espos-0.7.0.bin", "size": 40000, "notes": "test build"},
            {"version": "0.6.0", "target": "linux", "url": "old.bin"},
            {"version": "9.9.9", "target": "esp32p4", "url": "p4.bin"},
        ])
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_OTA_PENDING": "1", "ESPOS_SIM_WIFI": "connect",
                                               "ESPOS_SIM_OTA_KEY_FP": "b3381b48b9cc9941"})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()
        cls.fw.stop()

    def ota(self):
        return req("GET", "/api/v1/ota/status")[3]

    def wait_state(self, *states, timeout=15):
        return wait_for(lambda: (lambda o: o if o["state"] in states else None)(self.ota()), timeout=timeout)

    def test_01_status_and_pending_image_confirms_once_connected(self):
        js = self.ota()
        self.assertEqual(js["running"]["version"], "0.6.0")
        self.assertEqual(js["running"]["target"], "linux")
        self.assertEqual(js["running"]["slot"], "ota_0")
        # Read by the OTA task once it starts, which can be after httpd answers.
        fp = wait_for(lambda: self.ota()["running"]["key_fp"], timeout=5)
        self.assertEqual(fp, "b3381b48b9cc9941")
        self.assertIsNone(js["available"])
        # boots PENDING_VERIFY; WiFi is unconfigured → no confirmation yet
        self.assertTrue(js["running"]["pending_verify"])
        req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "secret12"}})
        js = wait_for(lambda: (lambda o: o if o["running"]["confirmed"] else None)(self.ota()), timeout=15)
        self.assertIsNotNone(js, "image must confirm once the network is up")
        self.assertFalse(js["running"]["pending_verify"])
        self.assertEqual(js["running"]["image_state"], "valid")

    def test_02_check_without_manifest_fails_cleanly(self):
        st, _, _, js = req("POST", "/api/v1/ota/check")
        self.assertEqual(st, 202)
        js = self.wait_state("failed")
        self.assertIsNotNone(js)
        self.assertIn("no manifest URL", js["last_error"])

    def test_03_manifest_check_finds_newer_build(self):
        sse = SseReader()
        st, _, _, js = req("PUT", "/api/v1/config", {"ota": {"manifest_url": self.fw.base + "/fw/manifest.json"}})
        self.assertEqual(st, 200)
        st, _, _, js = req("POST", "/api/v1/ota/check")
        self.assertEqual(st, 202)
        js = self.wait_state("available")
        self.assertIsNotNone(js, self.ota())
        self.assertEqual(js["available"]["version"], "0.7.0")
        self.assertEqual(js["available"]["url"], self.fw.base + "/fw/espos-0.7.0.bin")   # relative → resolved
        self.assertTrue(js["available"]["newer"])
        self.assertEqual(js["available"]["notes"], "test build")
        self.assertIsNotNone(js["manifest"]["last_check_s"])
        events = [e for e, _ in sse.events(timeout=1.5)]
        sse.close()
        self.assertIn("ota", events)

    def test_04_install_available_downloads_with_progress(self):
        st, _, _, js = req("POST", "/api/v1/ota", {})
        self.assertEqual(st, 202)
        prog = wait_for(lambda: (lambda o: o if o["state"] == "downloading" and o["progress"]["received"] > 0 else None)(self.ota()), timeout=10)
        self.assertIsNotNone(prog, self.ota())
        self.assertEqual(prog["progress"]["total"], 40000)
        # a second install while busy is refused
        st, _, _, js = req("POST", "/api/v1/ota", {"url": self.fw.base + "/fw/espos-0.7.0.bin"})
        self.assertEqual(st, 409)
        self.assertEqual(js["error"], "busy")
        js = self.wait_state("ready", "idle", timeout=20)
        self.assertIsNotNone(js, self.ota())
        # sim "reboots" and comes back idle
        js = self.wait_state("idle", timeout=5)
        self.assertIsNotNone(js)
        self.assertIn("reboot", "".join(self.h.log[-40:]).lower())

    def test_05_rejected_images(self):
        for path, expect in (("/fw/other.bin", "this device runs"), ("/fw/badsig.bin", "rejected"),
                             ("/fw/notimage.bin", "not a firmware image"), ("/fw/missing.bin", "HTTP 404")):
            st, _, _, js = req("POST", "/api/v1/ota", {"url": self.fw.base + path})
            self.assertEqual(st, 202, (path, js))
            # Wait for THIS attempt's error, not just for state==failed: the
            # state is still "failed" from the previous iteration, so a slow
            # runner can satisfy wait_state() before the new attempt has
            # replaced last_error, and the assert then reads a stale value.
            js = wait_for(lambda: (lambda o: o if o["state"] == "failed"
                                   and expect in (o.get("last_error") or "") else None)(self.ota()),
                          timeout=15)
            self.assertIsNotNone(js, (path, self.ota()))
            self.assertIn(expect, js["last_error"], path)
        st, _, _, js = req("POST", "/api/v1/ota", {"url": "ftp://x/y"})
        self.assertEqual(st, 400)

    def test_06_manual_rollback(self):
        st, _, _, js = req("POST", "/api/v1/ota/rollback")
        self.assertEqual(st, 202)
        js = wait_for(lambda: (lambda o: o if o["running"]["rolled_back"] else None)(self.ota()), timeout=5)
        self.assertIsNotNone(js)
        self.assertEqual(js["running"]["image_state"], "invalid")


class OtaRollbackTimeoutTests(unittest.TestCase):
    """A pending image that never reaches the network is rolled back after ota.confirm_tmo_s."""

    @classmethod
    def setUpClass(cls):
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_OTA_PENDING": "1", "ESPOS_SIM_WIFI": "fail:99"})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()

    def test_01_rollback_after_timeout(self):
        st, _, _, js = req("PUT", "/api/v1/config", {"ota": {"confirm_tmo_s": 30}, "wifi": {"ssid0": "Boat", "psk0": "secret12"}})
        self.assertEqual(st, 200)
        js = req("GET", "/api/v1/ota/status")[3]
        self.assertTrue(js["running"]["pending_verify"])
        self.assertIsNone(js["running"]["key_fp"], "an unknown key reads null, never an empty fingerprint")
        js = wait_for(lambda: (lambda o: o if o["running"]["rolled_back"] else None)(req("GET", "/api/v1/ota/status")[3]), timeout=45, step=1)
        self.assertIsNotNone(js, req("GET", "/api/v1/ota/status")[3])
        self.assertIn("rollback", js["last_error"])
        self.assertIn("rolling back", "".join(self.h.log).lower())


class SkInboundTests(unittest.TestCase):
    """M7: subscriptions, meta delivery, PUT requests, raw frames, resubscribe on reconnect."""

    @classmethod
    def setUpClass(cls):
        cls.mock = MockSignalK()
        cls.mock.security = False          # open server: streams right away
        servers = f"127.0.0.1,{cls.mock.port},{cls.mock.self_urn},mockboat"
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_SK_SERVERS": servers})
        req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "secret12"}, "sk": {"check_s": 10}})
        js = wait_sk(lambda j: j["ws"]["connected"], timeout=25)
        assert js, sk_status()

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()
        cls.mock.stop()

    def rx(self):
        return req("GET", "/__harness/sk/rx")[3]

    def wait_rx(self, n, timeout=5):
        return wait_for(lambda: (lambda r: r if r["count"] >= n else None)(self.rx()), timeout=timeout)

    def test_01_subscribe_sends_frame_and_receives_values_and_meta(self):
        # The firmware holds subscriptions of its own (the clock fallback), so
        # count from where this test starts rather than from zero.
        _, _, _, sk0 = req("GET", "/api/v1/sk/status")
        subs_before = sk0["ws"]["in"]["subs"]
        st, _, _, js = req("POST", "/__harness/sk/sub", {"pattern": "navigation.*", "period_ms": 500})
        self.assertEqual(st, 200)
        h = js["handle"]
        self.assertGreater(h, 0)
        # Not subs[-1]: the firmware subscribes on its own behalf too (the clock
        # fallback follows navigation.datetime until the time is known), so pick
        # the frame that carries the pattern this test asked for.
        sub = wait_for(lambda: next((s for s in self.mock.subs
                                     if any(x["path"] == "navigation.*" for x in s["subscribe"])), None), timeout=5)
        self.assertIsNotNone(sub, "device must send a subscribe frame")
        self.assertEqual(sub["context"], "vessels.self")
        self.assertEqual(sub["subscribe"][0]["path"], "navigation.*")
        self.assertEqual(sub["subscribe"][0]["period"], 500)
        self.assertEqual(sub["subscribe"][0]["policy"], "instant")
        # the stream was opened with sendMeta=all
        self.assertTrue(any("sendMeta=all" in p for m, p, a in self.mock.log if m == "GET" and "/stream" in p))
        self.mock.push_delta({"navigation.speedOverGround": 3.25, "navigation.position": {"latitude": 54.1, "longitude": 10.2},
                              "environment.depth.belowKeel": 4.0},
                             meta={"navigation.speedOverGround": {"units": "m/s", "zones": [{"lower": 0, "state": "normal"}]}})
        r = self.wait_rx(3)
        self.assertIsNotNone(r, self.rx())
        paths = [i["path"] for i in r["items"]]
        self.assertIn("navigation.speedOverGround", paths)
        self.assertIn("navigation.position", paths)
        self.assertNotIn("environment.depth.belowKeel", paths)          # not subscribed
        sog = [i for i in r["items"] if i["path"] == "navigation.speedOverGround"]
        self.assertEqual(sog[0]["value"], 3.25)
        self.assertEqual(sog[0]["source"], "mock.1")
        self.assertEqual(sog[1]["meta"]["units"], "m/s")               # meta item after values
        pos = [i for i in r["items"] if i["path"] == "navigation.position"][0]
        self.assertEqual(pos["value"], {"latitude": 54.1, "longitude": 10.2})
        st, _, _, sk = req("GET", "/api/v1/sk/status")
        self.assertEqual(sk["ws"]["in"]["subs"], subs_before + 1)
        self.assertGreaterEqual(sk["ws"]["in"]["received"], 3)
        self.assertGreaterEqual(sk["ws"]["in"]["frames"], 2)             # hello + delta at least
        type(self).h1 = h

    def test_02_exact_path_and_wildcard_prefix_dispatch(self):
        req("DELETE", "/__harness/sk/rx")
        n_before = len(self.mock.subs)
        st, _, _, js = req("POST", "/__harness/sk/sub", {"pattern": "notifications.*"})
        st, _, _, js2 = req("POST", "/__harness/sk/sub", {"pattern": "environment.mode"})
        # Wait for both patterns, not for a frame count. `subs` is not empty
        # at this point — test_01 already subscribed — so `len(subs) >= 2`
        # was satisfied by the FIRST of these two, and subs[-1] was then read
        # before the second arrived. Whether the two POSTs share one
        # incremental frame or get one each is a timing detail; what the
        # subscription carries is not.
        new_subs = wait_for(
            lambda: (lambda xs: xs if {"environment.mode", "notifications.*"} <=
                     {x["path"] for x in xs} else None)
                    ([x for s in self.mock.subs[n_before:] for x in s["subscribe"]]),
            timeout=5)
        self.assertIsNotNone(new_subs, self.mock.subs[n_before:])
        # incremental frames carry only the new patterns
        paths = sorted(x["path"] for x in new_subs if x["path"] != "navigation.datetime")
        self.assertEqual(paths, ["environment.mode", "notifications.*"])
        self.assertTrue(all(x["period"] == 1000 for x in new_subs), new_subs)   # default period
        self.mock.push_delta({"notifications.mob": {"state": "emergency", "message": "MOB"},
                              "notifications.anchor.dragging": {"state": "alarm"},
                              "environment.mode": "night", "environment.modeX": "no"})
        r = self.wait_rx(3)
        self.assertIsNotNone(r, self.rx())
        paths = sorted(i["path"] for i in r["items"])
        self.assertEqual(paths, ["environment.mode", "notifications.anchor.dragging", "notifications.mob"])
        # a null value comes through as null (anchor raised etc.)
        req("DELETE", "/__harness/sk/rx")
        self.mock.push_delta({"navigation.anchor.position": None})
        r = self.wait_rx(1)
        self.assertIsNotNone(r)
        self.assertIsNone(r["items"][0]["value"])

    def test_03_put_round_trip_and_failure(self):
        st, _, _, js = req("POST", "/__harness/sk/put", {"path": "navigation.anchor.maxRadius", "value": 42.5})
        self.assertEqual(st, 200, js)
        res = wait_for(lambda: req("GET", "/__harness/sk/put")[3], timeout=5)
        self.assertIsNotNone(res)
        self.assertEqual(res["state"], "COMPLETED")
        self.assertEqual(res["status_code"], 200)
        put = self.mock.puts[-1]
        self.assertEqual(put["path"], "navigation.anchor.maxRadius")
        self.assertEqual(put["value"], 42.5)
        self.assertEqual(put["requestId"], res["request_id"])
        self.assertEqual(len(put["requestId"]), 36)
        # object and null values, verbatim
        req("POST", "/__harness/sk/put", {"path": "navigation.anchor.position", "value": {"latitude": 1.5, "longitude": 2.5}})
        wait_for(lambda: req("GET", "/__harness/sk/put")[3], timeout=5)
        self.assertEqual(self.mock.puts[-1]["value"], {"latitude": 1.5, "longitude": 2.5})
        req("POST", "/__harness/sk/put", {"path": "navigation.anchor.position", "value": None})
        wait_for(lambda: req("GET", "/__harness/sk/put")[3], timeout=5)
        self.assertIsNone(self.mock.puts[-1]["value"])
        # server-side failure is reported as such
        self.mock.put_reply = {"state": "FAILED", "statusCode": 405, "message": "no PUT handler"}
        req("POST", "/__harness/sk/put", {"path": "some.path", "value": 1})
        res = wait_for(lambda: req("GET", "/__harness/sk/put")[3], timeout=5)
        self.assertEqual(res["state"], "FAILED")
        self.assertEqual(res["status_code"], 405)
        self.assertEqual(res["message"], "no PUT handler")
        self.mock.put_reply = {"state": "COMPLETED", "statusCode": 200}
        st, _, _, sk = req("GET", "/api/v1/sk/status")
        self.assertEqual(sk["ws"]["put"]["ok"], 3)
        self.assertEqual(sk["ws"]["put"]["failed"], 1)
        self.assertEqual(sk["ws"]["put"]["pending"], 0)

    def test_04_put_timeout(self):
        self.mock.put_reply = None                    # server never answers
        req("POST", "/__harness/sk/put", {"path": "slow.path", "value": 1})
        res = wait_for(lambda: req("GET", "/__harness/sk/put")[3], timeout=15, step=0.5)
        self.assertIsNotNone(res)
        self.assertEqual(res["state"], "TIMEOUT")
        self.mock.put_reply = {"state": "COMPLETED", "statusCode": 200}

    def test_05_raw_frame(self):
        frame = json.dumps({"context": "vessels.self", "updates": [{"values": [{"path": "notifications.mob", "value": {"state": "normal"}}]}]})
        st, _, _, js = req("POST", "/__harness/sk/put", {"raw": frame})
        self.assertEqual(st, 200)
        d = wait_for(lambda: self.mock.deltas[-1] if self.mock.deltas and self.mock.deltas[-1].get("updates", [{}])[0].get("values", [{}])[0].get("path") == "notifications.mob" else None, timeout=5)
        self.assertIsNotNone(d)

    def test_06_unsubscribe_and_resubscribe_after_reconnect(self):
        # Subscriptions the firmware makes on its own behalf (the clock
        # fallback) are not this test's business, but they are counted.
        own_subs = sum(1 for s in self.mock.subs for x in s["subscribe"] if x["path"] == "navigation.datetime")
        own_subs = 1 if own_subs else 0
        st, _, _, js = req("POST", "/__harness/sk/sub", {"unsubscribe": self.h1})
        self.assertEqual(js["result"], "ESP_OK")
        un = wait_for(lambda: self.mock.unsubs[-1] if self.mock.unsubs else None, timeout=5)
        self.assertIsNotNone(un)
        self.assertEqual(un["unsubscribe"][0]["path"], "navigation.*")
        # drop the stream: everything still subscribed must be re-sent in one frame
        n_before = len(self.mock.subs)
        self.mock.ws_accept = False
        wait_sk(lambda j: not j["ws"]["connected"], timeout=10)
        self.mock.ws_accept = True
        js = wait_sk(lambda j: j["ws"]["connected"], timeout=25)
        self.assertIsNotNone(js, sk_status())
        sub = wait_for(lambda: self.mock.subs[-1] if len(self.mock.subs) > n_before else None, timeout=5)
        self.assertIsNotNone(sub)
        # The firmware's own subscription (the clock fallback) is resent too;
        # what this test is about is that the app's survived the reconnect.
        paths = sorted(x["path"] for x in sub["subscribe"] if x["path"] != "navigation.datetime")
        self.assertEqual(paths, ["environment.mode", "notifications.*"])
        st, _, _, sk = req("GET", "/api/v1/sk/status")
        # The two this test registered, plus whatever the firmware holds itself.
        self.assertEqual(sk["ws"]["in"]["subs"], len(paths) + own_subs)

    def test_07_large_frame_is_reassembled(self):
        req("DELETE", "/__harness/sk/rx")
        req("POST", "/__harness/sk/sub", {"pattern": "big.*"})
        wait_for(lambda: len(self.mock.subs) or None, timeout=5)
        time.sleep(0.5)
        # ~9 KiB delta: many values in one frame (larger than the 4 KiB initial buffer)
        vals = {f"big.path{i:03d}": {"text": "x" * 40, "n": i} for i in range(120)}
        self.mock.push_delta(vals)
        r = self.wait_rx(64, timeout=8)     # probe keeps 64
        self.assertIsNotNone(r, self.rx()["count"])
        self.assertEqual(r["count"], 120)


class WifiFailureTests(unittest.TestCase):
    """Driver reports AUTH_FAIL (202) on every attempt."""

    @classmethod
    def setUpClass(cls):
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_WIFI": "fail:202"})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()

    def test_wrong_password_is_reported_with_backoff(self):
        st, _, _, js = req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "wrongpass", "ssid1": "Other", "psk1": "xxxxxxxx"}})
        self.assertEqual(st, 200)
        # after both networks fail once we must be in backoff with the reason
        js = wait_for(lambda: (lambda r: r[3] if r[3]["state"] == "backoff" else None)(req("GET", "/api/v1/wifi/status")), timeout=6)
        self.assertIsNotNone(js)
        self.assertEqual(js["reason"]["code"], 202)
        self.assertEqual(js["reason"]["text"], "wrong password")
        self.assertGreater(js["backoff_ms"], 0)
        self.assertLessEqual(js["backoff_ms"], 1300 * (2 ** (js["round"] - 1)))   # 1 s·2^(round-1) ±25 %
        self.assertGreaterEqual(js["round"], 1)
        self.assertEqual(js["attempt"], 2 * js["round"])   # both networks tried every round
        self.assertEqual(js["network_index"], 0)
        # a second round follows automatically
        js2 = wait_for(lambda: (lambda r: r[3] if r[3]["round"] >= 2 else None)(req("GET", "/api/v1/wifi/status")), timeout=6)
        self.assertIsNotNone(js2)
        self.assertGreaterEqual(js2["attempt"], 3)


def sk_status():
    st, _, _, js = req("GET", "/api/v1/sk/status")
    assert st == 200, js
    return js


def wait_sk(pred, timeout=15.0):
    return wait_for(lambda: (lambda js: js if pred(js) else None)(sk_status()), timeout=timeout, step=0.25)


class SkTests(unittest.TestCase):
    """SignalK discovery + token flow against MockSignalK (real HTTP)."""

    @classmethod
    def setUpClass(cls):
        cls.mock = MockSignalK()
        servers = f"127.0.0.1,{cls.mock.port},{cls.mock.self_urn},mockboat"
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_SK_SERVERS": servers})
        # simulated WiFi must be up for discovery to fire promptly
        req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "secret12"}, "sk": {"check_s": 10}})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()
        cls.mock.stop()

    def test_01_discovery_selects_server_and_requests_access(self):
        js = wait_sk(lambda j: j["token"]["state"] == "pending", timeout=20)
        self.assertIsNotNone(js, sk_status())
        self.assertEqual(js["server"]["host"], "127.0.0.1")
        self.assertEqual(js["server"]["port"], self.mock.port)
        self.assertEqual(js["server"]["self"], self.mock.self_urn)
        self.assertEqual(js["server"]["source"], "discovered")
        self.assertTrue(js["token"]["pending_href"].startswith("/signalk/v1/requests/"))
        self.assertEqual(len(js["client_id"]), 36)
        self.assertEqual(js["description"], "espOS espos-1a2b")
        self.assertEqual(js["permissions"], "readwrite")
        st, _, _, srv = req("GET", "/api/v1/sk/servers")
        self.assertEqual(st, 200)
        self.assertEqual(len(srv["servers"]), 1)
        self.assertTrue(srv["servers"][0]["selected"])
        # the request the mock saw carries our identity
        posted = [b for m, p, b in self.mock.ctl("log")[1] if m == "POST"]
        self.assertEqual(posted[-1]["clientId"], js["client_id"])
        self.assertEqual(posted[-1]["permissions"], "readwrite")

    def test_02_approve_yields_token_verified(self):
        cid = sk_status()["client_id"]
        st, js = self.mock.ctl("approve", cid)
        self.assertEqual(st, 200, js)
        js = wait_sk(lambda j: j["token"]["state"] == "approved", timeout=15)
        self.assertIsNotNone(js, sk_status())
        self.assertTrue(js["token"]["has_token"])
        self.assertEqual(js["token"]["counts"]["approved"], 1)
        self.assertNotIn("pending_href", js["token"])
        # secrets: token never appears in the status document
        self.assertNotIn("mock.", json.dumps(js))

    def test_03_revoke_is_detected_and_re_requested(self):
        cid = sk_status()["client_id"]
        self.mock.ctl("revoke", cid)
        js = wait_sk(lambda j: j["token"]["state"] == "pending", timeout=30)   # check_s = 10
        self.assertIsNotNone(js, sk_status())
        # The mock is plaintext, where a single 401 buys a second opinion 5 s
        # later rather than clearing the token (a captive portal or a proxy
        # answers 401 too). Two in a row do clear it, so the count is 2.
        self.assertEqual(js["token"]["counts"]["unauthorized"], 2)
        self.assertFalse(js["token"]["has_token"])

    def test_04_deny_then_user_retry(self):
        cid = sk_status()["client_id"]
        self.mock.ctl("deny", cid)
        js = wait_sk(lambda j: j["token"]["state"] == "denied", timeout=70)     # poll backoff ≤ 60 s
        self.assertIsNotNone(js, sk_status())
        self.assertIn("denied", js["token"]["last_error"])
        time.sleep(1.0)
        self.assertEqual(sk_status()["token"]["state"], "denied")           # no auto retry
        st, _, _, _ = req("POST", "/api/v1/sk/request", None)
        self.assertEqual(st, 202)
        js = wait_sk(lambda j: j["token"]["state"] == "pending", timeout=10)
        self.assertIsNotNone(js)

    def test_05_server_forgets_request_404_re_request(self):
        before = sk_status()["token"]["counts"]["requests"]
        self.mock.ctl("forget")
        # The counter increments when the request is SENT, and the state is
        # "requesting" until the response lands and moves it to "pending".
        # Waiting on the count alone can therefore observe the intermediate
        # state on a slow runner; wait for both.
        js = wait_sk(lambda j: j["token"]["counts"]["requests"] > before
                     and j["token"]["state"] == "pending", timeout=70)
        self.assertIsNotNone(js, sk_status())
        self.assertEqual(js["token"]["state"], "pending")

    def test_06_manual_token_paste(self):
        cid = sk_status()["client_id"]
        st, js = self.mock.ctl("issue", cid)
        tok = js["token"]
        st, _, _, js = req("POST", "/api/v1/sk/token", {"token": tok})
        self.assertEqual(st, 202)
        js = wait_sk(lambda j: j["token"]["state"] == "approved", timeout=10)
        self.assertIsNotNone(js, sk_status())
        # bad requests
        st, _, _, js = req("POST", "/api/v1/sk/token", {"nope": 1})
        self.assertEqual(st, 400)
        st, _, _, js = req("POST", "/api/v1/sk/token", {"token": "x"}, content_type=None)
        self.assertEqual(st, 415)

    def test_07_forget_and_sse_events(self):
        sse = SseReader()
        # every component's snapshot follows the hello; take enough of them
        hello = list(itertools.islice(sse.events(timeout=3), 8))
        kinds = [e for e, _ in hello]
        self.assertIn("sk", kinds)
        self.assertIn("sk_servers", kinds)
        self.mock.ctl("forget")   # the request left pending on the server would 400 (duplicate)
        st, _, _, _ = req("POST", "/api/v1/sk/forget", None)
        self.assertEqual(st, 202)
        seen = []
        for ev, data in sse.events(timeout=10):
            if ev == "sk":
                seen.append(json.loads(data)["token"]["state"])
                if seen[-1] == "pending":
                    break
        sse.close()
        self.assertIn("pending", seen)
        self.assertFalse(sk_status()["token"]["has_token"])

    def test_08_security_disabled_means_open(self):
        self.mock.ctl("security", "off")
        self.mock.ctl("forget")
        st, _, _, _ = req("POST", "/api/v1/sk/forget", None)   # start from scratch → POST → 404 → open
        js = wait_sk(lambda j: j["token"]["state"] == "open", timeout=10)
        self.assertIsNotNone(js, sk_status())
        self.mock.ctl("security", "on")

    def test_08b_stream_sends_deltas_and_reconciles_meta(self):
        # ensure approved with a token again (test_08 left it "open"/re-requesting)
        cid = sk_status()["client_id"]
        self.mock.ctl("forget")
        req("POST", "/api/v1/sk/request", None)
        wait_sk(lambda j: j["token"]["state"] == "pending", timeout=10)
        self.mock.ctl("approve", cid)
        js = wait_sk(lambda j: j["token"]["state"] == "approved", timeout=15)
        self.assertIsNotNone(js, sk_status())
        js = wait_sk(lambda j: j["ws"]["connected"], timeout=15)
        self.assertIsNotNone(js, sk_status())
        self.assertTrue(self.mock.ws_auth.startswith("Bearer "))
        # publish two values quickly → one delta with both; declared meta reconciled by PUT
        n0 = len(self.mock.deltas)
        st, _, _, js = req("POST", "/api/v1/sk/publish", {"path": "espos.test.a", "value": 1.5,
                                                          "meta": {"units": "V"}, "period_ms": 1000})
        self.assertEqual(st, 202, js)
        st, _, _, js = req("POST", "/api/v1/sk/publish", {"path": "espos.test.b", "value": "hello"})
        self.assertEqual(st, 202, js)
        ok = wait_for(lambda: len(self.mock.deltas) > n0 or None, timeout=5)
        self.assertTrue(ok, "no delta arrived")
        d = self.mock.deltas[n0]
        self.assertEqual(d["context"], "vessels.self")
        upd = d["updates"][0]
        self.assertEqual(upd["source"]["label"], "espos-1a2b")
        vals = {v["path"]: v["value"] for v in upd["values"]}
        self.assertEqual(vals.get("espos.test.a"), 1.5)
        self.assertEqual(vals.get("espos.test.b"), "hello")
        ok = wait_for(lambda: "espos.test.a" in self.mock.meta or None, timeout=5)
        self.assertTrue(ok, "meta not reconciled")
        self.assertEqual(self.mock.meta["espos.test.a"]["units"], "V")
        self.assertEqual(self.mock.meta["espos.test.a"]["timeout"], 2.5)
        # server-side edits win: a redeclare against existing meta does not PUT again
        self.mock.meta["espos.test.a"] = {"units": "kV"}
        puts0 = len(self.mock.meta_puts)
        req("POST", "/api/v1/sk/publish", {"path": "espos.test.a", "value": 2, "meta": {"units": "V"}})
        time.sleep(1.5)
        self.assertEqual(len(self.mock.meta_puts), puts0)
        self.assertEqual(self.mock.meta["espos.test.a"]["units"], "kV")
        # health values arrive under espos.<label>.*
        js = wait_for(lambda: any(v["path"].endswith(".uptime") for d in self.mock.deltas for v in d["updates"][0]["values"]) or None, timeout=15)
        self.assertTrue(js, "no health delta")

    def test_08c_offline_buffer_drains_in_order_after_reconnect(self):
        js = wait_sk(lambda j: j["ws"]["connected"], timeout=15)
        self.assertIsNotNone(js)
        self.mock.ws_accept = False          # server drops the stream and refuses upgrades
        js = wait_sk(lambda j: not j["ws"]["connected"], timeout=15)
        self.assertIsNotNone(js, sk_status())
        n0 = len(self.mock.deltas)
        for i in range(10):
            req("POST", "/api/v1/sk/publish", {"path": "espos.test.seq", "value": i})
            time.sleep(0.4)                  # well beyond the batch window: one message each
        js = wait_sk(lambda j: j["ws"]["buffered"] >= 10, timeout=5)
        self.assertIsNotNone(js, sk_status())
        self.assertFalse(js["ws"]["connected"])
        self.mock.ws_accept = True
        js = wait_sk(lambda j: j["ws"]["connected"] and j["ws"]["buffered"] == 0, timeout=70)
        self.assertIsNotNone(js, sk_status())
        seq = [v["value"] for d in self.mock.deltas[n0:] for v in d["updates"][0]["values"] if v["path"] == "espos.test.seq"]
        self.assertEqual(seq, list(range(10)))     # complete and in order

    def test_09_manual_host_config_wins_over_discovery(self):
        st, _, _, _ = req("PUT", "/api/v1/config", {"sk": {"server_host": "127.0.0.1", "server_port": self.mock.port}})
        self.assertEqual(st, 200)
        js = wait_sk(lambda j: j["server"].get("source") == "manual", timeout=10)
        self.assertIsNotNone(js, sk_status())
        self.assertEqual(js["server"]["host"], "127.0.0.1")
        req("PUT", "/api/v1/config", {"sk": {"server_host": None, "server_port": None}})
        js = wait_sk(lambda j: j["server"].get("source") == "discovered", timeout=10)
        self.assertIsNotNone(js, sk_status())

    # ---- espos_sk_http_* (the real esp_http_client, through the harness probe) ----

    def http(self, op, path, **kw):
        st, _, _, js = req("POST", "/__harness/sk/http", dict(op=op, path=path, **kw))
        self.assertEqual(st, 200, js)
        return js

    def test_10_http_helper_get_200_with_body_and_bearer_header(self):
        # The firmware probes /signalk on its own to settle sk.scheme, and that
        # probe is deliberately unauthenticated (SensESP #1057). It can land
        # between this request and the log read, so "the last /signalk in the
        # log" is not necessarily ours: take a baseline and look only at what
        # this call added.
        def signalk_gets():
            return [e for e in self.mock.ctl("log")[1] if e[0] == "GET" and e[1].startswith("/signalk")]

        n0 = len(signalk_gets())
        js = self.http("get", "/signalk")
        self.assertEqual(js["err"], "ESP_OK")
        self.assertEqual(js["status"], 200)
        self.assertFalse(js["truncated"])
        self.assertEqual(js["len"], len(js["body"]))
        self.assertEqual(json.loads(js["body"])["endpoints"]["v1"]["version"], "2.31.1")
        # the token rode as an Authorization header, never in the URL
        added = signalk_gets()[n0:]
        mine = [e for e in added if e[2] is not None]
        self.assertTrue(mine, added)
        self.assertTrue(mine[-1][2].startswith("Bearer "), mine[-1])
        self.assertNotIn("token=", mine[-1][1])
        # opting out of auth drops the header entirely
        n1 = len(signalk_gets())
        self.http("get", "/signalk", no_auth=True)
        added = [e for e in signalk_gets()[n1:] if e[1] == "/signalk"]
        self.assertTrue(added, "the no_auth request was never seen")
        self.assertTrue(all(e[2] is None for e in added), added)

    def test_11_http_helper_404_is_a_reply_not_an_error(self):
        js = self.http("get", "/signalk/v1/api/vessels/self/nothing/here")
        self.assertEqual(js["err"], "ESP_OK")
        self.assertEqual(js["status"], 404)
        self.assertEqual(json.loads(js["body"]), {"error": "no such path"})

    def test_12_http_helper_oversize_body_is_truncated_not_parsed(self):
        js = self.http("get", "/__test/blob/40000")          # default cap 16 KiB
        self.assertEqual(js["status"], 200)
        self.assertTrue(js["truncated"])
        self.assertEqual(js["len"], 16384)
        self.assertEqual(js["body"], "x" * 16384)
        js = self.http("get", "/__test/blob/40000", max_body=1000)
        self.assertTrue(js["truncated"])
        self.assertEqual(js["len"], 1000)
        js = self.http("get", "/__test/blob/1000", max_body=1000)   # exactly at the cap: whole
        self.assertFalse(js["truncated"])
        self.assertEqual(js["len"], 1000)
        js = self.http("get", "/__test/blob/0")
        self.assertEqual(js["status"], 200)
        self.assertEqual(js["len"], 0)
        self.assertEqual(js["body"], "")

    def test_12b_value_meta_urls_and_put_share_the_core(self):
        self.mock.values["navigation.speedOverGround"] = 3.25
        self.mock.values["navigation.position"] = {"latitude": 54.1, "longitude": 10.2}
        self.mock.meta["navigation.speedOverGround"] = {"units": "m/s"}
        js = self.http("value", "navigation.speedOverGround")
        self.assertEqual(js["err"], "ESP_OK")
        self.assertEqual(js["json"], 3.25)
        js = self.http("value", "navigation.position")
        self.assertEqual(js["json"], {"latitude": 54.1, "longitude": 10.2})
        js = self.http("value", "navigation.nothing")
        self.assertEqual(js["err"], "ESP_ERR_NOT_FOUND")
        self.assertIsNone(js["json"])
        js = self.http("meta", "navigation.speedOverGround")
        self.assertEqual(js["json"], {"units": "m/s"})
        js = self.http("meta", "navigation.position")
        self.assertEqual(js["err"], "ESP_ERR_NOT_FOUND")
        base = f"127.0.0.1:{self.mock.port}"
        self.assertEqual(self.http("url", "/signalk/v1/api")["url"], f"http://{base}/signalk/v1/api")
        self.assertEqual(self.http("ws_url", "signalk/v1/stream")["url"], f"ws://{base}/signalk/v1/stream")
        # PUT goes through the same core: the meta lands on the mock under our token
        js = self.http("put", "/signalk/v1/api/vessels/self/espos/http/test/meta", json='{"value":{"units":"Hz"}}')
        self.assertEqual(js["status"], 200, js)
        self.assertEqual(self.mock.meta["espos.http.test"], {"units": "Hz"})

    def test_13_http_helper_401_reports_unauthorized(self):
        # A long check interval, so what flips the token machine below is the
        # helper's report and not the periodic re-verify.
        req("PUT", "/api/v1/config", {"sk": {"check_s": 3600}})
        js = wait_sk(lambda j: j["token"]["state"] == "approved", timeout=15)
        self.assertIsNotNone(js, sk_status())
        before = js["token"]["counts"]["unauthorized"]
        self.mock.ctl("revoke", js["client_id"])
        js = self.http("get", "/signalk/v1/api/self")
        self.assertEqual(js["err"], "ESP_OK")
        self.assertEqual(js["status"], 401)
        # First report: the token is kept and a verify leg is scheduled 5 s
        # out (plaintext second-opinion rule). That leg gets 401 too, and the
        # second one clears it.
        js = wait_sk(lambda j: j["token"]["counts"]["unauthorized"] == before + 1, timeout=10)
        self.assertIsNotNone(js, sk_status())
        self.assertTrue(js["token"]["has_token"])
        js = wait_sk(lambda j: not j["token"]["has_token"], timeout=15)
        self.assertIsNotNone(js, sk_status())
        self.assertEqual(js["token"]["counts"]["unauthorized"], before + 2)
        self.assertIn(js["token"]["state"], ("requesting", "pending"))


# --------------------------------------------------------------- TLS trust
#
# WHAT IS *NOT* HERE, AND WHY: a real TLS handshake.
#
# The IDF linux target has no entropy source for mbedTLS.
# components/mbedtls/CMakeLists.txt compiles port/esp_hardware.c — the file
# that defines mbedtls_hardware_poll() on top of esp_fill_random() — only
# `if(NOT ${IDF_TARGET} STREQUAL "linux")`, so on the host HMAC_DRBG has
# nothing to seed from and every handshake dies before a certificate is ever
# exchanged:
#
#   E esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x0089
#   I esp-tls-mbedtls: HMAC_DRBG - The entropy source failed
#
# That is upstream and has nothing to do with the trust store; espos_sk and
# esp-tls do link and run on the host with CONFIG_ESPOS_SK_TLS=y, and the
# attach hook is reached (the device logs the skip_common_name warning and
# raises tlsMemory: normal before the handshake fails). So:
#
#   * the trust DECISION — every branch of the table, the SAN normalisation,
#     the truncation rule, and the token machine's cert_error states — is
#     covered by Unity in test/host/espos_sk_test/main/test_tls_policy.c, where
#     it needs no certificate at all because it is pure C.
#   * what is covered HERE is everything around it that a real HTTP request
#     can reach: the /sk/tls endpoints and their validation, the config keys
#     and the migration, the scheme probe's plaintext half, and the
#     plaintext-401 rule.
#   * the parts that need a certificate on the wire — first-use pinning, a
#     changed certificate becoming cert_error, a CA-signed renewal being
#     accepted — are the manual recipe in docs/signalk.md, against a real
#     signalk-server with `ssl: true`.


def sk_tls():
    st, _, _, js = req("GET", "/api/v1/sk/tls")
    if st == 404:
        return None   # built without CONFIG_ESPOS_SK_TLS
    assert st == 200, js
    return js


# A CA certificate, fixed rather than generated: PUT /sk/tls/ca has to parse a
# real one to answer 200, and a constant keeps the runner free of openssl.
# Generated once with:
#   openssl req -x509 -newkey rsa:2048 -keyout /dev/null -nodes \
#       -subj /CN=espOS\ test\ CA -addext basicConstraints=critical,CA:TRUE \
#       -days 36500 -out ca.crt
CA_PEM = """-----BEGIN CERTIFICATE-----
MIIDEzCCAfugAwIBAgIUKwJri355/enabspihVq3MRsLIiMwDQYJKoZIhvcNAQEL
BQAwGDEWMBQGA1UEAwwNZXNwT1MgdGVzdCBDQTAgFw0yNjA5MDcxNzIwMzBaGA8y
MTI2MDgxNDE3MjAzMFowGDEWMBQGA1UEAwwNZXNwT1MgdGVzdCBDQTCCASIwDQYJ
KoZIhvcNAQEBBQADggEPADCCAQoCggEBAO2x+3o0+yr/Wtmb8fYS3VDGC2rgdXbz
epmWpEM0GkxPBzxambDJRdNnfh/j6v2kPHX+F9IZJzzNawQ7tuAqa/NY3qIDQ5Jw
FAWk31GSl/LTR9gykognj+rySojljN5rnCqRxZvJlB7Fe42vT6jQ4UVW29TiwNwN
ohRNE1pVWj/nXm6o4vGFd5zxqCKQ6RoyxG3hPujTMXWXpSQLdNlYAkSZS/0bVWC1
asuHUYXMhhjvcSF2qSF5nwfiWNaIh/9V8AODzAkZ45VP9NjwmpZ11FL8wU30PcOr
RJCzzeG8hBRpi3UZ3G3DbNSPRnYHIuHKE3mwUW7lX/A+/iSvOqctWqECAwEAAaNT
MFEwHQYDVR0OBBYEFFPCYVAHALNtfIyR3y5KN1KVRAeTMB8GA1UdIwQYMBaAFFPC
YVAHALNtfIyR3y5KN1KVRAeTMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQEL
BQADggEBAAiTluz/OqEr9gXioaRtySIIsEb0obC9H7YyWlzjz7aczvi9w6o/Azlz
EvevNXvh+vaJI083z1gN3xlO/5jtcGIccypRYpbAv7DhGVuNv3v6gM91ReKSJZFR
1JpqHh3Rku204v+x8notJ8Y8qqCatryJb+UVO8N1cYDWwmw/lTVELdJbTNm8R2gk
z3HfGbbkn4NvMPD2ekfkBnQeZSR7sx+8MajqChy8bawd3ceWWVtIWJ7QfEx6pNGV
ixT7/+WSDT5wgkFM1ast97FmvwF525Req6J+2cjxo0fiuBBQJAdl6uKnDK8stZvx
v6fQ7NmTKeKYacOxyMfC3SE00rf728k=
-----END CERTIFICATE-----
"""


class SkTlsTests(unittest.TestCase):
    """The /sk/tls endpoints, the config keys behind them, and the scheme
    probe — everything the trust store exposes that does not need a handshake
    (see the note above)."""

    @classmethod
    def setUpClass(cls):
        cls.mock = MockSignalK()
        servers = f"127.0.0.1,{cls.mock.port},{cls.mock.self_urn},mockboat"
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_SK_SERVERS": servers})
        req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "secret12"}})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()
        cls.mock.stop()

    def test_01_the_document_starts_empty_and_says_tofu(self):
        js = sk_tls()
        if js is None:
            self.skipTest("built without CONFIG_ESPOS_SK_TLS")
        self.assertEqual(js["trust"], "tofu")      # the default: not "bundle"
        self.assertIsNone(js["pinned"])            # nothing pinned before a connection
        self.assertIsNone(js["presented"])
        self.assertEqual(js["last_error"], "")

    def test_02_trust_and_scheme_are_config_keys_with_the_documented_enums(self):
        st, _, _, schema = req("GET", "/api/v1/config/schema")
        self.assertEqual(st, 200)
        sk = schema["properties"]["sk"]["properties"]
        # sk.tls (bool) is gone; sk.scheme (enum) replaced it.
        self.assertNotIn("tls", sk)
        self.assertEqual(sk["scheme"]["enum"], ["auto", "http", "https"])
        self.assertEqual(sk["scheme"]["default"], "auto")
        self.assertEqual(sk["tls_trust"]["enum"], ["tofu", "ca", "bundle"])
        self.assertEqual(sk["tls_trust"]["default"], "tofu")
        # The descriptor version was bumped for the rename.
        self.assertGreaterEqual(schema["properties"]["sk"]["x-espos-version"], 2)
        # ca_pem is a blob, capped, and travels base64 like any blob.
        self.assertEqual(sk["ca_pem"]["x-espos-type"], "blob")
        self.assertEqual(sk["ca_pem"]["x-espos-maxBytes"], 2048)

        st, _, _, js = req("PUT", "/api/v1/config", {"sk": {"scheme": "nonsense"}})
        self.assertEqual(st, 400, js)
        st, _, _, js = req("PUT", "/api/v1/config", {"sk": {"tls_trust": "anything"}})
        self.assertEqual(st, 400, js)   # there is no accept-anything mode
        st, _, _, js = req("PUT", "/api/v1/config", {"sk": {"scheme": "https", "tls_trust": "bundle"}})
        self.assertEqual(st, 200, js)
        st, _, _, js = req("GET", "/api/v1/config?ns=sk")
        self.assertEqual(js["sk"]["scheme"], "https")
        self.assertEqual(js["sk"]["tls_trust"], "bundle")
        # Neither needs a reboot any more: the stream rebuilds its transport.
        st, _, _, js = req("PUT", "/api/v1/config", {"sk": {"scheme": "auto", "tls_trust": "tofu"}})
        self.assertFalse(js["restart_required"], js)

    def test_03_put_ca_validates_the_pem_before_storing_it(self):
        if sk_tls() is None:
            self.skipTest("built without CONFIG_ESPOS_SK_TLS")
        for bad in ("hello", "-----BEGIN CERTIFICATE-----\nnot base64\n-----END CERTIFICATE-----\n"):
            st, _, _, js = req("PUT", "/api/v1/sk/tls/ca", {"pem": bad})
            self.assertEqual(st, 400, js)
            self.assertEqual(js["error"], "validation", js)
        st, _, _, js = req("PUT", "/api/v1/sk/tls/ca", {"nope": 1})
        self.assertEqual(st, 400, js)
        st, _, _, js = req("PUT", "/api/v1/sk/tls/ca", {"pem": "x" * 4000})
        self.assertEqual(st, 400, js)          # over ESPOS_SK_TLS_CA_MAX
        st, _, _, js = req("PUT", "/api/v1/sk/tls/ca", {"pem": "x"}, content_type=None)
        self.assertEqual(st, 415, js)
        # A rejected PEM changed nothing.
        st, _, _, js = req("GET", "/api/v1/config?ns=sk")
        self.assertEqual(js["sk"]["tls_trust"], "tofu")

        # A real certificate: on a device this is accepted, stored and
        # switches the mode — the fleet path, where the CA is pre-seeded so
        # the first connection is verified with nothing to capture.
        #
        # On THIS host it is rejected with mbedtls_x509_crt_parse -0x3b00
        # (PK_INVALID_PUBKEY), for the same reason the handshake cannot run:
        # the linux target has no startup framework, so the
        # ESP_SYSTEM_INIT_FN that calls psa_crypto_init()
        # (components/mbedtls/port/esp_psa_crypto_init.c) never runs, and
        # mbedTLS 4 routes RSA key parsing through PSA. Assert the endpoint's
        # *contract* either way rather than skipping the case outright: a 200
        # must store and switch, a 400 must change nothing.
        st, _, _, js = req("PUT", "/api/v1/sk/tls/ca", {"pem": CA_PEM})
        st2, _, _, cfg = req("GET", "/api/v1/config?ns=sk")
        if st == 200:
            self.assertEqual(js["trust"], "ca")
            self.assertEqual(cfg["sk"]["tls_trust"], "ca")
            # The blob comes back base64 and round-trips to what went in.
            self.assertEqual(base64.b64decode(cfg["sk"]["ca_pem"]).decode(), CA_PEM)
            # And the CA became the anchor immediately, with no handshake.
            js = wait_for(lambda: (lambda d: d if d and d["pinned"] else None)(sk_tls()), timeout=10)
            self.assertIsNotNone(js, sk_tls())
            self.assertEqual(js["pinned"]["kind"], "ca")
            self.assertEqual(len(js["pinned"]["fingerprint"]), 64)
            req("PUT", "/api/v1/config", {"sk": {"tls_trust": "tofu"}})
        else:
            self.assertEqual(st, 400, js)
            self.assertEqual(cfg["sk"]["tls_trust"], "tofu")   # nothing changed
            self.assertEqual(cfg["sk"]["ca_pem"], "")

    def test_04_delete_resets_the_anchor(self):
        if sk_tls() is None:
            self.skipTest("built without CONFIG_ESPOS_SK_TLS")
        st, _, _, js = req("DELETE", "/api/v1/sk/tls")
        self.assertEqual(st, 202, js)
        self.assertEqual(js["status"], "reset")
        js = wait_for(lambda: (lambda d: d if d and d["pinned"] is None else None)(sk_tls()), timeout=5)
        self.assertIsNotNone(js, sk_tls())

    def test_05_the_status_document_carries_the_scheme_in_use(self):
        st, _, _, cfg = req("GET", "/api/v1/config?ns=sk")
        js = wait_sk(lambda j: j["server"]["source"] == "discovered"
                     and j["server"].get("scheme") == "http", timeout=25)
        self.assertIsNotNone(js, (cfg["sk"]["scheme"], sk_status()))
        # sk.scheme is auto and the sim advertised the plaintext service type,
        # so the answer is http and no probe was needed.
        self.assertIn("cert_errors", js["token"]["counts"])
        st, _, _, srv = req("GET", "/api/v1/sk/servers")
        self.assertEqual(srv["servers"][0]["scheme"], "http")

    def test_07_a_manual_host_is_probed_and_the_probe_carries_no_token(self):
        # sk.scheme = auto with a manual host: one GET of the plain port,
        # unauthenticated (SensESP #1057 — the address has not been
        # established as our server yet, so it must not see the token).
        if sk_tls() is None:
            self.skipTest("built without CONFIG_ESPOS_SK_TLS")
        n0 = len([e for e in self.mock.ctl("log")[1] if e[0] == "GET" and e[1] == "/signalk"])
        # Discovery off empties the list: an address discovery has just seen
        # takes the scheme it advertised, and this test is about the probe.
        req("PUT", "/api/v1/config", {"sk": {"server_host": "127.0.0.1", "discovery": False,
                                             "server_port": self.mock.port, "scheme": "auto"}})
        # A plain server that does not redirect stays plain.
        js = wait_sk(lambda j: j["server"].get("source") == "manual"
                     and j["server"].get("scheme") == "http", timeout=20)
        self.assertIsNotNone(js, sk_status())
        probes = [e for e in self.mock.ctl("log")[1] if e[0] == "GET" and e[1] == "/signalk"]
        self.assertGreater(len(probes), n0, "the manual host was never probed")
        self.assertIsNone(probes[-1][2], probes[-1])
        req("PUT", "/api/v1/config", {"sk": {"server_host": None, "discovery": True}})


class SkSchemeAdvertisedTests(unittest.TestCase):
    """A discovered server that advertised _signalk-https._tcp: sk.scheme =
    auto reads the scheme off the advertisement, with no probe at all."""

    @classmethod
    def setUpClass(cls):
        cls.mock = MockSignalK()
        # The sim's fifth field is the https service type, which is how
        # signalk-server says its `ssl` setting is on.
        cls.h = Harness(fresh=True, extra_env={
            "ESPOS_SIM_SK_SERVERS": f"127.0.0.1,{cls.mock.port},{cls.mock.self_urn},secureboat,tls"})
        req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "secret12"}})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()
        cls.mock.stop()

    def test_01_the_advertisement_decides_and_nothing_is_probed(self):
        st, _, _, _ = req("GET", "/api/v1/sk/tls")
        if st == 404:
            self.skipTest("built without CONFIG_ESPOS_SK_TLS")
        js = wait_sk(lambda j: j["server"].get("scheme") == "https", timeout=30)
        self.assertIsNotNone(js, sk_status())
        st, _, _, srv = req("GET", "/api/v1/sk/servers")
        self.assertEqual(srv["servers"][0]["scheme"], "https")
        # The server said so, so nothing was asked of it to find out.
        self.assertEqual([e for e in self.mock.ctl("log")[1] if e[0] == "GET" and e[1] == "/signalk"], [])


class SkSchemeRedirectTests(unittest.TestCase):
    """The probe's other answer: a plain port that redirects to https."""

    @classmethod
    def setUpClass(cls):
        # 44300 is never listened on here; what matters is that the device
        # reads the scheme AND the port out of the Location header.
        cls.plain = MockSignalK(redirect_to="https://127.0.0.1:44300")
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_SK_SERVERS": ""})
        req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "secret12"}})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()
        cls.plain.stop()

    def test_01_a_302_to_https_selects_https_and_the_port_from_the_location(self):
        st, _, _, js = req("GET", "/api/v1/sk/tls")
        if st == 404:
            self.skipTest("built without CONFIG_ESPOS_SK_TLS")
        req("PUT", "/api/v1/config", {"sk": {"server_host": "127.0.0.1",
                                             "server_port": self.plain.port, "scheme": "auto"}})
        js = wait_sk(lambda j: j["server"].get("scheme") == "https", timeout=30)
        self.assertIsNotNone(js, sk_status())
        # signalk-server with ssl on commonly listens for TLS on another port,
        # so following the Location's host:port is the only way to land on it.
        self.assertEqual(js["server"]["port"], 44300)
        probes = [e for e in self.plain.ctl("log")[1] if e[0] == "GET"]
        self.assertTrue(probes, "the plain port was never probed")
        self.assertTrue(all(e[2] is None for e in probes), probes)


class SkSchemeProbeTimingTests(unittest.TestCase):
    """When the scheme probe runs, and what it may remember. A device boots
    before its network is up and, on a boat, usually before its server: a
    probe that reached nothing is not an answer."""

    @classmethod
    def setUpClass(cls):
        cls.mock = MockSignalK()
        cls.late = None
        # No WiFi credentials: the simulated network stays down until test_01
        # provides them.
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_SK_SERVERS": ""})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()
        cls.mock.stop()
        if cls.late:
            cls.late.stop()

    @staticmethod
    def probes(mock):
        return [e for e in mock.ctl("log")[1] if e[0] == "GET" and e[1] == "/signalk"]

    def test_01_a_manual_host_is_not_probed_before_the_network_is_up(self):
        if sk_tls() is None:
            self.skipTest("built without CONFIG_ESPOS_SK_TLS")
        st, _, _, net = req("GET", "/api/v1/net/status")
        self.assertFalse(net["up"], net)   # the premise: no network yet
        req("PUT", "/api/v1/config", {"sk": {"server_host": "127.0.0.1",
                                             "server_port": self.mock.port, "scheme": "auto"}})
        # Loopback reaches the mock whatever the simulated network says, so a
        # probe sent now would be in its log. Before this was fixed, it was,
        # and its answer was remembered for the address from then on.
        time.sleep(3)
        self.assertEqual(self.probes(self.mock), [])
        req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "secret12"}})
        self.assertTrue(wait_for(lambda: self.probes(self.mock) or None, timeout=20),
                        "the manual host was never probed once the network came up")
        js = wait_sk(lambda j: j["server"].get("source") == "manual"
                     and j["server"].get("scheme") == "http", timeout=20)
        self.assertIsNotNone(js, sk_status())

    def test_02_a_probe_nothing_answered_is_asked_again_not_remembered(self):
        if sk_tls() is None:
            self.skipTest("built without CONFIG_ESPOS_SK_TLS")
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        # Nothing listens there yet: the server is down, or still booting.
        req("PUT", "/api/v1/config", {"sk": {"server_host": "127.0.0.1",
                                             "server_port": port, "scheme": "auto"}})
        js = wait_sk(lambda j: j["server"].get("source") == "manual"
                     and j["server"].get("port") == port, timeout=20)
        self.assertIsNotNone(js, sk_status())
        self.assertEqual(js["server"]["scheme"], "http")   # a guess, for now
        # Then it comes up, and it is an https server. The device finds out on
        # its next attempt instead of talking plain http to it for ever.
        type(self).late = MockSignalK(redirect_to="https://127.0.0.1:44300", port=port)
        js = wait_sk(lambda j: j["server"].get("scheme") == "https", timeout=45)
        self.assertIsNotNone(js, sk_status())
        self.assertEqual(js["server"]["port"], 44300)


class SkPlaintextUnauthTests(unittest.TestCase):
    """Over plaintext a single 401 must not cost the token: anything on the
    path can produce one, and replacing a token costs an admin approval."""

    @classmethod
    def setUpClass(cls):
        cls.mock = MockSignalK(unauth_once=True)
        servers = f"127.0.0.1,{cls.mock.port},{cls.mock.self_urn},mockboat"
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_SK_SERVERS": servers})
        req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "secret12"},
                                      "sk": {"check_s": 3600}})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()
        cls.mock.stop()

    def test_01_one_stream_401_keeps_the_token_and_the_device_recovers(self):
        js = wait_sk(lambda j: j["token"]["state"] == "pending", timeout=25)
        self.assertIsNotNone(js, sk_status())
        self.mock.ctl("approve", js["client_id"])
        js = wait_sk(lambda j: j["token"]["state"] == "approved", timeout=20)
        self.assertIsNotNone(js, sk_status())
        # The stream's first upgrade is refused with 401 — the shape a captive
        # portal or a proxy takes. The device asks again rather than throwing
        # away a token that costs an admin approval to replace.
        ok = wait_for(lambda: (self.mock.ws_unauth_count > 0) or None, timeout=30)
        self.assertTrue(ok, "the stream never attempted an upgrade")
        time.sleep(2)
        self.assertTrue(sk_status()["token"]["has_token"])
        # ... and it connects by itself once the 401 stops.
        js = wait_sk(lambda j: j["ws"]["connected"], timeout=90)
        self.assertIsNotNone(js, sk_status())
        self.assertTrue(js["token"]["has_token"])


class PowerTests(unittest.TestCase):
    """espos_power's task on the host: the configuration, the status endpoint
    and a full wake up to its deadline. The host never sleeps, so the last step
    is a refused sleep that the status keeps reporting."""

    @classmethod
    def setUpClass(cls):
        # No WiFi credentials: the network never comes up, so a wake can only
        # end at its deadline.
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_SK_SERVERS": ""})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()

    def test_01_off_by_default(self):
        st, _, _, js = req("GET", "/api/v1/power")
        self.assertEqual(st, 200, js)
        self.assertEqual(js["mode"], "off")
        self.assertEqual(js["decision"], "stay")
        self.assertEqual(js["why"], "off")
        self.assertFalse(js["timer_wake"])      # the host is never a wake from sleep
        self.assertEqual(js["wake_count"], 0)
        self.assertEqual(js["interval_s"], 300)

    def test_02_mode_is_an_enum_and_the_limits_hold(self):
        st, _, _, schema = req("GET", "/api/v1/config/schema")
        power = schema["properties"]["power"]["properties"]
        self.assertEqual(power["mode"]["enum"], ["off", "cycle"])
        self.assertEqual(power["mode"]["default"], "off")
        st, _, _, js = req("PUT", "/api/v1/config", {"power": {"mode": "always"}})
        self.assertEqual(st, 400, js)
        st, _, _, js = req("PUT", "/api/v1/config", {"power": {"window_s": 5}})
        self.assertEqual(st, 400, js)            # below the 30 s floor: the way back in cannot be shut
        st, _, _, js = req("GET", "/api/v1/power")
        self.assertEqual(js["mode"], "off")      # nothing changed

    def test_03_a_wake_without_a_network_sleeps_at_its_deadline(self):
        st, _, _, js = req("PUT", "/api/v1/config",
                           {"power": {"mode": "cycle", "window_s": 30, "awake_max_s": 5, "publish_ms": 0}})
        self.assertEqual(st, 200, js)
        # Followed live: the window applies, measured from this boot.
        js = wait_for(lambda: (lambda d: d if d["mode"] == "cycle" else None)(req("GET", "/api/v1/power")[3]),
                      timeout=5)
        self.assertIsNotNone(js)
        self.assertEqual(js["deadline_ms"], 35000)
        if js["uptime_ms"] < 30000:
            self.assertEqual(js["why"], "window", js)
            self.assertEqual(js["decision"], "stay")
        # Past window + awake_max with no network: sleep. The host refuses, and
        # the status keeps saying what was decided rather than re-deciding.
        js = wait_for(lambda: (lambda d: d if d["decision"] == "sleep" else None)(req("GET", "/api/v1/power")[3]),
                      timeout=45)
        self.assertIsNotNone(js, req("GET", "/api/v1/power")[3])
        self.assertEqual(js["why"], "deadline", js)
        self.assertGreaterEqual(js["uptime_ms"], 35000)
        time.sleep(1)
        again = req("GET", "/api/v1/power")[3]
        self.assertEqual((again["decision"], again["why"]), ("sleep", "deadline"), again)
        # A configuration change is a new question.
        req("PUT", "/api/v1/config", {"power": {"mode": "off"}})
        js = wait_for(lambda: (lambda d: d if d["why"] == "off" else None)(req("GET", "/api/v1/power")[3]),
                      timeout=5)
        self.assertIsNotNone(js, req("GET", "/api/v1/power")[3])
        self.assertEqual(js["decision"], "stay")


if __name__ == "__main__":
    if not os.path.exists(ELF):
        print(f"missing {ELF}; build first (idf.py --preview set-target linux && idf.py build)")
        sys.exit(2)
    unittest.main(verbosity=2)
