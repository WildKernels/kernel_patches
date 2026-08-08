#!/bin/bash
# fix-susfs-compat.sh — Runtime SUSFS kernel compatibility fixes
# Called by build workflows after patches are applied.
# Fixes sublevel-dependent issues that can't be in static patches.
# Idempotent: safe to run multiple times on the same tree.
#
# Usage: fix-susfs-compat.sh <kernel_common_dir> <sublevel> <android_ver> <kernel_ver> <kernel_patches_dir>

KERNEL_DIR="$1"   # e.g., /path/to/common
SUBLEVEL="$2"     # e.g., 107, 209, 246
ANDROID_VER="$3"  # e.g., android12
KERNEL_VER="$4"   # e.g., 5.10
PATCHES_DIR="$5"  # reserved — not currently used

if [ -z "$KERNEL_DIR" ] || [ ! -d "$KERNEL_DIR" ]; then
    echo "fix-susfs-compat: ERROR: kernel dir '$KERNEL_DIR' not found" >&2
    exit 1
fi

echo "fix-susfs-compat: kernel=$KERNEL_DIR sublevel=$SUBLEVEL android=$ANDROID_VER kver=$KERNEL_VER"

# ---------------------------------------------------------------------------
# Fix 1: show_pad: label missing from fs/proc/task_mmu.c
# Needed when the 50_ patch adds a 'goto show_pad;' but the label itself
# isn't present in the source (android12-5.10 sublevels < 218).
# ---------------------------------------------------------------------------
TMU="$KERNEL_DIR/fs/proc/task_mmu.c"
if [ -f "$TMU" ]; then
    if grep -q 'goto show_pad;' "$TMU" && ! grep -q '^show_pad:' "$TMU"; then
        echo "fix-susfs-compat: injecting show_pad: label in task_mmu.c (sublevel $SUBLEVEL)"
        sed -i '/show_smap_vma_flags(m, vma);/,/return 0;/{/return 0;/i\show_pad:
        }' "$TMU"
    fi
    # Suppress -Wunused-label on show_pad regardless of goto presence
    if grep -q '^show_pad:' "$TMU" && ! grep -q 'show_pad:.*__attribute__' "$TMU"; then
        echo "fix-susfs-compat: marking show_pad: as maybe-unused in task_mmu.c"
        sed -i 's/^show_pad:$/show_pad: __attribute__((__unused__));/' "$TMU"
    fi
else
    echo "fix-susfs-compat: task_mmu.c not found — skipping show_pad fix"
fi

# ---------------------------------------------------------------------------
# Fix 2: fdinfo.c — inotify_mark_user_mask() fallback
# The helper was backported mid-stable (~5.10.68+). On older sublevels
# the function is absent, causing a link error. Replace the call with
# the direct field access it wraps.
# Guard: only act when the call-site is present but the definition is absent.
# ---------------------------------------------------------------------------
FDINFO="$KERNEL_DIR/fs/notify/fdinfo.c"
if [ -f "$FDINFO" ]; then
    if grep -q 'inotify_mark_user_mask(mark)' "$FDINFO"; then
        if ! grep -rq 'static.*inotify_mark_user_mask\|^u32 inotify_mark_user_mask' "$KERNEL_DIR/fs/notify/"; then
            echo "fix-susfs-compat: replacing inotify_mark_user_mask(mark) with mark->mask in fdinfo.c"
            sed -i 's/inotify_mark_user_mask(mark)/mark->mask/g' "$FDINFO"
        else
            echo "fix-susfs-compat: inotify_mark_user_mask defined — no fallback needed"
        fi
    fi
else
    echo "fix-susfs-compat: fdinfo.c not found — skipping inotify_mark_user_mask fix"
fi

# ---------------------------------------------------------------------------
# Fix 3: fdinfo.c — old-style 'u32 mask' declaration (sublevel ≤ 117)
# Early 5.10 sublevels declare 'u32 mask = mark->mask & IN_ALL_EVENTS;'
# before the SUSFS injected code, producing a C89 "declaration after
# statement" error. Delete the declaration and replace bare 'mask' refs.
# ---------------------------------------------------------------------------
if [ -f "$FDINFO" ]; then
    if grep -q 'u32 mask = mark->mask & IN_ALL_EVENTS;' "$FDINFO"; then
        echo "fix-susfs-compat: fixing u32 mask declaration in fdinfo.c (sublevel $SUBLEVEL)"
        # Remove the declaration
        sed -i '/u32 mask = mark->mask & IN_ALL_EVENTS;/d' "$FDINFO"
        # Single-line seq_printf variant: s_dev, mask)
        sed -i 's/s_dev, mask)/s_dev, mark->mask)/g' "$FDINFO"
        # Multi-line seq_printf variant: leading whitespace + mask, mark->ignored_mask
        sed -i 's/^\([[:space:]]*\)mask, mark->ignored_mask/\1mark->mask, mark->ignored_mask/' "$FDINFO"
    else
        echo "fix-susfs-compat: fdinfo.c u32 mask declaration not present — OK"
    fi
fi

# ---------------------------------------------------------------------------
# Fix 4: fdinfo.c — 'out_seq_printf:' label missing trailing semicolon
# C requires a statement after a label. The SUSFS patch may inject the
# label without a semicolon on older sublevels.
# ---------------------------------------------------------------------------
if [ -f "$FDINFO" ]; then
    # Match lines that have ONLY the label (no trailing semicolon)
    if grep -qE '^[[:space:]]*out_seq_printf:[[:space:]]*$' "$FDINFO"; then
        echo "fix-susfs-compat: adding semicolon after out_seq_printf: label in fdinfo.c"
        sed -i 's/^\([[:space:]]*\)out_seq_printf:[[:space:]]*$/\1out_seq_printf: ;/' "$FDINFO"
    else
        echo "fix-susfs-compat: out_seq_printf: label OK (already has semicolon or absent)"
    fi
