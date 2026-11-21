# GlassineOS/packages

Alpine package recipes of GlassineOS (aports layout, `APKBUILD` + `abuild`), and the
CI that builds, signs and publishes them to

    https://pkgs.glassine.org/alpine/v3.24/glassine

It is published for two architectures, each a directory below that URL:

- `aarch64`: every recipe whose `arch=` allows it (the Note Air and everything on it);
- `x86_64`: only the recipes listed in `arches/x86_64`, which are what the PC stages of
  GlassineOS/images (`glassine-common`, `device-uefi-pc`) install from here
  (`glassine-keys`, `glassine-deploy`, `glassine-reboot`). PCs take everything else,
  Mesa included, from Alpine.

Recipes for the Note Air's hardware (`forks/mesa`, `glassine-epdd`, `glassine-slot`,
`qrtr-diag-sink`, `device-onyx-noteair`, `linux-glassine-sdm660`,
`mesa-flatpak-glassine`) are `arch="aarch64"`. A recipe published for PCs is added to
`arches/x86_64`.

## Using the repository

GlassineOS images already have the repository line and the signing key, and the
`glassine-keys` package keeps both up to date. On another Alpine v3.24 system on aarch64
or x86_64,
put the public signing key, served under `keys/` on the same site
(`https://pkgs.glassine.org/keys/`, the same file as `keys/glassine-keys/*.rsa.pub` here),
into `/etc/apk/keys/` and add this line to `/etc/apk/repositories`:

    https://pkgs.glassine.org/alpine/v3.24/glassine

The line has no architecture: apk adds it (`/aarch64`, `/x86_64`) itself. The site's landing page,
written by `scripts/site.sh`, says the same.

## Layout

| Directory | Holds |
|---|---|
| `keys/` | `glassine-keys`: the repository's public signing key + the repository line |
| `device/` | kernels and device packages (`linux-glassine-sdm660`, …) |
| `glassine/` | our own software |
| `forks/` | patched upstream software, renamed and installed beside the original (`sway-glassine` installs `/usr/bin/sway-glassine`, Alpine's `sway` stays), or an Alpine recipe rebuilt with our patches under Alpine's names (`mesa`); also one package of our own, `mesa-flatpak-glassine`, which ships the Flatpak GL extension bundles (see Flatpak GL extension below) |
| `flatpak/` | Flatpak builds that devices get through a package here (see below) |
| `arches/` | `x86_64`: the recipes published for x86_64 (see above) |
| `scripts/` | build, pin, changed, docker-build, retain (which published versions stay) and site (landing page) scripts, see Building and CI |
| `.github/` | the workflows (`build`, `publish`, `checksum`, `flatpak-gl`, `safety`) and `dependabot.yml` |

A recipe directory has the name of its main package. Sources come from tags or pinned
commits of the GlassineOS repositories (checksum in the recipe); patches live in those
repositories, not here, apart from packaging fixes. The exception is a recipe that rebuilds
Alpine's own package with a few patches (`forks/mesa`): it keeps Alpine's recipe and source
tarball, and the patches sit next to it, as in aports.

## Branches

`main` builds for Alpine v3.24 (the release GlassineOS images use). The release lives
in one place per script (`BRANCH` in `scripts/build.sh`, `REPO_URL` in
`.github/workflows/publish.yml`, `_branch` and a new `pkgrel` in `keys/glassine-keys`,
the container in `scripts/docker-build.sh`). When we move to the next Alpine release,
the old one keeps its recipes on a `<release>-stable` branch, as in aports
(`3.22-stable` so far).

Only `main`'s release is published: Pages replaces the whole site on each deploy, so
an older release's repository stops being served at the first publish of the new one.
After a release change, run `publish` by hand with `all`, because the new repository
starts empty and a normal run builds only what changed. `glassine-keys` moves an
installed system to the new repository line on upgrade.

## Building

    scripts/docker-build.sh device/linux-glassine-sdm660

builds in a clean `alpine:3.24` container into `out/alpine/v3.24/glassine/<arch>/`,
signed with a throwaway key unless `ABUILD_PRIVKEY` holds the repository key. The
packages are of the host's architecture: aarch64 on an arm64 host, x86_64 on an x86-64
host. For aarch64 on another host use an arm64 machine (the kernel takes hours under
QEMU). Pin a source with `scripts/pin-source.sh URL [NAME]` (fails on HTTP
errors and broken archives; the `checksum` workflow runs it on a GitHub runner when
GitHub rate-limits your network). `scripts/changed.sh [--arch ARCH] BASE` lists what changed since `BASE`, in build
order (keys, glassine, forks, device); with `--arch`, only the recipes published for
`ARCH`, and a change to a recipe's `arch=` line alone builds nothing for an
architecture it already built for (so limiting a recipe to aarch64 doesn't rebuild
the published aarch64 packages). `scripts/test-changed.sh` checks it.
`scripts/unpublished.sh --arch ARCH INDEX` lists the recipes published for `ARCH` whose
current version (`pkgname` at `pkgver-rpkgrel`) the published `APKINDEX` lacks;
`scripts/test-unpublished.sh` checks it, and that every recipe's `pkgname`, `pkgver`
and `pkgrel` are plain values.

A recipe may depend on packages of other recipes here: the build resolves those from
the published repository when they aren't rebuilt in the same run (so on an
architecture without a published repository, build them together).

