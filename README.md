# GlassineOS/packages

Alpine package recipes of GlassineOS (aports layout, `APKBUILD` + `abuild`), and the
CI that builds, signs and publishes them to

    https://pkgs.glassine.org/alpine/v3.22/glassine

| Directory | Holds |
|---|---|
| `keys/` | `glassine-keys`: the repository's public signing key + the repository line |
| `device/` | kernels and device packages (`linux-glassine-sdm660`, …) |
| `glassine/` | our own software |
| `forks/` | patched upstream software, renamed (`sway-glassine` with `provides`/`replaces`) |

A recipe directory has the name of its main package. Sources come from tags or pinned
commits of the GlassineOS repositories (checksum in the recipe); patches live in those
repositories, not here, apart from packaging fixes.

## Branches

`main` builds for Alpine v3.22 (the release GlassineOS images use). The release lives
in one place per script (`BRANCH` in `scripts/build.sh`, `REPO_URL` in
`.github/workflows/publish.yml`, `_branch` in `keys/glassine-keys`). When we move to
the next Alpine release, the old one gets a `3.22-stable` branch, as in aports.

## Building

    scripts/docker-build.sh device/linux-glassine-sdm660

builds in a clean `alpine:3.22` container into `out/alpine/v3.22/glassine/<arch>/`,
signed with a throwaway key unless `ABUILD_PRIVKEY` holds the repository key. On an
arm64 host the packages are aarch64; elsewhere use an arm64 machine (the kernel takes
hours under QEMU). Pin a source with `scripts/pin-source.sh URL [NAME]` (fails on HTTP
errors and broken archives; the `checksum` workflow runs it on a GitHub runner when
GitHub rate-limits your network). `scripts/changed.sh BASE` lists what changed since `BASE`, in build
order (keys, device, glassine, forks).

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
