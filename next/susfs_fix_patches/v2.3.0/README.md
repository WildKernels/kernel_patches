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
setup.sh changes, userspace/gradle changes, Samsung KDP/RKP/DEFEX compat).

## Supported KernelSU-Next version ranges

Three patch eras exist because KernelSU-Next `dev` and the susfs4ksu
enable patch both keep moving:

| Directory | KSUVER range | Upstream base | Enable patch |
|---|---|---|---|
| `./` (this dir) | `<= 33333` | `dev` @ 2026-10-08 (`763a08d2` / `cffddfb1`, tiann/KernelSU#3871) | `10_enable_susfs_for_ksu.patch` stored here (susfs4ksu @ `687d2d18`, 2026-09-26) |
| `./high-33341/` | `33341 .. 33390` | `dev` @ 2026-10-15 (`89dc20d2`) | susfs4ksu archive @ `2ad2973` or newer (`kernel_patches/KernelSU/10_enable_susfs_for_ksu.patch`) |
| `./high/` | `>= 33397` | `dev` @ 2026-10-17 (`7b5fdb7a`, tiann/KernelSU#3911) | susfs4ksu archive @ `ca7093a1` or newer |

Unsupported transition windows:
- 33334–33340: selinux-hide time side-channel rewrite @33334, forced
  feature values @33337, native module_blacklist @33338 and
  shell_su_adb_only @33341.
- 33391–33396: module preinit refactor (tiann/KernelSU#3907, @33391)
  and follow-ups. Builds pinned into a window should be moved to one
  side or the other.

Notes:
- The stored `10_enable_susfs_for_ksu.patch` (old era) must be used
  instead of the archive's latest enable patch when KSUVER <= 33333,
  because the archive patch moved forward with the Oct 12 "Sync with the
  official KernelSU" commits. The `fs/` and `include/linux/` files from
  the archive are unchanged between the eras and stay compatible.
- The `high-33341/` and `high/` eras were regenerated following the
  "Regeneration" section below; their extras additionally drop the
  Samsung compat hunks that appeared in pershoot's rebased `dev-susfs`.

## Provenance (old era, `./`)

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

## Provenance (high era `high-33341/`, 33341..33390)

- Upstream base: `KernelSU-Next/KernelSU-Next` @ `89dc20d2`
  ("kernel: susfs (LKM): close O_PATH and dirent leak races"),
  i.e. `dev` as of 2026-10-15.
- SUSFS integration source: `pershoot/KernelSU-Next` branch
  `dev-susfs` rebased onto `89dc20d2`, pure-SUSFS commits:
  - `e46ac589` kernel: susfs (v2.3.0): Introduce SuSFS
  - `0b4ea672` kernel: susfs: Optimize path flagging, syscalls and Zygote detection
  - `541342fb` kernel: kbuild: Use strip for CONFIG conditional checks
  - `a864260b` kernel: susfs: Implement coexistence with LKM variant
  Dropped fork-local commits: throne_tracker offload, apk_sign
  signature-pair extension, setup.sh changes, userspace/gradle changes,
  Samsung KDP/RKP/DEFEX compat (`1535f484`, `57381c43`).
- Enable patch: susfs4ksu @ `2ad2973` ("KernelSU: Remove unused
  upstream functions in selinux_hide.c").

The high-era tree was verified by:
1. byte-comparing the fully patched tree against the target tree at
   `89dc20d2` (identical);
2. compiling the patched KernelSU-Next module (all SUSFS features
   enabled) against linux-5.15.180 with gcc — builds cleanly at both
   `89dc20d2` and `4cb8ee68` (KSUVER 33388);
3. patch-application tests across KSUVER 33341..33388.

## Provenance (high era `high/`, >= 33397)

- Upstream base: `KernelSU-Next/KernelSU-Next` @ `7b5fdb7a`
  ("Handle execveat selinux_setup exec on Android 17 QPR3",
  tiann/KernelSU#3911), i.e. `dev` as of 2026-10-17.
- SUSFS integration source: `pershoot/KernelSU-Next` branch
  `dev-susfs` rebased onto `7b5fdb7a`, pure-SUSFS commits:
  - `c61e123a` kernel: susfs (v2.3.0): Introduce SuSFS
  - `02cb0447` kernel: susfs: Optimize path flagging, syscalls and Zygote detection
  - `53173573` kernel: kbuild: Use strip for CONFIG conditional checks
  - `17922524` kernel: susfs: Implement coexistence with LKM variant
  Dropped fork-local commits: throne_tracker offload, apk_sign
  signature-pair extension, setup.sh changes, userspace/gradle changes,
  Samsung KDP/RKP/DEFEX compat (`a3b65405`, `f48ea311`, `931f97d6`).
  Samsung remnants that the rebase folded into the SUSFS commits were
  stripped by hand (app_profile, tp_marker, syscall_hook_manager,
  init, selinux_hide, Kbuild, compat/).
- Enable patch: susfs4ksu @ `ca7093a1` (identical to `2ad2973`).

Verified by:
1. byte-comparing the fully patched tree against the target tree at
   `7b5fdb7a` (identical);
2. compiling the patched KernelSU-Next module (all SUSFS features
   enabled) against linux-5.15.180 with gcc — builds cleanly
   (kernelsu.ko produced).

## Regeneration

When KernelSU-Next `dev` moves and the enable patch/fix patches start
rejecting, regenerate against the new upstream HEAD:

```sh
# partial = new upstream + enable patch (tolerating rejects)
# target  = new upstream + the pure-SUSFS commits above (cherry-pick,
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
