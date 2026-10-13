# Development

## Prerequisites

* ESP-IDF 6.0.x installed and exported (`. $IDF_PATH/export.sh`). The
  release named in `.idf-version` is the one CI tests; any other 6.0.x
  builds with one warning, versions outside `[6.0.0, 6.1.0)` are refused
  unless `-DESPOS_ALLOW_IDF_MISMATCH=1`.
* For host tests: `libbsd-dev` (Debian/Ubuntu) — required by IDF's linux
  target.
* Python ≥ 3.10 (IDF's own venv is used at build time; the generator needs
  only the standard library).

## Build & flash the example app

```sh
. $IDF_PATH/export.sh                                    # ESP-IDF 6.0.x, see .idf-version
scripts/build.sh -DIDF_TARGET=esp32c6                    # any of: esp32 esp32s3 esp32c3 esp32c6 esp32p4
scripts/build.sh flash monitor
```

`scripts/build.sh` is the build entry point: one lock per machine, half the
cores, `nice`/`ionice`, `idf.py reconfigure` followed by a capped `ninja`.
Anything that is not a plain build (`flash`, `monitor`, `menuconfig`,
`set-target`) it hands to `idf.py` unchanged, still locked. When `ccache` is
on `PATH` it is enabled automatically (`IDF_CCACHE_ENABLE`; IDF wires the
launcher in on the *first* configure of a build directory, so recreate the
directory to pick it up).

There is no `sdkconfig.defaults` in the root. The example app is configured
exactly like every consumer: `espos_project_prologue()` puts
`sdkconfig.d/espos.defaults` (IDF adds the `.esp32`/`.esp32p4` sibling for
those targets) and the partition table on `SDKCONFIG_DEFAULTS`. Bench-local
overrides go in `sdkconfig.local` next to the root `CMakeLists.txt`
(git-ignored, never committed); a profile is selected with
`-DESPOS_PROFILE=debug` or `release` (`sdkconfig.d/<profile>.defaults`, each
commented). After changing any defaults file delete the build's `sdkconfig`
— IDF applies defaults only to options the existing `sdkconfig` does not
already set.

## Web UI

`components/espos_httpd/ui-dist/` — the gzipped Vite bundle the firmware builds into its app
image, and packs into the LittleFS `storage` image (`build/storage.bin`) — is
committed. Building a firmware therefore needs no Node. The bundle is absent
only in a damaged checkout, and the configure stops with an error then rather
than ship the placeholder page.

Node is needed to *change* the UI: `npm ci && npm run build` in `ui/`
regenerates `components/espos_httpd/ui-dist/`, and the result is committed with the source
change. CI rebuilds the bundle and fails when the committed one differs, so a
UI change without its `npm run build` does not merge. `npm run dev` runs the
UI against an API mock; see [ui.md](ui.md).

## Documentation site