## CI

- `build`: pull requests and branches. Builds the changed packages with a throwaway
  key, per architecture on a native GitHub-hosted runner (arm64, x86-64), and keeps them
  as an artifact for 14 days. An architecture with nothing published yet builds all
  its recipes.
- `publish`: `main`. Per architecture, on a native runner: builds the changed
  packages (those the push changed, and every recipe whose version the live index
  lacks, so a push whose own run was replaced in the queue, failed or was cancelled is
  built by the next run; a version bump is what gets a change published for sure)
  signed with the repository key (a secret of the `release` environment, one
  approval per architecture job; both jobs wait at the same time), takes every other
  package from that architecture's live repository (its recipe must still be published
  for it), indexes and signs. An architecture with nothing to build carries its live
  repository forward; one with nothing published yet builds all its recipes. Then one
  job puts both architectures and a landing page that `scripts/site.sh` writes (key and
  repository line, see Using the repository) into one site and deploys it to GitHub
  Pages, which replaces the whole site each time. Earlier versions stay for a while,
  see Earlier versions.
  By hand (`workflow_dispatch`): a list of package directories, `all`, or `missing`
  (only the recipes whose version is not published); `select_only` prints the
  selection and builds nothing, beside a running publish.
  Needs the repository to be public (environments and Pages on private repositories
  need a paid plan).

## Earlier versions

GlassineOS/images installs the exact versions its locks name (`stages/*/lock` for
aarch64, `stages/*/lock.x86_64` for x86_64), and
only a lock refresh moves them, so a publish must not take a locked version away. The
repository therefore keeps earlier versions beside the newest in the same signed
`APKINDEX` (apk takes several versions of one name: `apk add name` installs the newest,
`apk add name=version` that one). `scripts/retain.sh` decides, at each publish, per
package name:

- the newest version stays (as before, only while its recipe exists);
- an earlier version stays for 30 days after a newer one replaced it;
- a version that a lock of GlassineOS/images `main` for that architecture names stays,
  whatever its age (the publish job reads the locks from a fresh clone; if the clone
  fails, only the 30 days hold, with a warning);
- if an architecture's packages together exceed its share of the 900 MiB cap (aarch64
  836 MiB, x86_64 64 MiB, `CAP` in `publish.yml`), its replaced versions that no lock
  names go, longest-replaced first; if the newest and the locked versions alone exceed
  it, the publish fails. The site job checks that the whole site stays under 900 MiB.

A version rebuilt under the same version string replaces the published file, as before.
`scripts/test-retain.sh` checks the rule on made-up indexes (the `build` workflow runs it).

