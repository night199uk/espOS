# Configuration store (`espos_config`)

## One source of truth: the config descriptor

Every component that owns settings ships **one JSON descriptor per NVS
namespace** and registers it from its `CMakeLists.txt`:

```cmake
idf_component_register(...)
espos_config_add_descriptor(config/myns.json)   # after idf_component_register
```

At build time `components/espos_config/tools/espos_gen_config.py` merges all
registered descriptors into

* `espos_cfg_keys.h` — `ESPOS_CFG_NS_<NS>` / `ESPOS_CFG_<NS>_<KEY>` name
  constants (never spell an NVS key by hand),
* the C descriptor tables `espos_cfg_namespaces[]` used at runtime for
  defaults, type checks and range validation,
* the JSON Schema served at `GET /api/v1/config/schema`.

Registration order does not matter; the generator runs after every
component's CMake has been processed. Duplicate namespaces, reserved names
and NVS length limits (15 chars for namespace and key) fail the build.

### Descriptor format

```json
{
  "namespace": "httpd",           // ^[a-z][a-z0-9_]{0,14}$
  "version": 1,                   // bump on incompatible layout change (see Migrations)
  "title": "HTTP server",
  "description": "optional",
  "keys": [
    {"name": "port", "type": "int", "default": 80, "min": 1, "max": 65535,
     "title": "TCP port", "description": "…", "unit": "", "restart_required": true}
  ]
}
```

| Field              | Types            | Notes                                                        |
|--------------------|------------------|--------------------------------------------------------------|
| `type`             | all              | `bool` `int` (int32) `float` `string` `blob`                 |
| `default`          | all but blob     | required to be valid against the constraints; blob default is empty |
| `min` / `max`      | int, float       | inclusive                                                     |
| `maxLength`        | string, blob     | bytes (string: excluding NUL, ≤ 3999; blob ≤ 508000). Default 256 / 1024; for `enum` keys defaults to the longest value |
| `enum`             | string           | allowed values                                                |
| `pattern`          | string           | regex, **schema only** (UI validates; device checks type/length/enum) |
| `secret`           | string, blob     | redacted on export, sentinel ignored on import                |
| `restart_required` | all              | surfaces in the PUT response and the schema                   |
| `unit`             | all              | display hint (`x-espos-unit`)                                 |
| `readOnly`         | all              | shown, never editable; a write returns `ESP_ERR_NOT_SUPPORTED` and an import ignores the member |
| `group`            | all              | UI tab within the namespace (`x-espos-group`), ≤ 24 chars     |
| `x`                | see below        | presentation only: `displayMultiplier`, `displayOffset`, `format`, `columns` |

Key name `config_version` is reserved.

### Presentation: the `x` block

The device stores SI — radians, seconds, metres — because SignalK requires
it. Nobody trims an anchor rode in radians, so the *UI* converts:

```
display = stored * displayMultiplier + displayOffset
```

and writes back the exact inverse. Both fields are numbers, int and float
keys only, and a multiplier of zero is rejected (it is not invertible).
Omitting the block leaves the identity transform, so every descriptor written
before this existed renders unchanged.

```json
{"name": "heading", "type": "float", "unit": "rad",
 "x": {"displayMultiplier": 57.29578}}          // radians stored, degrees typed
{"name": "runtime", "type": "int", "unit": "s",
 "x": {"displayMultiplier": 0.000277778}}       // seconds stored, hours shown
```

`"format": "table"` marks a **string** key whose value is a JSON array of
rows; `columns` names the members, in order, and the UI renders a row editor.
It is a string and not a blob on purpose: an export stays readable and
diffable, and the editor never round-trips through base64. Such a key
defaults to `"[]"` and to the full NVS string budget of 3999 bytes — roughly
250 numeric points.

```json
{"name": "curve", "type": "string",
 "x": {"format": "table", "columns": ["input", "output"]}}
```

## Runtime API (`espos_config.h`)

