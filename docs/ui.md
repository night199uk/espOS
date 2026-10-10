# Web UI (`ui/`) — M5

A Preact + TypeScript single-page app built with Vite, served by
`espos_httpd` from the LittleFS `storage` partition. It talks only to the
versioned REST API in [rest-api.md](rest-api.md) and gets live state over the SSE
stream — no polling, no coupling to firmware internals.

Pages: **Status** (WiFi, SignalK, device, access, last crash), **WiFi** (join /
scan / saved networks / portal), **SignalK** (token state and actions,
delta stream, discovered servers, manual server), **Config** (every
namespace rendered from `GET /config/schema`: types, ranges, enums,
secrets, restart-required marker, export/import JSON, reset section),
**Logs** (live log ring with filter, follow, download, runtime log level),
**OTA** (running image and slot state, confirm/rollback, manifest check
with the available build, install with progress, install from a URL,
update-source settings). Plus the **login page**, which is not a tab: it
replaces the app while the device wants a key this browser does not hold.

## Login

Authentication is the device's decision ([security.md](security.md)): with
`httpd.api_key` unset there is no login and the UI behaves as it always did.
Once a key is set, the shell asks `GET /api/v1/auth/status` at startup and
shows the login page when the answer is "required, not authenticated"; a
`401` on any later call (the session expired, the device rebooted, the key
changed) brings the page back. The page posts the key to `/api/v1/auth/login`
and gets the `espos_sid` cookie, which `fetch()` and `EventSource` then send
by themselves — every call goes out with `credentials: "same-origin"`. After
a login the app is mounted afresh with a new event stream, so nothing has to
retry what it failed to load while logged out. **Log out** in the header
posts `/api/v1/auth/logout`.

Two conveniences on the Config page and Status page: `httpd.api_key` has a
**Generate** button (20 characters from an alphabet without look-alikes,
shown once in full — write it down; after Save every browser, the designer
and any script need it), and Status has an **Access** card saying whether the
device is open or keyed and how this very page got in (cookie, Bearer, the
setup portal). A firmware built with `CONFIG_ESPOS_HTTPD_AUTH_REQUIRED=y`
and no key yet answers `403 auth_unconfigured`; the login page then explains
that the key is set from the portal network.

In development the page is not served by the device it talks to — the Vite
dev server proxies `/api` to a device or to the mock — so `Origin` never
equals `Host` and the device would refuse every cookie-authenticated save.
`npm run dev` therefore keeps the key in `sessionStorage` and sends it as
`Authorization: Bearer` on every call as well (the cookie is still taken, for
`EventSource`). The built bundle on a device uses the cookie only. A bundle
built for another origin sets `VITE_ESPOS_BASE` to an absolute API URL and
gets the same Bearer behaviour.

## Working on it — no hardware needed

```sh
cd ui
npm ci
npm run dev            # http://localhost:5173, API mock started in-process
ESPOS_API=http://192.168.0.118 npm run dev     # proxy /api to a real device…
ESPOS_API=http://127.0.0.1:<port> npm run dev  # …or to the host harness (test/host/espos_httpd_test)
```

`mock/server.ts` (Node runs it as TypeScript directly, zero deps) implements the API contract with a
simulated WiFi state machine, discovery + token flow, a log ring, SSE, and
the authentication (`/auth/*`, `/system/ping`, Bearer and cookie, the
throttle; set `httpd.api_key` on the Config page to see the login page), and
regenerates the config schema from the real descriptors via
`components/espos_config/tools/espos_gen_config.py` when python3 is present. It is the reference
"device" for UI development; when the API changes, change the mock and the
docs together.

`npm run build` type-checks (strict, `noUncheckedIndexedAccess`), bundles
(~18 KiB gzipped in total) and writes `components/espos_httpd/ui-dist/` — every file gzipped as
`<name>.gz`, nothing else. The root `CMakeLists.txt` turns `components/espos_httpd/ui-dist`
into `build/storage.bin` (`littlefs_create_partition_image`,
`FLASH_IN_PROJECT`), so `idf.py flash` writes it. Without a UI build the
firmware still builds and serves the embedded placeholder page (`GET
/system/info` → `ui_storage`, and the boot log says so).

## Serving rules (firmware side)