The docs are the Markdown in `docs/` — plus the root `CHANGELOG.md`, which a
hook renders as the changelog page — built with mkdocs-material into
[signalk-espos.github.io/espOS](https://signalk-espos.github.io/espOS/) by
`.github/workflows/docs.yml`: every pull request runs `mkdocs build
--strict`, which fails on a dead link, a missing heading a link names, or a
page the nav lists that does not exist; a push to `main` deploys. The C API
pages are generated from the public headers by Doxygen (`Doxyfile`) through
the mkdoxy plugin, so Doxygen has to be installed for the site to build at
all. Locally, from the repository root (mkdoxy resolves its source
directories against the working directory):

```sh
sudo apt-get install doxygen                      # a system package, not a Python one
python3 -m venv ~/.venvs/espos-docs && . ~/.venvs/espos-docs/bin/activate
pip install -r docs/requirements.txt              # exact pins; bump them deliberately
mkdocs serve                                      # http://127.0.0.1:8000, rebuilds on save
mkdocs build --strict                             # what CI runs; output in site/ (git-ignored)
```

A new page goes into `docs/` and into the `nav:` of `mkdocs.yml`; a page
that is not in the nav is still built and searchable, but unreachable from
the menu. Doxygen warnings about a header (an unknown `<tag>` in prose, an
undocumented parameter) are printed by the build and do not fail it; fix
them in the header. `doxygen Doxyfile` alone reproduces them without mkdocs.

## Building a firmware on espOS

A firmware project vendors espOS as a submodule (`espos/`) and includes the
shared prologue; that is the whole of the espOS-specific build glue — no
copied `sdkconfig.defaults`, no copied partition table:

```cmake
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/espos/cmake/espos_project.cmake")
espos_project_prologue(NAME "my-firmware"
                       PARTITIONS "${ESPOS_PARTITIONS_DIR}/16mb.csv"
                       COMPONENTS espos_ble)      # the optional espOS parts this firmware uses
project(my_firmware)
espos_project_ui_partition()
```

`espos_project_prologue()`:

* enforces the IDF version policy (`cmake/espos_version.cmake`):
  the release named in `.idf-version` is what CI tests; another release in
  `[6.0.0, 6.1.0)` builds with one warning naming the tested one; anything
  else is refused with the install link unless `-DESPOS_ALLOW_IDF_MISMATCH=1`.
  A project that keeps its own `.idf-version` must keep it equal to espOS's
  — disagreeing pins are a hard error, not a silent choice between the two.
* assembles `SDKCONFIG_DEFAULTS`, later files winning:
  `espos/sdkconfig.d/espos.defaults` (IDF appends the `.<target>` sibling
  itself), the optional profile (`PROFILE release` on the call, or
  `-DESPOS_PROFILE=release` on the command line, which wins), your
  `sdkconfig.defaults` (and its `.<target>`), your git-ignored
  `sdkconfig.local`, any files named in
  `-DESPOS_EXTRA_SDKCONFIG_DEFAULTS=a;b` (a board variant, say), then a
  generated fragment that selects the partition table. Your
  `sdkconfig.defaults` holds only what differs from espOS — a BLE gateway's is
  the Bluetooth stack and nothing else. An explicit `-DSDKCONFIG_DEFAULTS=a;b`
  replaces the assembled list, espOS's signed-update settings included (only
  the partition fragment is still appended, so the OTA slots cannot silently
  vanish); to add files, use `ESPOS_EXTRA_SDKCONFIG_DEFAULTS`.
* selects the partition table with `PARTITIONS` (absolute, or relative to the
  project). Bundled: `espos/components/espos_core/partitions/4mb.csv` (the default), `8mb.csv`,
  `16mb.csv` — same layout, bigger app slots and storage, and each sets the
  matching flash size. A project's own CSV sets `CONFIG_ESPTOOLPY_FLASHSIZE_*`
  in its own defaults.
* turns on IDF's `MINIMAL_BUILD`, so only what `main/` requires (transitively)
  is compiled: `espos_ble`, `espos_n2k`, `espos_voice` cost nothing unless a
  component requires them. espOS's own tree keeps the full set so its CI
  covers them.
* puts `espos/components` on `EXTRA_COMPONENT_DIRS`, sets `PROJECT_VER` from
  your project's own `git describe`/`version.txt` (docs/releasing.md), and
  manages the app-signing key — including forcing a re-link when the key
  changes, without which a rebuilt image keeps the *previous* key's signature
  and the device rejects every OTA.

`espos_project_ui_partition()` (after `project()`) packs the committed
`espos_httpd/ui-dist` into the LittleFS `storage` partition; `PARTITION` and
`DIR` change that, and a custom `DIR` that is missing is a warning, not an
error. Further prologue options: `IDF_VERSION_FILE`, and `SIGNING_KEY` for a
key kept outside the project ([ota.md](ota.md), "Signing key").

A project that installs espOS's components from the registry and writes its
own `CMakeLists.txt` bypasses the prologue. `espos_core`'s
`project_include.cmake` then checks the sdkconfig values whose absence shows
only in the field — the event and timer task stacks, and on the ESP32-P4
PSRAM itself, the L2 cache line with the hosted mempool in PSRAM and the
block-ack window with PSRAM — and fails the configure with the exact line to
add.

