# CLAUDE.md — GlassineOS/packages

Alpine package recipes (see `README.md`). Every change: build the changed packages
locally (`scripts/docker-build.sh DIR…`) or in the `build` workflow before merging.

- Nothing vendor-owned in a recipe, a package or this repository: no firmware, blobs,
  dumps, private keys. Firmware packages extract from the device, they never ship it.
- Recipes follow Alpine aports conventions; the directory is named after the main
  package; sources are pinned (commit or tag + sha512sums).
- Never commit a private key. Only `keys/glassine-keys/*.rsa.pub` belongs here.
- Commits carry an `Assisted-by:` or `Co-Authored-By:` trailer. Never add
  `Signed-off-by`. Run `.glassine/org/scripts/safety-scan.sh --staged` before committing.
