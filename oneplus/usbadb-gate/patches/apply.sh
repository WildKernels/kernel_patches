#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Drop usbadb_gate into an existing kernel tree. Idempotent: safe to re-run
# after updating the sources.
#
#   ./patches/apply.sh /path/to/kernel [/path/to/KernelSU-Next]
#
# Applies both patches:
#   0001  usb gadget state notifier          -> into the kernel tree
#   0002  the reader trigger in KERNEL_SU_RC -> into the KernelSU-Next tree
#
# 0002 is the half that acts, so it matters. If you use KernelSU-Next, pass it
# as the second argument: the repo root, or straight at
# kernel/runtime/ksud_integration.c. Omit it and apply.sh looks in the usual
# places. Found -> patched; not found -> the driver still builds and publishes,
# but nothing acts on the verdict until you add the block by hand.

set -eu

KDIR=${1:-}
[ -n "$KDIR" ] || { echo "usage: $0 /path/to/kernel [/path/to/KernelSU-Next]" >&2; exit 2; }

KSU_ARG=${2:-}
[ "$#" -le 2 ] || { echo "usage: $0 /path/to/kernel [/path/to/KernelSU-Next]" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SRC="$ROOT/drivers/misc/usbadb_gate"
DEST="$KDIR/drivers/misc/usbadb_gate"

[ -d "$KDIR/drivers/misc" ] || { echo "! $KDIR does not look like a kernel tree" >&2; exit 1; }

# Append a line without ever gluing it onto the previous one.
#
# Some vendor drivers/misc/Makefile files end WITHOUT a trailing newline (5.10
# OnePlus trees do). A plain >> then produces
#     obj-y   += tri_state_key/obj-$(CONFIG_USBADB_GATE) += usbadb_gate/
# which make parses as obj-y="tri_state_key/obj-" and fails with
#     No rule to make target 'drivers/misc/tri_state_key/obj-y'
# A line the user asks to add must never corrupt an unrelated one.
append_line() { # append_line <file> <line>
    f=$1; line=$2
    if [ -s "$f" ] && [ -n "$(tail -c 1 "$f" 2>/dev/null)" ]; then
        printf '\n' >> "$f"
    fi
    printf '%s\n' "$line" >> "$f"
}

echo "== copying sources"
mkdir -p "$DEST"
cp -a "$SRC"/. "$DEST"/
echo "   -> $DEST"

# The one test that models the kernel rather than a FIFO: it compiles the driver
# as copied and drives read() through ksys_read(). Copied in next to the driver
# so the check travels with the tree it is checking; `make` ignores it.
if [ -f "$ROOT/tests/read_pos_check.py" ] && [ -d "$ROOT/tests/kstub" ]; then
    cp -a "$ROOT/tests/kstub" "$DEST/kstub"
    cp -a "$ROOT/tests/read_pos_check.py" "$DEST/read_pos_check.py"
fi

echo "== wiring up Kconfig"
if grep -q "usbadb_gate/Kconfig" "$KDIR/drivers/misc/Kconfig"; then
    echo "   already present"
else
    append_line "$KDIR/drivers/misc/Kconfig" ""
    append_line "$KDIR/drivers/misc/Kconfig" 'source "drivers/misc/usbadb_gate/Kconfig"'
    echo "   added source line"
fi

echo "== applying the gadget state notifier patch"
GADGET_CORE="$KDIR/drivers/usb/gadget/udc/core.c"
GADGET_HDR="$KDIR/include/linux/usb/gadget.h"
NOTIFIER_PATCH="$HERE/0001-usb-gadget-state-notifier.patch"
NOTIFIER_FALLBACK="$HERE/apply_notifier_fallback.sh"

[ -f "$GADGET_CORE" ] || { echo "! $GADGET_CORE missing - USB gadget support needed" >&2; exit 1; }

# Both files, not either. `grep -q pat f1 f2` succeeds if one of them matches,
# which would accept a half-patched tree - the header done and core.c not.
if grep -q "usb_gadget_register_state_notifier" "$GADGET_CORE" 2>/dev/null &&
   grep -q "usb_gadget_register_state_notifier" "$GADGET_HDR" 2>/dev/null; then
    echo "   already applied"
else
    # -p1 from the kernel root, default fuzz. Fuzz is patch's safety net for a
    # tree that has drifted a few lines from what the diff expects (vendor
    # kernels usually have).
    if (cd "$KDIR" && patch -p1 --dry-run -s < "$NOTIFIER_PATCH") >/dev/null 2>&1 &&
       (cd "$KDIR" && patch -p1 --no-backup-if-mismatch -s < "$NOTIFIER_PATCH"); then
        echo "   patched udc core + gadget.h"
    else
        # Vendors insert their own code inside usb_gadget_set_state() and around
        # its prototype, which moves the context lines the diff pins. The
        # function is still there, so find it by name instead of by offset.
        echo "   the diff does not match this tree, placing the same three edits by name"
        if sh "$NOTIFIER_FALLBACK" "$KDIR"; then
            echo "   patched by anchor, not by context"
        elif grep -q "usb_gadget_set_state" "$GADGET_CORE" 2>/dev/null; then
            # Last resort, and it is a real one: usb_gadget_set_state() is
            # reachable from the driver. Speak to it from udc/core.c's ioctl-side
            # no - from the gadget's own state work instead, which is a notifier
            # the kernel already provides: the workqueue item that core.c queues
            # on every state change calls sysfs_notify(). A kobject uevent is not
            # equivalent, so instead require the caller to place three lines, and
            # say exactly where.
            echo "   the function is there but not in a shape this can edit" >&2
            echo "   Three edits by hand, and nothing else needs touching:" >&2
            echo "     1. $GADGET_CORE: put the chain + register/unregister" >&2
            echo "        immediately above usb_gadget_set_state()" >&2
            echo "     2. $GADGET_CORE: add inside it, as the last statement:" >&2
            echo "          atomic_notifier_call_chain(&usb_gadget_state_notifier, state, gadget);" >&2
            echo "     3. $GADGET_HDR: add after the usb_gadget_set_state() prototype:" >&2
            echo "          extern int usb_gadget_register_state_notifier(struct notifier_block *nb);" >&2
            echo "          extern void usb_gadget_unregister_state_notifier(struct notifier_block *nb);" >&2
            echo "   The wording is in $NOTIFIER_PATCH, which shows the exact block." >&2
            exit 1
        else
            echo "! could not add the notifier to this tree. Nothing was changed." >&2
            echo "  What it needs, by hand, in $GADGET_CORE:" >&2
            echo "    - the notifier chain + register/unregister, just before" >&2
            echo "      usb_gadget_set_state()" >&2
            echo "    - atomic_notifier_call_chain() inside it, after schedule_work()" >&2
            echo "    - #include <linux/notifier.h>" >&2
            echo "  and in $GADGET_HDR, the two externs after the" >&2
            echo "  usb_gadget_set_state() prototype." >&2
            echo "  The patch itself, for reference: $NOTIFIER_PATCH" >&2
            exit 1
        fi
    fi
fi

echo "== applying the KernelSU-Next rc patch (this is the half that acts)"
KSU_PATCH="$HERE/0002-kernelsu-usbadb-gate-rc.patch"
RULES_C=

# An explicit path is a promise: if it is wrong, say so instead of guessing.
if [ -n "$KSU_ARG" ]; then
    case $KSU_ARG in
        */ksud_integration.c) RULES_C=$KSU_ARG ;;
        *)             for c in "$KSU_ARG/kernel/runtime/ksud_integration.c" \
                               "$KSU_ARG/runtime/ksud_integration.c"; do
                           [ -f "$c" ] && { RULES_C=$c; break; }
                       done ;;
    esac
    [ -n "$RULES_C" ] || {
        echo "! no kernel/runtime/ksud_integration.c under $KSU_ARG" >&2
        echo "  Point it at the KernelSU-Next root, or at ksud_integration.c." >&2
        exit 1
    }
