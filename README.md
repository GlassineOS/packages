# GlassineOS/packages

Alpine package recipes of GlassineOS (aports layout, `APKBUILD` + `abuild`), and the
CI that builds, signs and publishes them to

    https://pkgs.glassine.org/alpine/v3.24/glassine

Only aarch64 is published: `REPO_URL` in `.github/workflows/publish.yml` is the
repository's `aarch64` directory, and there is no other architecture.

## Using the repository

GlassineOS images already have the repository line and the signing key, and the
`glassine-keys` package keeps both up to date. On another Alpine v3.24 system on aarch64,
put the public signing key, served under `keys/` on the same site
(`https://pkgs.glassine.org/keys/`, the same file as `keys/glassine-keys/*.rsa.pub` here),
into `/etc/apk/keys/` and add this line to `/etc/apk/repositories`:

    https://pkgs.glassine.org/alpine/v3.24/glassine

The line has no architecture: apk adds it (`/aarch64`) itself. The site's landing page,
written by `scripts/site.sh`, says the same.

## Layout

| Directory | Holds |
|---|---|
| `keys/` | `glassine-keys`: the repository's public signing key + the repository line |
| `device/` | kernels and device packages (`linux-glassine-sdm660`, …) |
| `glassine/` | our own software |
| `forks/` | patched upstream software, renamed and installed beside the original (`sway-glassine` installs `/usr/bin/sway-glassine`, Alpine's `sway` stays), or an Alpine recipe rebuilt with our patches under Alpine's names (`mesa`); also one package of our own, `mesa-flatpak-glassine`, which ships the Flatpak GL extension bundles (see Flatpak GL extension below) |
| `flatpak/` | Flatpak builds that devices get through a package here (see below) |
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
signed with a throwaway key unless `ABUILD_PRIVKEY` holds the repository key. On an
arm64 host the packages are aarch64, the only architecture published; elsewhere use an arm64 machine (the kernel takes
hours under QEMU). Pin a source with `scripts/pin-source.sh URL [NAME]` (fails on HTTP
errors and broken archives; the `checksum` workflow runs it on a GitHub runner when
GitHub rate-limits your network). `scripts/changed.sh BASE` lists what changed since `BASE`, in build
order (keys, glassine, forks, device).

A recipe may depend on packages of other recipes here: the build resolves those from
the published repository when they aren't rebuilt in the same run (so on an
architecture without a published repository, build them together).

## CI

- `build`: pull requests and branches. Builds the changed packages with a throwaway
  key on a GitHub-hosted arm64 runner and keeps them as an artifact for 14 days.
- `publish`: `main`. Builds the changed packages signed with the repository key (a
  secret of the `release` environment, one approval per run), takes every other
  package from the live repository (its recipe must still exist), indexes, signs and
  deploys everything to GitHub Pages, with a landing page that `scripts/site.sh` writes
  (key and repository line, see Using the repository). Earlier versions stay for a while,
  see Earlier versions.
  Needs the repository to be public (environments and Pages on private repositories
  need a paid plan).

## Earlier versions

GlassineOS/images installs the exact versions its locks (`stages/*/lock`) name, and
only a lock refresh moves them, so a publish must not take a locked version away. The
repository therefore keeps earlier versions beside the newest in the same signed
`APKINDEX` (apk takes several versions of one name: `apk add name` installs the newest,
`apk add name=version` that one). `scripts/retain.sh` decides, at each publish, per
package name:

- the newest version stays (as before, only while its recipe exists);
- an earlier version stays for 30 days after a newer one replaced it;
- a version that a lock of GlassineOS/images `main` names stays, whatever its age (the
  publish job reads the locks from a fresh clone; if the clone fails, only the 30 days
  hold, with a warning);
- if everything together exceeds 900 MiB, replaced versions that no lock names go,
  longest-replaced first; if the newest and the locked versions alone exceed it, the
  publish fails.

A version rebuilt under the same version string replaces the published file, as before.
`scripts/test-retain.sh` checks the rule on made-up indexes (the `build` workflow runs it).

Sizes, measured 2026-11-15: the site holds 49 packages, 97 MiB. The largest are
`mesa-flatpak-glassine` (47 MiB per version), `linux-glassine-sdm660` (22 MiB) and
`mesa` (12 MiB); everything else is under 6 MiB a recipe. In the 30 days before,
`glassine-shell` (3 MiB) had 19 versions, `mesa-flatpak-glassine` 3, `sway-glassine` and
`wvkbd-glassine` 7, `glassine-deploy` and `device-onyx-noteair` 13 (both tiny), and
`publish` succeeded up to 16 times a day. At that rate the replaced versions add about
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
