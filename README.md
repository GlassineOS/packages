# GlassineOS/packages

Alpine package recipes of GlassineOS (aports layout, `APKBUILD` + `abuild`), and the
CI that builds, signs and publishes them to

    https://pkgs.glassine.org/alpine/v3.24/glassine

| Directory | Holds |
|---|---|
| `keys/` | `glassine-keys`: the repository's public signing key + the repository line |
| `device/` | kernels and device packages (`linux-glassine-sdm660`, …) |
| `glassine/` | our own software |
| `forks/` | patched upstream software, renamed and installed beside the original (`sway-glassine` installs `/usr/bin/sway-glassine`, Alpine's `sway` stays), or an Alpine recipe rebuilt with our patches under Alpine's names (`mesa`) |

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
arm64 host the packages are aarch64; elsewhere use an arm64 machine (the kernel takes
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
  deploys everything to GitHub Pages. Only the newest version of each package stays.
  Needs the repository to be public (environments and Pages on private repositories
  need a paid plan).

## Signing key

One RSA key, `keys/glassine-keys/glassine-20261015.rsa.pub`. The private half exists
in two places: the `ABUILD_PRIVKEY` secret of the `release` environment and an offline
backup with the maintainer. Losing it means every device has to be told to trust a new
key by hand, so: rotate by adding the new public key to `glassine-keys` first, publish,
and switch the secret only after devices have updated.

## License

GPL-2.0-or-later (`LICENSE`) for the recipes and scripts. The packaged software keeps
its own license (`license=` in each recipe).
