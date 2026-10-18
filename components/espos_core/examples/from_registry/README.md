# from_registry — **Essential**

The same application as [`minimal`](../minimal/), built the way a project
outside this repository builds: **espOS installed from the component registry**,
nothing on disk but this directory. The C is deliberately identical — what the
example demonstrates is the files around it.

Use this one as the starting point for your own firmware. Use `minimal` when you
are working inside an espOS checkout, where the prologue does all of this for
you.

## Why it is not like the other examples

Every other example starts with

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/espos_project.cmake")
espos_project_prologue(NAME "minimal")
```

The prologue sets the sdkconfig defaults, the partition table, the IDF version
policy, the component search path and the signing key in one call. That is the
right thing in-tree — and it cannot work anywhere else, because
`cmake/espos_project.cmake` is a path *inside a checkout*. It is also not in the
published component archives, and cannot be: the prologue has to run **before**
`project()`, while a component's `project_include.cmake` runs after.

So this example is plain IDF plus a dependency, and everything the prologue
would have set is written out where you can read it.

## Building it

```sh
. $IDF_PATH/export.sh                 # ESP-IDF 6.0.x
espsecure generate-signing-key --version 2 --scheme rsa3072 secure_boot_signing_key.pem
idf.py set-target esp32c6             # or esp32; CI builds both
idf.py build flash monitor
```

A target can need settings of its own, and the original ESP32 is the example:
RSA app signing is the Secure Boot **V2** scheme, which on that chip exists only
for revision >= 3.0. Without `CONFIG_ESP32_REV_MIN_3=y` Kconfig cannot offer it
and silently resolves `CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y` down to the V1
ECDSA scheme -- so the setting reads as present in `sdkconfig.defaults` and
absent in the generated `sdkconfig`, and espos_core's lint rejects the build
naming a line that is already there. That one line lives in
`sdkconfig.defaults.esp32`, which IDF appends automatically for that target and
ignores for every other; espOS's own `sdkconfig.d/espos.defaults.esp32` carries
it for the same reason.

CI builds this example for **esp32c6 and esp32**. The other three targets the
prologue examples support are untested here -- adding one means finding what
that chip's signing scheme needs and checking the lint passes, not assuming it.

The key has to exist **before** the first configure, and it has to be made with
`espsecure` rather than `idf.py secure-generate-signing-key`. Both of those are
the same trap from two sides, and both were found by running the commands:

* `idf.py` *configures the project* in order to run a subcommand. Before a
  target is set that configure picks the default one, so generating the key
  first leaves `CONFIG_IDF_TARGET="esp32"` behind and the `set-target` after it
  has no effect — an esp32 build when you asked for esp32c6. (Here the configure
  fails outright on the sdkconfig lint, so no key is produced either.)
* Generating it *after* `set-target` is too late: IDF writes a "key is missing"
  rule into the ninja graph while creating the build directory, and a key that
  appears afterwards does not clear it — `idf.py build` still stops on it.

`espsecure` touches no project state, so it sidesteps both. It is what
`scripts/build_example.sh` uses for the same reason.

The signing key is **not** generated for you, unlike in the prologue examples.
espOS builds with `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, so a device
accepts an OTA only when it is signed with the key whose public half it was
flashed with — and a key invented by a build step is a key nobody kept. Generate
it once, keep it safe, and read [ota.md](../../../../docs/ota.md) before a
device leaves the bench: lose it and no device flashed with it can ever be
updated over the air again.

## Starting your own project from this

`idf.py create-project-from-example "signalk-espos/espos_core:from_registry"`
gives you all of this. What each part is for, in case you start elsewhere:

1. **`main/idf_component.yml`** names espOS. `espos_sk` pulls the rest of the
   core in as its own dependencies; `espos_ota` has to be named, because nothing
   else pulls it in and a firmware without it can never be updated over the air:

   ```yaml
   dependencies:
     idf: ">=6.0.0,<6.1.0"
     signalk-espos/espos_core: "^0.23.0"  # x-release-please-version
     signalk-espos/espos_sk: "^0.23.0"  # x-release-please-version
     signalk-espos/espos_ota: "^0.23.0"  # x-release-please-version
   ```

   Keep the `^`: espOS is pre-1.0, where a minor bump does the work a major
   will do later. The components release in lockstep — one version of any works
   with the same version of every other. (The file in this directory also
   carries `override_path`, so espOS's own CI builds it against the tree it
   ships in. The registry removes those lines from the copy you download.)

2. **A root `CMakeLists.txt`** — plain IDF, plus the UI partition:

   ```cmake
   cmake_minimum_required(VERSION 3.22)
   include($ENV{IDF_PATH}/tools/cmake/project.cmake)
   project(my_firmware)
   espos_project_ui_partition()          # after project(), always
   ```

   `espos_project_ui_partition()` comes from `espos_httpd`, which ships the web
   UI bundle inside the component, so it needs no prologue. Without it the
   device serves a placeholder page instead of the config UI — which reads as a
   firmware bug rather than a missing build step.

3. **A partition table.** IDF's default has one app slot and no `storage`;
   espOS needs two OTA slots and the UI partition. Copy one of the bundled
   tables rather than pointing at the copy inside `managed_components/` — that
   directory is a build artefact, absent from a fresh checkout:

   ```sh
   cp managed_components/signalk-espos__espos_core/partitions/4mb.csv partitions.csv
   ```

4. **`sdkconfig.defaults`** — see the file here; every line has the reason
   above it. The one that is easy to miss is `CONFIG_ESPTOOLPY_FLASHSIZE_*`,
   which must match the table you picked: a 4 MB table on the 2 MB default runs
   past the end of the chip and the flash fails partway through writing it.
   espos_core's lint sums the table and says so before you get there — for 2–4
   it names each missing piece with the consequence, so delete one and the build
   tells you what to put back.

5. **`version.txt`** — your firmware's version, which the device reports and an
   update is compared against. Without it IDF falls back to `git describe`, and
   to `1` outside a repository. release-please's `simple` release type keeps it
   current if you let it.

6. **`.idf-version`** — the ESP-IDF version you build with (`v6.0.3`), one line.
   espOS's reusable firmware workflow reads it to pick the build image.

7. **`.gitignore`** — keeps the build output, `managed_components/` and, above
   all, `secure_boot_signing_key.pem` out of git. Commit `dependencies.lock`: it
   is what makes the next build use the same components.

## Releasing it

espOS's reusable `release-firmware.yml` builds every target and board, attaches
the images to your GitHub release and publishes the browser-readable copies the
hosted flasher needs, from a workflow of about twenty lines. See [Releasing a
firmware](../../../../docs/releasing.md#releasing-a-firmware).

## What the monitor shows

Same as `minimal`: the portal SSID if there is no stored network, then the IP
and `.local` name, the Signal K server it found, `approve in Security → Access
Requests`, and `approved, streaming`. See [minimal's
README](../minimal/README.md#what-the-monitor-shows) and
[getting-started.md](../../../../docs/getting-started.md).
