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
| `./high-33341/` | `33341 .. 33390` | `dev` @ 2026-10-15 (`89dc20d2`) | pre-2026-10-19 susfs4ksu archive (`2ad2973` / `ca7093a1`) |
| `./high/` | `>= 33397` | `dev` @ 2026-10-19 (`d65c9db1`) | pre-2026-10-19 susfs4ksu archive (enable patch without the KernelSU-main preinit helpers) |
| `./high-kmain/` | `>= 33397` | `dev` @ 2026-10-19 (`d65c9db1`) | 2026-10-19 06:30 UTC KernelSU-main sync (`3d5c4dc1` / `291d5715` / `049e7b8a`) |
| `./high-kmain2/` | `>= 33397` | `dev` @ 2026-10-19 (`d65c9db1`) | 2026-10-19 10:33 UTC "Reuse upstream selinux_hide" sync (`c3e3d945` / `1ac091c4` / `f6c04677`) |

The `>= 33397` directory is selected by the **exact sha256 of the
enable patch**, using `fix-patch-manifest` in this directory.  Add a
new directory and manifest line for every new SUSFS enable-patch
revision; never overwrite an existing directory, because two SUSFS
revisions can be in use at the same KSUVER.

The KernelSU-main syncs are only supported for KSUVER >= 33397.  For
KSUVER <= 33390 the workflow rejects them with an explicit error
instead of applying mismatched fixes; pin SUSFS to a pre-2026-10-19
ref for those KernelSU-Next versions.

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

## Provenance (high era `high/`, >= 33397, pre-2026-10-19 SUSFS)

- Upstream base: `KernelSU-Next/KernelSU-Next` @ `7b5fdb7a`, and the
  set is also patch-compatible with `dev` through `d65c9db1`
  (KSUVER 33397..33403).
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
- Enable patch: susfs4ksu `gki-*` branches before the 2026-10-19
  KernelSU-main sync (`2ad2973` / `ca7093a1`; all pre-sync branches
  carry the same enable patch).
- `fix_sucompat.c.patch` keeps its local `get_user_arg_ptr()` because
  this enable patch does not add the helper to `runtime/ksud.h`.

Verified by:
1. byte-comparing the fully patched tree against the target tree at
   `7b5fdb7a` (identical);
2. compiling the patched KernelSU-Next module against linux-5.15.180
   with gcc — builds cleanly;
3. the 2026-10-19 04:03 UTC CI run (159/159 jobs) and local
   patch-application tests at KSUVER 33397..33403.

## Provenance (high era `high-kmain/`, >= 33397, KernelSU-main sync)

- Upstream base: `KernelSU-Next/KernelSU-Next` @ `d65c9db1`
  ("kernel: susfs (LKM): declare susfs_btf_member_offset in the
  header too"), i.e. `dev` as of 2026-10-19.
- SUSFS integration source: susfs4ksu `gki-*` branches after the
  2026-10-19 `"kernel & KernelSU: Sync with the official KernelSU
  main repo"` commits (`3d5c4dc1` for android15-6.6, `291d5715` for
  android16-6.12, `049e7b8a` for android14-6.1).  The enable patch is
  now generated against KernelSU main rather than KernelSU-Next, so
  `high-kmain/` adapts its hunks to the KernelSU-Next tree.
- Compared with `high/`, this set:
  - drops the obsolete `sh_user_path()` hunk from `fix_sucompat.c`;
  - uses the `get_user_arg_ptr()` helper that the synced enable patch
    adds to `kernel/runtime/ksud.h` instead of defining a duplicate;
  - keeps the sync's fd-based `ksu_setup_preinit_file()` /
    `ksu_preinit_handle_post_execveat()` functions, because the
    synced `50_add_susfs_in_*.patch` calls them from `fs/exec.c`;
  - keeps `close_fd()` on 5.15+ and falls back to
    `ksu_close_fd()` on < 5.15, where the vendor trees do not provide
    `close_fd()`;
  - no longer needs `fix_ksud_integration.c.patch`.

Verified by:
1. applying the full workflow sequence (enable patch → per-file fixes
   → `susfs_extras.patch`) at KSUVER 33397, 33400, 33401, 33402 and
   33403; every fix applies without rejects and `kernel/Kconfig`
   contains `config KSU_SUSFS`;
2. building the KSU tree in the CI matrix (the first CI run using this
   set exposed and fixed the `get_user_arg_ptr` redefinition).

## Provenance (high era `high-kmain2/`, >= 33397, selinux_hide reuse)

- SUSFS integration source: the 2026-10-19 10:33 UTC commits
  `"kernel & KernelSU: Reuse upstream selinux_hide existing code"`
  (`c3e3d945` for android15-6.6, `1ac091c4` for android16-6.12,
  `f6c04677` for android14-6.1).
- This revision replaces the large SUSFS selinux_hide implementation
  with the upstream selinux_hide code plus a `my_setprocattr()` hook.
  Consequently the old `kernel/feature/selinux_hide.c` hunks from
  `high-kmain/` are obsolete and removed from this set.  The enable
  patch itself now provides the implementation that the synced
  `50_add_susfs_in_*.patch` expects.
- The KSU-side fixes are otherwise the same as `high-kmain/`
  (`get_user_arg_ptr` from `ksud.h`, fd-based preinit, version-gated
  `ksu_close_fd()`).
- The follow-up 2026-10-19 16:00 UTC `"KernelSU: Sync with the official
  KernelSU main repo"` revision (`f43926d`, enable-patch sha256
  `056d684c...`) keeps the same upstream-selinux_hide implementation;
  its enable-patch changes are limited to Kconfig defaults and
  `ksud_integration.c` offsets.  The same manifest entry and extras set
  therefore apply.

Verified by applying the full enable → fixes → extras sequence at
KSUVER 33397, 33403 and 33405 (KernelSU-Next `b427a3f9`); no rejects
remain and `config KSU_SUSFS` is present in `kernel/Kconfig`.

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