else
    # Auto-detect. KernelSU-Next's own dev branch puts it at kernel/KernelSU,
    # but roms and downstream copies use drivers/kernelsu (OnePlus/OPLUS trees
    # and the "source/drivers/kernelsu" layout), drivers/KernelSU, or a plain
    # checkout beside the kernel. Try those, then search for the file by name.
    for d in "$KDIR/kernel/KernelSU" "$KDIR/drivers/kernelsu" \
             "$KDIR/drivers/KernelSU" "$KDIR/KernelSU" "$KDIR/KernelSU-Next" \
             "$KDIR/drivers/kernelsu/KernelSU-Next" \
             "$(dirname "$KDIR")/KernelSU-Next"; do
        if [ -f "$d/kernel/runtime/ksud_integration.c" ]; then
            RULES_C=$d/kernel/runtime/ksud_integration.c
            break
        fi
    done
    # Some forks drop the leading kernel/.
    if [ -z "$RULES_C" ]; then
        for d in "$KDIR/kernel/KernelSU" "$KDIR/drivers/kernelsu" \
                 "$KDIR/drivers/KernelSU"; do
            if [ -f "$d/runtime/ksud_integration.c" ]; then
                RULES_C=$d/runtime/ksud_integration.c
                break
            fi
        done
    fi
    # Last resort: any ksud_integration.c that defines KERNEL_SU_RC.
    if [ -z "$RULES_C" ]; then
        for f in $(find "$KDIR" -maxdepth 6 -name ksud_integration.c 2>/dev/null); do
            if grep -q 'KERNEL_SU_RC' "$f" 2>/dev/null; then
                RULES_C=$f
                break
            fi
        done
    fi