## Multi-target

One build directory per target, each with its own `sdkconfig`:

```sh
for t in esp32 esp32s3 esp32c3 esp32c6 esp32p4; do
  scripts/build.sh -B build-$t -DSDKCONFIG=build-$t/sdkconfig -DIDF_TARGET=$t build || break
done
```

`managed_components/` is synced to the current target's dependency set
(ESP32-P4 pulls extra components), so build targets one after another, not
concurrently, from the same checkout — the lock in `scripts/build.sh` sees to
that.

## Host tests (no hardware)

On a shared development host, run a host test suite under the same lock the
build wrapper uses — `flock ~/.cache/.idf-build-$(id -u).lock ./test/host/run_all.sh` —
because another `set-target` on the same test project deletes the ELF a
running `run_test.py` is talking to.

`./test/host/run_all.sh` discovers every `test/host/*/` project (any
directory there with a `CMakeLists.txt`), builds it for the linux target and
runs it: `run_test.py` where a project has one, otherwise the Unity ELF from
its `build/`. It is what CI runs, so a new test directory is covered the
moment it exists; pass project names to run a subset. One project by hand:

```sh
cd test/host/espos_sk_test
idf.py --preview set-target linux && idf.py build
./build/espos_sk_test.elf                  # Unity; exit code 0 == pass
```

`test/host/espos_httpd_test` is the one with a `run_test.py`: it drives the
real REST server over HTTP -- config, WiFi status/scan/SSE with the simulated
driver, static serving (`ESPOS_WWW_DIR`, gzip, SPA fallback, cache headers),
`/logs` paging and the `logs` SSE event, the core-dump endpoints' host
behaviour, the OTA task against the sim port with a throwaway firmware
server, and the SignalK token/stream flows against a Python mock of
signalk-server. The rest are Unity binaries over the pure-C/C++ parts of one
component each.

## Flashing the Waveshare ESP32-P4 panels

`sdkconfig.d/espos.defaults.esp32p4` carries the SDIO pinout of the C6 co-processor
and allows the rev-1.x silicon those boards use. `idf.py -p /dev/ttyACM0
flash` as usual; credentials via the portal or an NVS image (docs/wifi.md).

`components/espos_config/tools/espos_gen_config.py` has its own tests:
`python3 -m unittest discover -s components/espos_config/tools -p 'test_*.py'`.

## Repository layout

```
components/espos_config/   NVS-backed config store, descriptor tables, JSON, migrations
components/espos_httpd/    esp_http_server + REST API + SSE + static UI
components/espos_wifi/     WiFi state machine, portal, /wifi endpoints
components/espos_sk/       SignalK discovery, token state machine, /sk endpoints
components/espos_ble/      BLE gateway to signalk-server's BLE provider API (optional)
main/                      example app
tools/                     tree-wide checks (Kconfig docs, public headers) and the size report
test/host/                 linux-target unit/integration tests
docs/                      contracts and guides
ui/                        Vite SPA (M5)
```

## Conventions

* Small, reviewable commits; one milestone per branch.
* Ask before adding a third-party dependency (`idf_component.yml`).
* `docs/rest-api.md` is a contract; changes there are discussed first.
* No absolute paths or machine-specific config in committed files.
* Warnings are errors (IDF 6 default); keep the build clean.

## Public API rules

The headers under `components/*/include` are the public API, and their C ABI
is the stable contract ([decisions.md](decisions.md), 2026-09-07): a binding
in another language is generated from them as they are (`bindgen` for Rust),
so whatever a header contains, every consumer gets.
`tools/check_public_headers.py` checks the rules below (CI's `headers` job;
standard library only, no IDF environment needed).