* `espos_config_init(NULL, NULL)` — NVS backend on `CONFIG_ESPOS_CONFIG_NVS_PARTITION`
  (default `"nvs"`). Erases and re-inits the partition if NVS reports it
  unusable (`ESP_ERR_NVS_NO_FREE_PAGES`, `ESP_ERR_NVS_NEW_VERSION_FOUND`);
  `espos_config_storage_was_reset()` tells you it happened.
* Typed getters never fail for declared keys: a missing, wrong-typed,
  out-of-range or over-long stored value **falls back to the compiled-in
  default** (and `espos_config_is_set()` reports false).
* Typed setters validate first (`ESP_ERR_INVALID_ARG`), write, commit, and
  fire change callbacks (`espos_config_subscribe`) only when the effective
  value changed. Callbacks run on the caller's task without the store lock.
* `espos_config_export_json` / `espos_config_import_json` implement the
  document semantics in `docs/rest-api.md`; import validates everything before
  writing anything.
* `espos_config_factory_reset()` erases the partition and re-initialises;
  the caller reboots.
* `espos_config_register_ns()` / `espos_config_unregister_ns()` add and remove
  namespaces no build-time descriptor declares (see below), and
  `espos_config_schema_json()` serves the merged schema.

Thread safety: one internal mutex around all storage access. Migrations run
inside `espos_config_init()` before anything else can touch the store.

## Namespaces registered at run time

A graph node built at run time — `Linear("cal", …)` with its own multiplier
and offset — has no CMakeLists of its own to register a descriptor from, yet
its settings must appear in the web UI, validate, and survive a reboot like
any other. It builds an `espos_cfg_ns_t` (a static `ParamSet` inside the node)
and hands it over:

```c
static const espos_cfg_key_t cal_keys[] = {
    { .name = "mul", .title = "Multiplier", .description = "", .unit = "",
      .type = ESPOS_CFG_TYPE_FLOAT, .def.f = 1.0f },
    { .name = "off", .title = "Offset", .description = "", .unit = "",
      .type = ESPOS_CFG_TYPE_FLOAT, .def.f = 0.0f },
};
static const espos_cfg_ns_t cal_ns = {
    .name = "f_cal", .title = "Calibration", .version = 1,
    .keys = cal_keys, .key_count = 2, .description = "Linear node cal",
};
espos_config_register_ns(&cal_ns);
```

From that moment the namespace is indistinguishable from a compiled one:
typed getters and setters, validation, `espos_config_export_json` /
`espos_config_import_json`, and a section in the schema served at
`GET /api/v1/config/schema` (marked `"x-espos-runtime": true` so the UI can
tell them apart).

* **Ownership.** The descriptor is *borrowed*, never copied — it, its key
  array and every string it points at must outlive the registration. Static
  storage in the node is the intended shape.
* **Naming.** NVS caps a namespace at 15 characters, so a node id of at most
  12 becomes `f_<id>`; `espos_config_flow_ns_name()` builds and checks it.
* **Failures are loud.** A duplicate name, a name that collides with a
  built-in, an over-long id, a malformed descriptor or a full table
  (`CONFIG_ESPOS_CONFIG_MAX_RUNTIME_NS`, default 32) all log an error *and*
  raise the health condition `flowConfig`. A node whose settings silently do
  not appear looks like a UI bug for as long as nobody reads the log.
* **Unregistering** removes the section but leaves the stored values in NVS,
  so a graph rebuilt on the next boot finds its calibration where it was.
  Call `espos_config_reset_ns()` first to actually discard them. After
  `espos_config_unregister_ns()` returns, the descriptor memory may be freed —
  the caller must have stopped every other reader first.
* Registering before `espos_config_init()` works; the namespace is opened
  along with the static ones. Registering afterwards is the normal case.

`espos_config_schema_json()` merges the compiled properties with the runtime
ones. With nothing registered it returns the compiled document byte for byte
under the compiled ETag, so a cached browser keeps its 304; once a node
registers, the ETag changes (compiled etag hashed with a generation counter
that moves on every register and unregister).

### NVS capacity