Sizes, measured 2026-11-15: the site holds 49 packages, 97 MiB. The largest are
`mesa-flatpak-glassine` (47 MiB per version), `linux-glassine-sdm660` (22 MiB) and
`mesa` (12 MiB); everything else is under 6 MiB a recipe. In the 30 days before,
`glassine-shell` (3 MiB) had 19 versions, `mesa-flatpak-glassine` 3, `sway-glassine` and
`wvkbd-glassine` 7, `glassine-deploy` and `device-onyx-noteair` 13 (both tiny), and
`publish` succeeded up to 16 times a day. The x86_64 directory is about 0.1 MiB
(`glassine-keys`, `glassine-deploy` with its `-doc`, `glassine-reboot`; measured in a
local build 2026-11-16), so x86_64 takes almost nothing of the cap. At that rate the replaced versions add about
165 MiB (two `mesa-flatpak-glassine` and eighteen `glassine-shell` versions are most of
it), so the site settles near 260 MiB. GitHub Pages limits a site to 1 GB (hence the
cap) and has a soft bandwidth limit of 100 GB a month: a publish that downloaded the
whole site each time would use about 4 GB a day, so the publish job keeps the last
publish's packages in an Actions cache (`work/published`, with its `APKINDEX.tar.gz`)
and downloads only when that index is not the one Pages serves (a failed deploy, an
evicted cache). A Pages deployment times out after 10 minutes; the deploy step waits up
to 20 for the file sync, which grows with the site.

## Flatpak GL extension

Flatpak apps don't use the system Mesa: they take Mesa from a GL extension of their
runtime, Flathub's `org.freedesktop.Platform.GL.default` unless told otherwise. Ours is
`org.freedesktop.Platform.GL.glassine`, built by
`flatpak/org.freedesktop.Platform.GL.glassine/build.sh BRANCH` from the same Mesa
version and `glassine-*.patch` files as `forks/mesa`: freedreno, llvmpipe as the GL
fallback and lavapipe for Vulkan, with the libLLVM they need from the Sdk's LLVM
extension (`org.freedesktop.Sdk.Extension.llvm22`, the LLVM `forks/mesa` uses). A
session selects it with `FLATPAK_GL_DRIVERS=glassine:default`. It is built for each
runtime branch apps use (`25.08`, `26.08`), since an extension only applies to a
runtime with the same GL version. For those branches it replaces GL.default: the
`mesa-flatpak-glassine` package masks and removes GL.default's `BRANCH` and
`BRANCH-extra` refs, and an app on any other runtime branch still gets GL.default.

- Its `libgallium` is `libgallium-<version>-glassine.so`, so it never collides with
  GL.default's when both carry the same Mesa version.
- Its `[ExtensionOf]` priority is 10: Flatpak orders GL extensions by priority, then by
  name, never by `FLATPAK_GL_DRIVERS`, and the first one supplies the merged EGL vendor
  file, Vulkan ICD files and DRI drivers. The EGL vendor file and lavapipe's ICD file
  name our libraries by their full paths.
- `check.sh BRANCH` installs the bundle into an empty system installation, pins and
  masks it, runs `uninstall --unused` and `update`; then, with GL.default masked,
  installs the runtime and checks that GL.default stays out and that in a sandbox EGL
  gives a GLES2 context from our Mesa and Vulkan lists lavapipe, with our libgallium,
  libLLVM and libvulkan_lvp loaded (`probe.py`); then, with GL.default beside it, that
  the merged EGL vendor file, ICD file and DRI driver are ours and the same libraries
  load.

The `flatpak-gl` workflow builds and checks both branches on an arm64 runner for pull
requests and branches (artifact, 14 days). On `main` it creates the release
`gl-glassine-<Mesa version>-r<forks/mesa pkgrel>-<release>` with the bundles and their
commits, once per tag, after an approval in the `flatpak-release` environment (its own,
so the job that writes releases never sees the signing key; same reviewers, `main`
only, no secrets). A patch change in `forks/mesa` raises its `pkgrel` and so makes a
new release; a change to the extension's own build raises the number in
`flatpak/org.freedesktop.Platform.GL.glassine/release`. Devices don't download
it: a package here ships the bundles and installs them at boot, so it works offline.

## Signing key

One RSA key, `keys/glassine-keys/glassine-20261015.rsa.pub`. The private half exists
in two places: the `ABUILD_PRIVKEY` secret of the `release` environment and an offline
backup with the maintainer. Losing it means every device has to be told to trust a new
key by hand, so: rotate by adding the new public key to `glassine-keys` first, publish,
and switch the secret only after devices have updated.

## License

GPL-2.0-or-later (`LICENSE`) for the recipes and scripts. The packaged software keeps
its own license (`license=` in each recipe).