* `<path>.gz` first, sent with `Content-Encoding: gzip` regardless of
  `Accept-Encoding` (all browsers accept it; `curl --compressed`).
* `/assets/*` is content-hashed → `Cache-Control: immutable` for a year;
  `index.html` → `no-cache`, so a new bundle is picked up on reload.
* Unknown extension-less paths → `index.html` (SPA routing with
  `history.pushState`); unknown files with an extension → JSON 404.
* Static serving is the 404 fallback for `GET`, not a wildcard handler, so
  API handlers registered later by other components are never shadowed.
* `..` and `//` in a path are refused.

## Design notes

* State lives in tiny subscribable stores fed by one `EventSource`
  (`src/api.ts`); pages `useStore()` what they show. `EventSource`
  reconnects on its own (`retry: 3000`), the header shows the link state.
  A stream the device *refused* (a `401` after the session ended) is closed
  by the browser for good; `api.ts` notices, asks `/auth/status`, and either
  reopens it or hands over to the login page.
* `authStore` (`open | ok | login | unconfigured`) is what `mount.tsx`
  renders from: the app, keyed by session so a fresh login remounts it, or
  the login page. Every helper in `api.ts` flips it to `login` on a `401`.
* No component library, no router package: a 30-line history router and
  ~120 lines of CSS with light/dark via `prefers-color-scheme`.
* The Config page is generic: adding a key to a descriptor JSON adds a
  field. Secrets render as set/not-set with Set/Change/Clear (the sentinel
  `********` is never sent back as a value). Numbers/enums/booleans get the
  matching control; `x-espos-unit`, ranges and `maxLength` are shown as
  hints; `x-espos-restartRequired` keys carry ↻ and a save of one offers a
  reboot.
* Destructive actions (reboot, factory reset, forget network/token,
  erase core dump, reset section) confirm first.

## Log ring (`espos_log`)

`espos_log_init()` hooks `esp_log_set_vprintf`: each console line is also
kept in a byte ring (`CONFIG_ESPOS_LOG_RING_SIZE`, default 16 KiB) as
`[u16 len][text]`, colour codes stripped, sequence-numbered, oldest
overwritten first; the previous vprintf still runs, so the console is
unchanged. Log v2's prefix/message/newline calls are gathered into one
record; a message longer than `CONFIG_ESPOS_LOG_LINE_MAX` is truncated
and closed. A FreeRTOS timer (500 ms) publishes the `logs` SSE event when
new lines arrived — never from inside the logging call, so nothing can
recurse or block a logger on a slow SSE client. `main.c` calls it before
anything else to catch the boot log; `espos_httpd_start()` calls it too
(idempotent).

## Pages from a firmware

The page list is a registry, not a constant: `ui/src/routes.ts` holds the core
pages and `registerPage()` adds to it, the same shape as
`espos_httpd_register()` and `espos_config_add_descriptor()` on the C side.

A firmware that wants its own page keeps a small Vite project of its own and
uses espOS's entry point:

```ts
// <firmware>/ui/src/main.tsx
import { registerPage, mount } from "../../espos/ui/src/mount";
import { TanksPage } from "./pages/tanks";

registerPage({ path: "/tanks", title: "Tanks", page: TanksPage, order: 35 });
mount();
```

`order` places the tab (core pages sit on 10–70); registering an existing
path replaces that page, which is how a firmware puts its own Status screen
in front. Build it the same way espOS builds its own — `npm run build` to
its own `dist-gz/`, then point `espos_project_ui_partition(DIR ...)` at it.

### Pages that are not always there

A route may declare `available()`, awaited once at startup; the tab appears
only if it resolves true. The BLE page uses it:

```ts
{ path: "/ble", title: "BLE", page: BlePage, available: () => endpointExists("/ble/status") }
```

`endpointExists()` treats a 404 as "the component is not in this build" and
anything else — a 500, a dropped connection — as no evidence either way, so a
momentarily busy device does not lose a tab. The shell paints the ungated
pages first and adds the rest when they answer, rather than holding a blank
page while it decides.

The Flow page does the same against `/flow`, so it appears on a firmware that
builds `espos_flow` and nowhere else — neither of espOS's own consumers links
it, and neither shows the tab. Two pages, two components, no firmware
declaring anything.