* **Includes.** The C standard library, other espOS public headers and
  `esp_err.h` — nothing else. `esp_err_t` is the return type of the whole
  API, which is why it is the one IDF type allowed. Two more exceptions exist
  and are **frozen** (the script's `ALLOWLIST`; adding one is a decision for
  decisions.md, not a convenience):
  * `espos_event.h` → `esp_event.h`: the thing on offer is the IDF default
    event loop itself — base, handler signature and subscribe are
    `esp_event`'s, and look-alike copies would only hide the loop an
    application already handles `WIFI_EVENT` on.
  * `espos_httpd.h` → `esp_http_server.h`: URI handlers are
    `esp_http_server` handlers (`httpd_req_t`, `httpd_uri_t`); a plain-C
    route/SSE shim is on the remediation list.

  `sdkconfig.h` is not an exception: it exists only to carry `CONFIG_` tokens
  and is reported together with them (below).
* **C only in the exported surface.** `#pragma once`, an `extern "C"` block,
  a file-level doc comment saying what the component is and on which task it
  calls back, the two SPDX lines. No classes, namespaces, templates,
  references, default arguments or `std::` types. `espos_n2k`, `espos_voice`
  and `espos_audio` are C++ interfaces by design (the script's `CPP_ONLY`):
  not part of the C ABI until they get C wrappers, and the check says so
  instead of failing on them.
* **Types.** Opaque handles for anything with a lifetime
  (`typedef struct espos_x espos_x_t;`, the struct defined in the `.c`).
  Fixed-width integers from `<stdint.h>` in structs and buffers, `size_t` for
  lengths, `bool`, `esp_err_t`; plain `int` only as a scalar return or count.
  Enums with explicit values and a `_MAX` last; the values are ABI — append,
  never renumber. No bitfields, anonymous unions, flexible array members or
  variadic functions: a generated binding cannot express the first three and
  cannot call the fourth.
* **Buffers** are pointer + length (`const uint8_t *buf, size_t len`);
  fixed-size character arrays are sized by a named `_MAX` macro of the
  header's own, never by a `CONFIG_` value.
* **Callbacks** are `void (*)(..., void *arg)` — the caller's context pointer
  handed back untouched (the existing headers put it last; keep that) — and
  the header states which task the callback runs on and what it may do
  there; [concepts.md](concepts.md) keeps the table. A new API prefers
  handing results to a queue over calling back from an internal task.
* **No `CONFIG_` in new public headers.** A header whose contents depend on
  `sdkconfig.h` means a different ABI per build. A limit a caller needs is a
  runtime query (`espos_x_max_y(void)`) or a fixed `_MAX` of the header's
  own; a comment may still name the knob. The existing uses are warnings the
  script counts down, not errors: `espos_wifi_sm.h` (`ESPOS_WIFI_MAX_NETWORKS`
  from `CONFIG_ESPOS_WIFI_MAX_NETWORKS`) and, in the C++-only components,
  `candump_tcp_server.h`, `twai_receiver.h` and `wyoming_satellite.h`
  (a default port or queue depth taken from Kconfig).
* **`ESPOS_ABI_VERSION`** (`espos.h`, which also carries the history of what
  changed at each value; `espos_abi_version()` returns the value the linked
  `espos_core` was built with) is bumped by any change to a public header that
  is not purely additive: a removal or rename; a changed signature; any change
  to a struct's members — an appended member changes `sizeof`, which a caller
  compiled against the old header has baked in; a changed enum or macro value;
  a changed callback contract; a new
  include exception. Additive, no bump: a new function, macro or header; an
  enum value appended before its `_MAX` when no public struct is sized by
  that `_MAX`. A bump records its reason in the history comment beside the
  constant in `espos.h` — that is where a reader holding an older binding
  looks — and the PR carries a `!` in its type so the release notes list it
  under Breaking changes. (A per-commit changelog line does not work: the
  notes list pull requests, not commits.)
* **The check.** `python3 tools/check_public_headers.py` — exit 1 on a
  non-allowlisted include or a missing guard, exit 0 with the `CONFIG_`
  warnings (`--strict` makes those fatal too, for the day the backlog is
  empty; `--quiet` drops the notes). CI runs it as the `headers` job.
