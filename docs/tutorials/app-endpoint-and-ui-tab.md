# Tutorial: an endpoint, live events and a UI tab

**Advanced.** Your firmware grows a REST endpoint of its own under `/api/v1`,
pushes changes to browsers over the existing SSE stream, and adds a tab to the
web UI — the three registration points espOS has for extending the device, in the
shape of `components/espos_httpd/examples/app_endpoint_and_page`. Start from [tank-level](tank-level.md)
(a value worth showing) or the minimal example; the contract you extend is [rest-api.md](../rest-api.md).

## 1. The endpoint

`espos_httpd_register()` takes an `esp_http_server` `httpd_uri_t`, and the server
must be running, so register after `espos_start()`. Handlers run on the one HTTP server
task: build the answer, send, return — anything slow stalls the whole UI. Paths under
`/api/v1/app/` belong to the application; espOS never uses that prefix. Add `espos_httpd` to `PRIV_REQUIRES`.

```c
#include <stdio.h>
#include "espos_httpd.h"
#include "espos_httpd_sse.h"
static float s_level; static int32_t s_readings; /* written by the sensor loop, read by the handlers: word-sized, no lock */

static int tank_json(char *buf, size_t n)
{
    return snprintf(buf, n, "{\"level\":%.3f,\"readings\":%ld}", s_level, (long)s_readings);
}
static esp_err_t tank_get(httpd_req_t *req)
{
    char buf[64];
    tank_json(buf, sizeof(buf));
    return espos_httpd_send_json(req, NULL, buf);           /* 200, application/json */
}
static esp_err_t tank_zero_post(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;                                       /* 415 already sent: the CSRF guard of rest-api.md */
    }
    s_readings = 0;
    return espos_httpd_send_json(req, "202 Accepted", "{\"status\":\"zeroed\"}");
}
static const httpd_uri_t tank_uris[] = {
    { .uri = "/api/v1/app/tank", .method = HTTP_GET, .handler = tank_get },
    { .uri = "/api/v1/app/tank/zero", .method = HTTP_POST, .handler = tank_zero_post },
};
```

Errors keep the contract's shape through `espos_httpd_send_error(req, "404 Not Found", "not_found", "…")`; a body comes from `espos_httpd_read_body()` (malloc'ed, NUL-terminated; the `413` for an oversize one is sent for you).

## 2. Live updates over SSE

One stream, `GET /api/v1/events`, carries every component's events. Publish yours
from the sensor loop, and hand each newly connected client a snapshot — the UI subscribes once and expects current state:

```c
static void tank_on_connect(int client, void *arg)   /* server task: send and return */
{
    (void)arg;
    char buf[64];
    tank_json(buf, sizeof(buf));
    espos_httpd_sse_send(client, "app.tank", buf);
}
/* app_main(), after espos_start(): */
for (size_t i = 0; i < sizeof(tank_uris) / sizeof(tank_uris[0]); i++) {
    ESP_ERROR_CHECK(espos_httpd_register(&tank_uris[i]));
}
ESP_ERROR_CHECK(espos_httpd_sse_on_connect(tank_on_connect, NULL));
/* the sensor loop, after each reading: */
s_level = level, s_readings++;
char buf[64];
tank_json(buf, sizeof(buf));
espos_httpd_sse_publish("app.tank", buf);             /* any task; ESP_OK even with no client */
```

Sends are serialised with a 250 ms per-socket timeout, so a stalled browser costs the loop at most that; `CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS` (3) streams at once, the oldest evicted when full. Flash and check:

```sh
D=http://espos-xxxx.local/api/v1
curl -s $D/app/tank                                                    # {"level":0.583,"readings":42}
curl -s -X POST -H 'Content-Type: application/json' $D/app/tank/zero  # {"status":"zeroed"}
curl -s -X POST $D/app/tank/zero                                       # 415 unsupported_media_type
curl -N $D/events                                                      # event: app.tank … on connect, then on every reading
```

## 3. A tab in the web UI

The page list is a registry ([ui.md](../ui.md)): your firmware keeps a small Vite
project and calls espOS's entry point with its page registered first. Create `ui/` in
the project (Node 22.18+): copy `package.json`, `tsconfig.json`, `tsconfig.node.json`, `index.html` and `scripts/gzip-dist.ts` from `espos/ui/`;
copy `vite.config.ts` too, minus the mock plugin (keep `preact()`, `build` and the `/api` proxy). Then two files:

```ts
// ui/src/main.tsx
import { registerPage, mount } from "../../espos/ui/src/mount";
import { TankPage } from "./tank";
registerPage({ path: "/tank", title: "Tank", page: TankPage, order: 35 }); // between SignalK (30) and Config (50)
mount();
```

```tsx
// ui/src/tank.tsx — the shell's EventSource wires only the core events, so the page listens for its own
import { useEffect, useState } from "preact/hooks";
import { BASE, get, post } from "../../espos/ui/src/api";
interface Tank { level: number; readings: number }
export function TankPage() {
  const [t, setT] = useState<Tank>();
  useEffect(() => {
    void get<Tank>("/app/tank").then(setT);
    const es = new EventSource(BASE + "/events");      // the second of the three SSE slots; closed on leave
    es.addEventListener("app.tank", (e) => setT(JSON.parse((e as MessageEvent).data) as Tank));
    return () => es.close();
  }, []);
  return (<><h1>Tank</h1><p>{t ? `${Math.round(t.level * 100)} % after ${t.readings} readings` : "…"}</p>
    <button onClick={() => void post("/app/tank/zero")}>Zero the counter</button></>);
}
```

`npm ci && npm run build` in `ui/` writes `ui/dist-gz/`; point the partition at
it in the root `CMakeLists.txt` — `espos_project_ui_partition(DIR "${CMAKE_CURRENT_LIST_DIR}/ui/dist-gz")` —
rebuild and flash. `http://espos-xxxx.local/tank` shows the new tab, updating with every reading;
`ESPOS_API=http://espos-xxxx.local npm run dev` develops the page against the live device without
reflashing. Commit `dist-gz/` with the source, as espOS does: a firmware build must never depend on Node.