fi

# ---------------------------------------------------------------------------
# Fix 5: susfs.c — i_uid_into_mnt / i_user_ns() fallback
# These helpers were backported mid-5.15 and are absent from 5.10 kernels
# that don't carry the backport. Fall back to direct i_uid field access.
# Guard: check for i_user_ns in include/linux/fs.h.
# ---------------------------------------------------------------------------
SUSFS_C="$KERNEL_DIR/fs/susfs.c"
if [ -f "$SUSFS_C" ]; then
    if ! grep -q 'i_user_ns' "$KERNEL_DIR/include/linux/fs.h" 2>/dev/null; then
        if grep -q 'i_uid_into_mnt' "$SUSFS_C"; then
            echo "fix-susfs-compat: replacing i_uid_into_mnt() calls in susfs.c (i_user_ns absent)"
            sed -i 's/i_uid_into_mnt(i_user_ns(&fi->inode), &fi->inode)\.val/fi->inode.i_uid.val/g' "$SUSFS_C"
            sed -i 's/i_uid_into_mnt(i_user_ns(inode), inode)\.val/inode->i_uid.val/g' "$SUSFS_C"
        fi
    else
        echo "fix-susfs-compat: i_user_ns present — i_uid_into_mnt fix not needed"
    fi
else
    echo "fix-susfs-compat: susfs.c not found — skipping i_uid_into_mnt fix"
fi

# ---------------------------------------------------------------------------
# Fix 6: setuid_hook.c — duplicate ksu_handle_setresuid on >= 6.8 kernels
# SukiSU builtin branch defines ksu_handle_setresuid in both the SUSFS
# #else block AND a separate 6.8+ MANUAL_HOOK block. When both
# CONFIG_KSU_SUSFS and CONFIG_KSU_MANUAL_HOOK are defined, both compile,
# causing a redefinition error. Remove the 6.8+ block.
# ---------------------------------------------------------------------------
SETUID="$KERNEL_DIR/drivers/kernelsu/setuid_hook.c"
if [ -f "$SETUID" ]; then
    DUPS=$(grep -c 'int ksu_handle_setresuid' "$SETUID")
    if [ "$DUPS" -gt 1 ]; then
        echo "fix-susfs-compat: removing duplicate ksu_handle_setresuid (6.8+ MANUAL_HOOK block)"
        python3 - "$SETUID" << 'PYEOF'
import sys
path = sys.argv[1]
with open(path) as f:
    lines = f.readlines()
start = next((i for i, l in enumerate(lines) if 'KERNEL_VERSION(6, 8, 0)' in l), None)
if start is not None:
    depth, end = 0, None
    for i in range(start, len(lines)):
        stripped = lines[i].strip()
        if stripped.startswith('#if'):
            depth += 1
        elif stripped.startswith('#endif'):
            depth -= 1
            if depth == 0:
                end = i
                break
    if end is not None:
        del lines[start:end+1]
        while lines and lines[-1].strip() == '':
            lines.pop()
        lines.append('\n')
        with open(path, 'w') as f:
            f.writelines(lines)
        print(f"  removed lines {start+1}-{end+1}")
PYEOF
    else
        echo "fix-susfs-compat: setuid_hook.c — no duplicate ksu_handle_setresuid"
    fi
else
    echo "fix-susfs-compat: setuid_hook.c not found — skipping"
fi

# ---------------------------------------------------------------------------
# Fix 7: task_mmu.c — pagemap_read vma scoping on older sublevels
# The 50_ patch places '#ifdef SUS_MAP / struct vm_area_struct *vma; /
# #endif' inside a nested block on older sublevels due to patch fuzz.
# Remove the misplaced 3-line block and insert the declaration at
# function scope after 'struct pagemapread pm;' (stable anchor present
# in all sublevels of pagemap_read).
# ---------------------------------------------------------------------------
if [ -f "$TMU" ]; then
    if grep -B1 'struct vm_area_struct \*vma;' "$TMU" | grep -q 'CONFIG_KSU_SUSFS_SUS_MAP'; then
        echo "fix-susfs-compat: relocating pagemap_read vma to function scope in task_mmu.c"
        python3 - "$TMU" << 'PYEOF'
import sys
path = sys.argv[1]
with open(path) as f:
    lines = f.readlines()

# Step 1: Remove the #ifdef-wrapped vma declaration (unique to pagemap_read)
removed = False
i = 0
while i < len(lines):
    if 'CONFIG_KSU_SUSFS_SUS_MAP' in lines[i] \
       and i+1 < len(lines) and 'struct vm_area_struct *vma;' in lines[i+1] \
       and i+2 < len(lines) and '#endif' in lines[i+2]:
        del lines[i:i+3]
        print(f"  removed #ifdef-wrapped vma block at line {i+1}")
        removed = True
        break
    i += 1

if not removed:
    print("  no #ifdef-wrapped vma found, skipping")
    sys.exit(0)

# Step 2: Insert vma decl after 'struct pagemapread pm;' in pagemap_read
inserted = False
for i, line in enumerate(lines):
    if 'struct pagemapread pm;' in line:
        lines.insert(i+1, '\tstruct vm_area_struct *vma;\n')
        print(f"  inserted vma decl after pagemapread pm (line {i+2})")
        inserted = True
        break

if not inserted:
    print("  ERROR: pagemapread pm anchor not found")
    sys.exit(1)

with open(path, 'w') as f:
    f.writelines(lines)
PYEOF
    fi
fi


echo "fix-susfs-compat: done"
exit 0