fi

if [ -z "$RULES_C" ]; then
    echo "   skipped: no KernelSU-Next tree found"
    echo "   The driver will detect and publish, but NOTHING will act on it."
    echo "   If you use KernelSU-Next, re-run as: $0 $KDIR /path/to/KernelSU-Next"
    echo "   Otherwise add the block in $HERE/usbadb_gate.rc to an .rc init parses."
elif grep -q 'usbadb_gate' "$RULES_C"; then
    echo "   already applied"
else
    # The patch header says a/kernel/runtime/ksud_integration.c. Upstream keeps
    # that path (root + -p1); trees that put it at drivers/kernelsu/runtime/
    # have no kernel/ directory, so the same patch needs -p2. Pick the strip
    # level by looking at what is actually on disk instead of assuming.
    KSU_ROOT=$(cd "$(dirname "$(dirname "$RULES_C")")" && pwd)
    STRIP=
    [ -f "$KSU_ROOT/kernel/runtime/ksud_integration.c" ] && STRIP=p1
    [ -z "$STRIP" ] && [ -f "$KSU_ROOT/runtime/ksud_integration.c" ] && STRIP=p2
    if [ -z "$STRIP" ]; then
        echo "! do not know how to address $RULES_C with this patch" >&2
        echo "  Apply by hand, from $(dirname "$(dirname "$RULES_C")"):" >&2
        echo "    patch -p1 < $KSU_PATCH   ( or -p2 if that complains )" >&2
        exit 1
    fi
    if (cd "$KSU_ROOT" && patch -"$STRIP" --dry-run -s < "$KSU_PATCH") >/dev/null 2>&1 &&
       (cd "$KSU_ROOT" && patch -"$STRIP" --no-backup-if-mismatch -s < "$KSU_PATCH"); then
        echo "   patched $RULES_C (-$STRIP)"
    else
        echo "! the KernelSU-Next patch did not apply to:" >&2
        echo "    $RULES_C" >&2
        echo "  Your ksud_integration.c differs from upstream dev." >&2
        echo "  Nothing was changed. Add the block from $HERE/usbadb_gate.rc by hand." >&2
        echo "  Apply by hand: (cd \"$KSU_ROOT\" && patch -p1 < $KSU_PATCH)" >&2
        exit 1
    fi
fi

echo "== wiring up Makefile"
if grep -q "usbadb_gate/" "$KDIR/drivers/misc/Makefile"; then
    echo "   already present"
else
    tab=$(printf '\t')
    append_line "$KDIR/drivers/misc/Makefile" "obj-\$(CONFIG_USBADB_GATE)${tab}+= usbadb_gate/"
    echo "   added obj line"
fi

cat <<EOF

Done. Enable it and build:

  cd $KDIR
  ./scripts/config --enable USBADB_GATE
  ./scripts/config --set-str USBADB_GATE_UDC_STATE "a600000.dwc3"
  make olddefconfig
  make ...                   # your usual arm64 build

Check it got compiled in:

  grep USBADB_GATE .config
  grep -r usbadb_gate drivers/misc/built-in.a

If you use KernelSU-Next and passed its path, the reader trigger is already in
KERNEL_SU_RC - that is the half that acts. Otherwise add the block in
patches/usbadb_gate.rc to an .rc that init parses.

Rebuild and boot, then check the reader is alive:

  ps -A | grep -c '/system/bin/sh'   # one more than before
  cat /sys/kernel/usbadb_gate/link   # adb=1 udc=configured src=event ...

On the device after boot:

  dmesg | grep usbadb_gate           # "detect only", then "udc=... -> adb ..."
  cat /sys/kernel/usbadb_gate/link   # adb=1 udc=configured src=event ...
  cat /sys/usbadb_gate/state 2>/dev/null || cat /sys/kernel/usbadb_gate/state

Confirm the reader is alive:

  ps -A | grep -c 'sh -c'            # the loop from the rc
  adb shell 'cat /dev/usbadb_gate'   # blocks; prints one line per verdict change

Override by hand at any time:

  echo 0  > /sys/kernel/usbadb_gate/state   # adb off, ignore the cable
  echo pc > /sys/kernel/usbadb_gate/state   # back to cable-driven
  echo 1  > /sys/kernel/usbadb_gate/state   # adb on, ignore the cable

The driver runs nothing. No helper binary, no module, no zip, no /data artifact
of ours: one sh loop started by init, using the /system/bin tools.
EOF