The `nvs` partition is 128K in every `components/espos_core/partitions/*.csv` — thirty-two
4096-byte pages, 126 entries of 32 bytes each. Devices first flashed with
espOS 0.18 or older keep the 48K (twelve pages) they were flashed with, because
an OTA never resizes a partition, so the budget below is measured against 48K.
A realistic graph of **8 nodes with 3 parameters each plus one 250-point
curve table** costs, measured by
`test/host/espos_config_test` ("a realistic graph's NVS footprint is
reported"):

| Item                                        | Entries | Bytes |
|---------------------------------------------|---------|-------|
| 8 × (mul, off, `config_version`)             | 24      | 768   |
| curve namespace `config_version`             | 1       | 32    |
| 8 × label string ("sensor", 7 bytes)         | 16      | 512   |
| 250-point curve string (2336 bytes)          | 75      | 2400  |
| **total**                                    | **116** | **3712** |

That is **one of the twelve pages**, and the built-in namespaces occupy a
fraction of another.

The partition was 24K through v0.7 and the arithmetic above fits that too, but
not with much left over. NVS keeps at least one page free to compact into
(`ESP_ERR_NVS_NO_FREE_PAGES` otherwise), and a rewritten key consumes fresh
entries until the old page is reclaimed, so six pages meant five usable and a
graph of this size already reached three of them. Past roughly five curve
tables a device would have started failing writes it had accepted the day
before.

The size is fixed when the table is flashed: growing it later needs a USB
flash, not an OTA. It was doubled in the release that introduced editable
tables rather than after devices were carrying them.

## Migrations

Each namespace stores its descriptor `version` under `config_version`. On
init:

| Stored vs. current    | Action                                                    |
|-----------------------|-----------------------------------------------------------|
| absent                | fresh namespace: stamp current version                    |
| corrupt / wrong type  | re-stamp current version (values are validated on read anyway) |
| equal                 | nothing                                                   |
| older                 | run steps `stored→stored+1 … current-1→current`, stamping after each |
| newer (downgrade)     | leave alone, warn                                         |

Register steps before init:

```c
static esp_err_t app_migrate_1_to_2(espos_config_migrate_ctx_t *ctx, void *arg)
{
    int32_t seconds; size_t n = sizeof(seconds);
    if (espos_config_migrate_get(ctx, "speed", ESPOS_CFG_TYPE_INT, &seconds, &n) == ESP_OK) {
        int32_t ms = seconds * 1000;
        espos_config_migrate_set(ctx, "speed_ms", ESPOS_CFG_TYPE_INT, &ms, sizeof(ms));
        espos_config_migrate_erase(ctx, "speed");
    }
    return ESP_OK;
}
espos_config_register_migration(ESPOS_CFG_NS_APP, 1, app_migrate_1_to_2, NULL);
```

A step without a registered function is treated as **additive** (new keys
read their defaults). A failing step stops the chain; the stored version
stays where it was and the step is retried on the next boot. Migration
callbacks use raw accessors so they can read a key in its *old* type; the
NVS backend handles a type change by erase-and-rewrite.

Rules of thumb: adding a key → no bump needed (defaults do the work);
renaming, retyping, changing units or tightening a range → bump and write a
step.

## Storage mapping

| Descriptor type | NVS type | Note                              |
|-----------------|----------|-----------------------------------|
| bool            | u8       | 0/1; anything else reads as default |
| int             | i32      |                                   |
| float           | u32      | IEEE-754 bit pattern              |
| string          | str      | ≤ 4000 bytes incl. NUL            |
| blob            | blob     | empty blob == key erased          |

Secrets go into the same partition; enabling `CONFIG_NVS_ENCRYPTION`
(automatic with flash encryption) encrypts the whole partition
transparently — see `docs/security.md`.

## Testing without hardware

`test/host/espos_config_test` runs the store on the linux target against an
in-memory backend (with fault injection) **and** the real NVS backend on
IDF's file-backed flash emulation, including the "corrupt partition →
defaults" path. See `docs/development.md`.
