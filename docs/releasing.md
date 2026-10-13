# Releasing

espOS is consumed as a git submodule (`espos/` in a firmware project), so a
"release" is a tag other repositories can point at, and `version.txt` is what
the device reports.

## Cutting one

Nobody edits `CHANGELOG.md` or a version number by hand.
[release-please](https://github.com/googleapis/release-please)
(`.github/workflows/release-please.yml`) runs on every push to `main` that
carries a change users get, and keeps one pull request open,
**`chore: release <version>`**, which

* prepends the new `CHANGELOG.md` section: the release notes GitHub
  generates for the pull requests merged since the last release, one line
  each with its title, its author and a link to it. A label sorts each into
  a section (`.github/release.yml`), and
  `.github/workflows/label-by-title.yml` sets that label from the title's
  type whenever a pull request is opened or its title or description is
  edited: `feat` → Added, `fix` and `revert` → Fixed, `perf` and
  `refactor` → Changed, while `docs`, `build`, `ci`, `test` and `chore` stay
  out. A breaking change goes under Breaking changes whatever its type. A
  label set by hand holds until the next such edit. A pull request from a
  fork is not labelled (its token is read-only), so label it by hand;
* bumps `version.txt` and every espOS version in the component manifests
  (below);
* picks the version from the same titles, because pull requests are
  squash-merged: a `feat` raises the minor, a `fix` the patch, and a
  breaking change (`!` after the type, or a `BREAKING CHANGE:` line in the
  squashed body) the minor as well while espOS is below 1.0.

A push that carries only `docs`, `build`, `ci`, `test` or `chore` changes
does not run release-please at all. GitHub's notes are never empty, so it
would otherwise propose a release for each of them. Such a push does not
refresh an open release PR either; run the workflow by hand (**Actions →
release-please → Run workflow**) to do that, for instance after a change to
the release configuration. A manual run judges every commit since the last
release rather than one push's, so with only those changes unreleased it
does nothing either.

**Merging that pull request is the release.** The next run tags the merge
commit `v<version>`, creates the GitHub release with the new section as its
notes, and publishes the components to the registry. Until a maintainer
merges it, nothing is released. The release PR can be edited before merging
— lead the notes with anything that requires a consumer to change its own
code: the notes list a breaking change by its title, not by the
`BREAKING CHANGE:` line in its description.

There are no binaries to attach — espOS is source consumed as a submodule,
and the tag remains the deliverable; the release is a readable front page
for it.

The `[Unreleased]` block further down `CHANGELOG.md` is the hand-written
changelog from before the switch. Its entries belong to the next feature
release: when that release PR opens, fold the block into its section by hand.
Nothing generates it, and nothing removes it.

## What a device reports

`espos_project_prologue()` sets `PROJECT_VER` from `git describe --tags
--dirty --always`, falling back to `version.txt` when the checkout has no
tags (a tarball, or a release that has not been tagged yet). Firmwares built
on espOS get the same treatment for their own version, since they call the
same prologue. So:

| Build | `GET /api/v1/system/info` reports |
|---|---|
| the tagged commit | `0.7.0` |
| three commits later | `0.7.0-3-gabc1234` |
| with uncommitted changes | `0.7.0-3-gabc1234-dirty` |
| no tags at all | `0.7.0` (from version.txt) |

That distinction is the whole point of tagging. Without it every build
between two releases reports the same number, and "which firmware is on that
box" has no answer short of comparing binaries. The build warns when the
nearest tag and `version.txt` disagree.

## Consumers

A firmware project pins espOS by submodule commit. Bump it to a *tag*, and
say which one:

```sh
git -C espos fetch --tags
git -C espos checkout v0.7.0
git commit -am "chore: bump espos to v0.7.0"
```

The `fetch --tags` is not optional, and it is the step that gets skipped.
`git submodule update` fetches the pinned commit but **no new tags**, so a
submodule initialised before a release never learns that release exists. It
does not fail — it answers with the newest tag it happens to have, which is
worse. Measured on this repo's two consumers: both held `v0.6.0` and `v0.7.0`
only, so a pin whose real name is `v0.7.1-13-g814b72b` described itself as
`v0.7.0-66-g814b72b`, and drifted further with every release. CI is shallower
still: `actions/checkout` clones submodules at `--depth=1`, so `git describe`
there has no tags at all and yields a bare hash.

So do not read the version off `git describe` inside a submodule. The
prologue reports it from the submodule's tracked `version.txt` and uses
`describe` only to contradict that file — which is why a pin at a genuinely
un-bumped release is a build warning, while a merely un-fetched tag is a
`STATUS` line telling you to fetch.

The submodule still records a SHA — that is how submodules work — but the
commit message makes the release readable in `git log`. A bump commit that
says `bump espos to c6fd455` answers nothing without a second repository to
hand.

Consumers version themselves independently; espOS's version is not theirs.

### Choosing which espOS to build against

A contributor working across both repositories needs to move a firmware
between a release, `main`, and a local branch. That is the submodule itself —
there is no espOS setting for it, and there cannot be one: a project reaches
espOS through `include("espos/cmake/espos_project.cmake")`, a path *inside*
the submodule, so the submodule is already checked out before any espOS code
runs. Selecting it is a `git` operation by construction.

None of these touch a tracked file. `git status` shows `M espos` — a moved
submodule pointer, which is exactly what it is — and nothing is committed
until you mean to.

```sh
# the release the firmware ships against (the committed pin)
git submodule update --init espos

# a specific release
git -C espos fetch --tags && git -C espos checkout v0.8.0

# espOS main, to check a consumer still builds against unreleased work
git -C espos fetch origin main --tags && git -C espos checkout origin/main

# a local branch, mid-change across both repositories. The refspec is not
# optional: `fetch <path>` with no refspec fetches only HEAD into FETCH_HEAD,
# so there is no local `your-branch` to check out afterwards.
git -C espos fetch --tags /path/to/your/espOS your-branch:your-branch
git -C espos checkout your-branch
```

The build says which one it got, on every configure, so a checkout left on
`main` or on a branch cannot be mistaken for the pinned release:

```
espos: base 0.8.0 (pin v0.8.0)                            # a release
espos: base 0.8.0 (pin v0.8.0-3-g27ac6d6)                 # 3 commits past it
espos: base 0.8.0 (pin v0.8.0-3-g27ac6d6-dirty)           # ...with local edits
espos: base 0.8.0 from https://github.com/you/espOS.git (pin v0.8.0)  # a fork
```

The remote is named only when it is **not** the canonical repository, because
a tag says nothing about where it came from: `v0.8.0` in a fork and `v0.8.0`
upstream print identically while the code behind them can differ completely.
The expected URL is `repository:` in `espos_core/idf_component.yml`, so a fork
that legitimately becomes upstream carries its own answer.

When the work is done, the two repositories are separate pull requests: espOS
first, then a `chore: bump espos to vX.Y.Z` in the consumer once the espOS
side has merged and been tagged. Never merge a consumer PR whose behaviour
depends on unmerged espOS work — the pinned commit is what CI and every other
checkout actually build.

Installing espOS from the component registry instead of vendoring it is a
different shape again: there is no submodule and no prologue (`cmake/` ships
in no component archive), so the version range in `idf_component.yml` is the
selector and `dependencies.lock` records what it resolved to. See the README.

## Keeping third-party components current

Every third-party registry dependency in espOS is **exact-pinned** — the rule
is stated above `espressif/cjson` in each manifest — so a solve is
reproducible and nothing moves under a build. The cost is that nothing tells
you an update exists: espOS sat on `esp_hosted` 2.12.x for weeks while 3.x was
out, and the first anyone knew was a consumer build failing.

Dependabot cannot cover this. The ESP-IDF component manager is not one of its
ecosystems, so `.github/dependabot.yml` reaches `github-actions` and the UI's
npm tree and nothing else. `espos-p4-cockpit` answers it by re-resolving within
its declared ranges and diffing `dependencies.lock`; that approach cannot work
here, because exact pins leave nothing to re-resolve and the diff is empty
however far behind the pins are.

So `.github/workflows/dependency-drift.yml` asks the other question weekly —
*is there anything newer than what we pinned?* — by querying the registry per
dependency (`tools/espos_registry_updates.py`).

A pin it can bump on its own becomes a **pull request**, one per component:
`tools/espos_apply_bump.py` rewrites every manifest that pins the component and
`scripts/regen_lock.sh` regenerates `dependencies.lock`. It never pushes to
`main` — a component bump changes the binary that goes on a boat, so it goes
through a review and a build like any other change. One PR per component rather
than one for all of them, so a bump that turns out to be risky can be held while
the rest merge.

What it will **not** bump on its own, and files as an issue instead:

- a **range**, because bumping it means deciding to pin, which is policy;
- several **different exact pins** for one component, because which one is
  right is exactly the question;
- a manifest it could not **read**.

Run it by hand any time:

```sh
python3 tools/espos_registry_updates.py --root .
```

It prints one line per dependency:

| state | meaning |
| --- | --- |
| `current` | an exact pin, nothing newer published |
| `behind` | an exact pin with something newer published, or manifests that disagree about one component |
| `unpinned` | a range, so the solver takes the newest it permits whenever it next runs |
| `unparsed` | a manifest declares dependencies the tool could not read, so something is unwatched — fix the tool, not the manifest |
| `unknown` | the registry did not answer — never a claim that something moved |

Exit status is 1 when anything is `behind`, `unpinned` or `unparsed`, and 0
otherwise. Anything above 1 is the tool itself failing, which the workflow
turns into a failed job rather than an empty report.

### Reviewing a bump pull request

Read the component's changelog. A bump can change behaviour without breaking
the build, and two have:

- **`esp_hosted` 2.x → 3.x** renamed fourteen host-side Kconfig keys, and IDF
  drops an assignment to a symbol that no longer exists without a word — a
  green build with no WiFi.
- **`esp-sr` 2.5.3 → 2.5.5** moved 1.42 MB out of the `esp-dl` portion of the
  speech stack and dropped `esp-dsp` on esp32p4 by a new target rule, with no
  line of espOS changing.

Flash it before merging where it touches the radio, display or audio path.

### Regenerating the lock by hand

```sh
scripts/regen_lock.sh          # regenerate in place
scripts/regen_lock.sh --check  # regenerate and fail if it differed
```

Always on **esp32s3**, the target the committed lock records, and always from
the whole app: building a narrow project rewrites the lock to that project's
closure and silently drops `esp-sr`, `esp-dl`, `dl_fft`, `esp_new_jpeg`,
`esp_websocket_client`, `cjson`, `littlefs` and `mdns`. The script asserts both
rather than trusting them.

Nothing re-solves the lock on its own: the component manager rewrites only the
entries a *changed* manifest touches, so an entry nobody edits keeps its old
value indefinitely. A release is exactly that case — release-please rewrites
every `espos_*` manifest and re-solves nothing, so 0.15.0 shipped with all
fourteen lock entries still reading 0.14.0. The `release-lock` job in
`release-please.yml` regenerates the lock on the release branch, and CI's
`dependencies.lock matches the manifests` check fails any pull request, and
`main`, whose lock has fallen behind.

The job runs on every push to `main` while a release PR is open, not only when
release-please rewrites the release branch. A merge release-please ignores, such
as a `chore(deps)` bump, can still change `dependencies.lock` on `main`, and the
release branch's own lock commit then conflicts with it. 0.18.0 hit exactly that
after #213. So the job first merges `main` into the release branch, takes
`main`'s side of a `dependencies.lock` conflict and regenerates it, and fails on
any other conflict, which needs a person.

That job pushes with `DRIFT_PR_TOKEN` so the push starts CI on the release
PR. Without the secret it falls back to the workflow token, whose pushes start
no workflow: the job then warns, and closing and reopening the release PR runs
CI on the new head.

## Versioning

Semantic-ish, judged against what a *consumer firmware* sees:

* **patch** — fixes, docs, internal changes. A consumer bumps and rebuilds.
* **minor** — new components, new config keys, new API endpoints. Additive:
  a consumer bumps and rebuilds, and may then use the new thing.
* **major** — a consumer has to change its own code: a removed or renamed
  public function, a changed struct field, a config key that no longer
  exists, an `/api/v1` change.

espOS is pre-1.0, so minor is doing the work major will do later. Say plainly
in the release notes when a bump requires consumer changes — that is the
number people actually need.

## Registry publishing

Every `components/espos_*` directory is also a component on the [Espressif
Component Registry](https://components.espressif.com), under the
`signalk-espos` namespace: `signalk-espos/espos_config`, `signalk-espos/espos_sk`, and so on. A firmware
that does not want the submodule adds what it needs and the component
manager pulls the rest:

```sh
idf.py add-dependency "signalk-espos/espos_sk^0.7"
```

`espos_sk`'s manifest names `espos_config`, `espos_httpd`, `espos_wifi` and
`espos_health` as dependencies, so that one line installs the core. The
registry names each download `signalk-espos__<name>` in the build; a component's
own `REQUIRES espos_config` still resolves, because the component manager
maps a short name onto the namespaced component when only that one exists.

### One version for everything

Every manifest carries `version:` equal to `version.txt`, and every
dependency between espOS components is `^<that version>` with an
`override_path` to the sibling directory. The `override_path` is what an
in-tree build — and a firmware that vendors espOS as a submodule — uses: the
manager takes the checkout next to the manifest and never asks the registry
for an espOS component. The version range is what a registry consumer sees,
and lockstep versions keep it from ever mixing two espOS releases in one
firmware.

Lockstep is maintained by the release PR, not by hand. release-please bumps
`version.txt` itself, and bumps a manifest because two things hold:

* the manifest is listed under `extra-files` in `release-please-config.json`
  (type `generic`);
* each espOS version line in it — the component's own `version:` and every
  `signalk-espos/espos_*` dependency's `version:` — sits inside a marker
  block:

  ```yaml
  # x-release-please-start-version
  version: "0.18.1"
  # x-release-please-end
  ```

  The generic updater rewrites every version inside such a block, keeping the
  `^` of a range. Blocks wrap single lines on purpose: one around the
  dependencies would rewrite the `idf:` and `espressif/*` pins as well.

Either half missing fails quietly: pre-1.0, `^0.7.0` excludes `0.8.0`, so a
release that leaves one manifest or one range behind publishes components that
cannot be installed together. `scripts/check_release_markers.py` fails CI when
a manifest is not listed, a version line is not marked, or a block holds
anything but one version line — a new component copies an existing manifest's
markers and adds its path to the config.

The manifests are the registry's contract; a manifest that fails to pack
fails the release. CI runs `compote component pack` for every component on
each pull request, and the tag check that compares the tag to `version.txt`
covers the manifests as well.

### Publishing a release

`.github/workflows/publish.yml` is called by `release-please.yml` once
merging a release PR has created the `v*` tag. It is called rather than
triggered by the tag: a tag release-please creates with the workflow's own
token starts no other workflow. It re-checks that the tag, `version.txt` and
every manifest agree — a registry version is immutable, and that check is
worth repeating rather than assuming the release PR had the right numbers —
then uploads the components with `espressif/upload-components-ci-action`.

**No registry secret exists.** The action authenticates with GitHub OIDC: with
no `api_token` given it exchanges the job's `id-token` for a registry token,
which is why the job asks for `id-token: write`. What may use that token is
decided on the registry side, under the `signalk-espos` namespace's *trusted
uploaders*: the repository (`signalk-espOS/espOS`), the publishing workflow and
the branch. Because `publish.yml` is *called* by `release-please.yml`, and
GitHub names the calling workflow and the reusable one in different OIDC
claims, both files are registered as trusted uploaders.

The action is used rather than a `compote` loop of our own only because it is
the one path that speaks OIDC; it runs the same `compote component upload` per
component, one at a time, in the order it is given.

Upload order is computed, not written down: the registry resolves a
component's dependencies when it accepts the upload, so a component must not
arrive before the ones it names. `scripts/registry_order.py` topologically
sorts the manifests, and `--check` fails the release *before* anything is
uploaded, since half the components published is the one state that cannot be
rolled back. This paragraph used to carry a hand-written list of eleven
components while the tree had nineteen; every component added after it was
written was missing from it.

A registry version is immutable; a dry run validates without creating one, and
is the right rehearsal for a first publish or a manifest change. Run it from
the Actions tab (`publish` → *Run workflow*, `dry_run` on) or with
`gh workflow run publish.yml -f dry_run=true`. A dry run needs no credential at
all: the action skips authentication entirely for it. A published version that turns out wrong
is yanked with a message, never deleted, and fixed by the next patch release.

Each component also gets a `README.md` -- the registry shows it as the
component's page -- and the publish workflow copies the root `LICENSE` and
`NOTICE` into every component directory just before uploading, because each
archive is a redistribution and Apache-2.0 asks for both. They are copied on
the runner rather than committed 23 times over.

What the registry ships is the packed archive: the component directory
minus `build/`, `sdkconfig*`, `managed_components/`, `dependencies.lock`
and the manager's own defaults (`.git`, `__pycache__`, ...). Anything a
component needs at build time — `espos_config`'s generator under `tools/`,
`espos_httpd`'s `www/index.html`, the `config/*.json` descriptors — lives
inside the component directory for exactly this reason.

## Releasing a firmware

`.github/workflows/release-firmware.yml` is the whole release of a firmware
built on espOS: it builds every target and board, attaches the images to the
GitHub release, and mirrors them where a browser can read them, so the hosted
flasher and the Signal K plugin can install the release. With release-please
driving the version, a project's entire release workflow is this — and the
`@v…` pin below is the one kept current, because release-please bumps this
file. The same example in the workflow's own header deliberately says `@vX.Y.Z`
instead: nothing can rewrite a file under `.github/workflows/`, since
`workflows: write` exists only as a GitHub App permission and no `GITHUB_TOKEN`
has it, so a version there would freeze at whatever release first wrote it.

```yaml
on:
  push:
    branches: [main]

permissions:
  contents: write
  pull-requests: write

jobs:
  release-please:
    runs-on: ubuntu-latest
    outputs:
      release_created: ${{ steps.rp.outputs.release_created }}
      tag_name: ${{ steps.rp.outputs.tag_name }}
    steps:
      - uses: googleapis/release-please-action@v5
        id: rp
        with:
          release-type: simple   # keeps version.txt, which the device reports

  firmware:
    needs: release-please
    if: needs.release-please.outputs.release_created == 'true'
    permissions:
      contents: write
    uses: signalk-espOS/espOS/.github/workflows/release-firmware.yml@v0.18.1 # x-release-please-version
    with:
      name: my-firmware
      builds: '[{"target": "esp32c6"}, {"target": "esp32"}]'
      tag: ${{ needs.release-please.outputs.tag_name }}
    secrets:
      signing_key: ${{ secrets.SIGNING_KEY_PEM }}
      dockerhub_username: ${{ vars.DOCKERHUB_USERNAME }}   # optional
      dockerhub_token: ${{ secrets.DOCKERHUB_TOKEN }}      # optional
```

Pin a release tag, not `@main`: the workflow receives your signing key. The
key is passed by name because `secrets: inherit` does not reach a workflow in
another organisation.

The two Docker Hub values are optional. With both, the build pulls the ESP-IDF
image logged in rather than anonymously; anonymous pulls are rate limited per
runner address, and that allowance is shared with every other job on the
runner. Both are secrets of the called workflow so the username can come from
a variable or a secret, whichever the repository keeps it in. A failed login
falls back to the anonymous pull.

**What it produces.** Per build, `<name>-<board>-<tag>-merged.bin` for a cable
and `<name>-<board>-<tag>-ota.bin` for an update, where `<board>` is the
build's `board`, or its target when it names none. **Tags must be plain
`vX.Y.Z`**; a beta is such a tag with GitHub's *prerelease* flag set. A suffix
like `-rc1` is refused, because the registry reads it as part of the file
name.

**Board variants.** Two images for one chip, each with its own defaults file:

```yaml
      builds: >-
        [{"target": "esp32p4", "board": "7b", "sdkconfig_defaults": "sdkconfig.board.7b"},
         {"target": "esp32p4", "board": "x7", "sdkconfig_defaults": "sdkconfig.board.x7"}]
```

The files are applied after `sdkconfig.defaults` through `SDKCONFIG_DEFAULTS`,
which a project without the prologue honours.

**The mirror.** GitHub serves release downloads without the CORS header a web
page needs, so the images are also published to a `release-assets` branch of
the same repository, as `<tag>/<file>`, where raw.githubusercontent.com serves
them with it. The branch is one commit, rewritten on every publish, and keeps
exactly what the flasher can offer: the three newest stable releases and up to
two prereleases newer than them. Older directories are pruned; the releases
stay the archive. The push is leased, so it never overwrites a branch that
moved, and if anything after it fails the publish is rolled back, leased
against this run's own commit so a later release is never discarded
(`tools/espos_mirror_release_assets.sh`, `tools/espos_release_window.py`).

**Older releases** that predate the mirror are added without rebuilding:

```yaml
    with:
      name: my-firmware
      builds: '[{"target": "esp32c6"}]'
      backfill: v0.2.0 v0.3.0
```

**In the registry**, the entry declares the mirror and a pattern for these
names (`signalk-espOS/registry`, README):

```json
  "webAssetsBranch": "release-assets",
  "assets": {
    "ota":    "^my-firmware-[a-z0-9]+-v(?<version>[0-9]+\\.[0-9]+\\.[0-9]+)-ota\\.bin$",
    "merged": "^my-firmware-[a-z0-9]+-v(?<version>[0-9]+\\.[0-9]+\\.[0-9]+)-merged\\.bin$"
  }
```

A project with board variants captures the segment as `board` and gives each
board its `assetSegment` (`7b`, `x7` in the example above):

```json
    "ota":    "^my-firmware-(?<board>[a-z0-9]+)-v(?<version>[0-9]+\\.[0-9]+\\.[0-9]+)-ota\\.bin$",
    "merged": "^my-firmware-(?<board>[a-z0-9]+)-v(?<version>[0-9]+\\.[0-9]+\\.[0-9]+)-merged\\.bin$"
```

Unsigned releases, `--require` and the IDF pin work as in `build-firmware.yml`
below, which this workflow calls once per build.

## Building a consumer's firmware

`.github/workflows/build-firmware.yml` is reusable: a firmware built on espOS
calls it and keeps a workflow of about fifteen lines rather than two hundred.

```yaml
jobs:
  firmware:
    uses: signalk-espOS/espOS/.github/workflows/build-firmware.yml@main
    with:
      target: esp32p4
      name: p4-cockpit
      require: storage      # partitions the merged image must contain
    secrets:
      signing_key: ${{ secrets.SIGNING_KEY_PEM }}
```

### Several targets, and release-please

Two inputs exist for the case the first version of this workflow could not
serve, which is why both consumers kept their own copy of the release job
instead of calling it:

* **`tag`** — a release created by release-please uses the workflow token, and
  a release created that way does **not** fire the `release:` event. So
  `github.event.release.tag_name` is empty and the caller is the only thing
  that knows a release is being built. Without `tag` the workflow skipped
  restoring the signing key, staged nothing and uploaded nothing.
* **`artifact`** — two matrix legs uploading one artifact name is an error on
  `upload-artifact` v4+, so a per-target caller passes
  `firmware-${{ matrix.target }}`.

```yaml
jobs:
  firmware:
    strategy:
      matrix:
        target: [esp32c5, esp32c6, esp32c3, esp32s3, esp32]
    uses: signalk-espOS/espOS/.github/workflows/build-firmware.yml@main
    with:
      target: ${{ matrix.target }}
      name: ble-gateway-${{ matrix.target }}
      tag: ${{ inputs.tag }}
      artifact: firmware-${{ matrix.target }}
    secrets:
      signing_key: ${{ secrets.SIGNING_KEY_PEM }}
```

The caller downloads every artifact and makes **one** release. Five parallel
releases on one tag would race each other, and only an aggregate job can attach
every target's assets to the same tag.

> **Never write an expression inside a `description:` value.** GitHub evaluates
> `${}` interpolations there, so documenting `firmware-${...matrix.target...}`
> as an example made the whole workflow fail to parse **for every caller** —
> `matrix` does not exist in the called workflow's context. The error is
> reported against the *caller's* line number, not the description, which is
> why it took five attempts to find. A `#` comment is safe; a `description:` is
> not.

It reads the IDF pin from the consumer's own `.idf-version` (or the espOS
submodule's), builds, merges the flash images with
`tools/espos_merge_firmware.py`, and stages two assets: the merged image for
a cable, and the app image on its own for an OTA. They are not the same file
and picking the wrong one fails confusingly, which is why both are published.

**The signing key is the part that matters.** A device only accepts an OTA
signed with the key whose public half it was flashed with, so a release
signed by a per-run throwaway installs fine over USB and then rejects every
future update — on the boat, months later, with no way back except a cable.
The workflow therefore refuses to build a release without `signing_key`,
unless the repository variable `ESPOS_ALLOW_UNSIGNED_RELEASE` is `true`, in
which case it warns and sets its `unsigned` output so the release notes can
say so.

### `--require`, and why it is not a warning

`espos_merge_firmware.py` reads the offset map the build already produced
(`flasher_args.json`), so the offsets can never drift from what the partition
table needs. A partition the build did not produce is skipped — usually
harmless, because an absent `otadata` just means "boot the first slot".

It is not harmless for a data partition, and the trouble is that the image
still **boots**. An espOS firmware merged without its `storage` partition
comes up and serves a placeholder page that reads as a firmware bug; one
merged without a wake-word model comes up with a dead wake word that no OTA
can repair. `--require storage` turns that into a build failure:

```
error: --require storage was given, but 'storage.bin' was not produced by
this build. Merging without it would still yield a bootable image -- which
is exactly why this is an error rather than a warning.
```

## Starting a new firmware

There is no template repository to keep in step. A release is the starting
point either way:

```sh
# from the registry -- no clone, no submodule; the newest release
idf.py create-project-from-example "signalk-espos/espos_core:from_registry"

# or in the tree, if you are working on espOS itself
git clone https://github.com/signalk-espOS/espOS.git
cd espOS/components/espos_core/examples/minimal
```

Both are examples CI builds on every release, so neither can go stale the way
a generated repository does. `signalk-espOS/espos-template` was exactly that
generated repository and was deleted in favour of these: it sat five releases
behind, pinned to a commit rather than a tag, and would have failed to build
the moment it was synced, because the partition tables it named had moved
inside `espos_core`. Nothing referenced it but this page.
