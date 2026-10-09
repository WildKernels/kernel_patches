# SUSFS v2.3.0 fix patches for KernelSU-Next

Fix patches that turn an upstream **KernelSU-Next** checkout into the
SUSFS v2.3.0 integrated state, following the same convention as the
older `v2.x` directories:

1. `10_enable_susfs_for_ksu.patch` from susfs4ksu is applied first
   (tolerating rejects).
2. `fix_<file>.patch` resolves every rejected hunk (one per `.rej` file).
3. `susfs_extras.patch` applies the remaining adaptations that the
   enable patch does not cover.

After the three steps the tree is byte-identical to pershoot's
`dev-susfs` integration minus its unrelated commits
(throne_tracker offload, apk_sign signature-pair extension,
setup.sh changes, userspace/gradle changes).

## Provenance

- Upstream base: `KernelSU-Next/KernelSU-Next` @ `763a08d2`
  ("fix(kernel): pkg_observer: defer track_throne to task_work",
  tiann/KernelSU#3871), i.e. `dev` as of 2026-10-08.
- SUSFS integration source: `pershoot/KernelSU-Next` branch
  `dev-susfs`, pure-SUSFS commits:
  - `04043f3a` kernel: susfs (v2.3.0): Introduce SuSFS
  - `a5b07635` kernel: susfs: Optimize path flagging, syscalls and Zygote detection
  - `88205bfe` kernel: kbuild: Use strip for CONFIG conditional checks
- Enable patch: susfs4ksu `gki-*` branches, `kernel_patches/KernelSU/10_enable_susfs_for_ksu.patch`
  (SUSFS_VERSION v2.3.0).

## Regeneration

When KernelSU-Next `dev` moves and the enable patch/fix patches start
rejecting, regenerate against the new upstream HEAD:

```sh
# partial = new upstream + enable patch (tolerating rejects)
# target  = new upstream + the three pure-SUSFS commits above (cherry-pick,
#           or rebase the dev-susfs integration and drop the unrelated commits)
for f in <each file that rejected>; do
  diff -u --label a/kernel/$f --label b/kernel/$f partial/kernel/$f target/kernel/$f > fix_$(basename $f).patch
done
diff -u ... (all other differing files) > susfs_extras.patch
```

Verify with:

```sh
patch -p1 --forward < 10_enable_susfs_for_ksu.patch || true
for file in $(find ./kernel -maxdepth 2 -name "*.rej" -exec basename {} .rej \;); do
  patch -p1 --forward < fix_${file}.patch
done
patch -p1 --forward < susfs_extras.patch
# result must be identical to the target tree
```
