#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Add the gadget state notifier to a tree where 0001 does not apply.
#
#   sh apply_notifier_fallback.sh /path/to/kernel
#
# 0001 is a normal context patch. It applies clean to upstream v5.4, v5.10,
# v5.15, v6.1, v6.6 and v6.12, but vendor forks add and move code inside
# usb_gadget_set_state() and around its prototype, and then the context lines no
# longer match even though the function is still there.
#
# This does the same three edits by locating things by name instead of by line:
#
#   core.c    the notifier chain + register/unregister, immediately before
#             usb_gadget_set_state(); one call inside it, after
#             schedule_work(&gadget->work)
#   gadget.h  the two externs, after the usb_gadget_set_state() prototype
#   core.c    #include <linux/notifier.h>, if it is not reachable already
#
# It refuses to guess: if it cannot find the function, or finds it twice, it
# changes nothing and says what it wanted. Nothing is written until every anchor
# has been found, so a half-patched tree is not a possible outcome.

set -eu

KDIR=${1:-}
[ -n "$KDIR" ] || { echo "usage: $0 /path/to/kernel" >&2; exit 2; }

CORE="$KDIR/drivers/usb/gadget/udc/core.c"
HEADER="$KDIR/include/linux/usb/gadget.h"
[ -f "$CORE" ] || { echo "! no $CORE" >&2; exit 1; }
[ -f "$HEADER" ] || { echo "! no $HEADER" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

say() { printf '%s\n' "$*"; }

# --- already there? -------------------------------------------------------
if grep -q 'usb_gadget_register_state_notifier' "$CORE" &&
   grep -q 'usb_gadget_register_state_notifier' "$HEADER"; then
    say "   already applied"
    exit 0
fi

# --- core.c ---------------------------------------------------------------
# Find the function by name, not by its exact signature: vendor trees reflow the
# argument list onto one line, add attributes, or both. Then find its body and
# put the call at the end of it.
n=$(grep -c '^void usb_gadget_set_state(' "$CORE" || true)
if [ "$n" != "1" ]; then
    say "! expected exactly one usb_gadget_set_state() definition, found $n" >&2
    say "  in $CORE. Nothing changed." >&2
    exit 1
fi

# The call it inserts references both parameters. If this tree's function does
# not take them, inserting it produces code that cannot compile, so stop here.
sig=$(awk '/^void usb_gadget_set_state\(/ { f = 1 } f { print } f && /\{/ { exit }' "$CORE")
case $sig in
    *"struct usb_gadget *gadget"*) ;;
    *)  say "! usb_gadget_set_state() does not take a usb_gadget *gadget argument:" >&2
        say "    $(printf '%s' "$sig" | head -1)" >&2
        say "  The notifier call needs it. Nothing changed." >&2
        exit 1 ;;
esac
case $sig in
    *"enum usb_device_state state"*) ;;
    *)  say "! usb_gadget_set_state() does not take an enum usb_device_state state argument:" >&2
        say "    $(printf '%s' "$sig" | head -1)" >&2
        say "  The notifier call needs it. Nothing changed." >&2
        exit 1 ;;
esac

awk '
    BEGIN {
        state = "before"
        depth = 0
        inserted = 0
        body = ""
    }

    # the definition, however the signature is laid out
    state == "before" && /^void usb_gadget_set_state\(/ {
        printf "%s", CHAIN
        print
        state = "signature"
        next
    }

    # count braces from the opening one so the end of the function is known
    state == "signature" || state == "body" {
        line = $0
        if (state == "signature" && index(line, "{") > 0)
            state = "body"
        if (state == "body") {
            opens = gsub(/\{/, "{", line)
            closes = gsub(/\}/, "}", line)
            depth += opens - closes
            # the natural place: right after the state is stored and the work
            # queued. If the tree does not do that any more, fall back to the
            # last statement of the body.
            if (depth == 1 && line ~ /(schedule_work|queue_work)\(/) {
                print
                print "\tatomic_notifier_call_chain(&usb_gadget_state_notifier, state, gadget);"
                inserted = 1
                next
            }
            if (depth == 0) {
                if (!inserted)
                    print "\tatomic_notifier_call_chain(&usb_gadget_state_notifier, state, gadget);"
                print
                state = "after"
                next
            }
        }
        print
        next
    }

    { print }

    END {
        if (state == "before") {
            print "! usb_gadget_set_state() was not found; nothing written" > "/dev/stderr"
            exit 1
        }
        if (state != "after") {
            print "! usb_gadget_set_state() has no closing brace; nothing written" > "/dev/stderr"
            exit 1
        }
    }

    BEGIN {
        CHAIN = ""
        CHAIN = CHAIN "\n"
        CHAIN = CHAIN "/*\n"
        CHAIN = CHAIN " * Atomic notifier chain: usb_gadget_set_state() is reachable from interrupt\n"
        CHAIN = CHAIN " * and UDC completion context, so callbacks must not sleep.  They get the\n"
        CHAIN = CHAIN " * state that was just set as the event value.\n"
        CHAIN = CHAIN " */\n"
        CHAIN = CHAIN "static ATOMIC_NOTIFIER_HEAD(usb_gadget_state_notifier);\n"
        CHAIN = CHAIN "\n"
        CHAIN = CHAIN "int usb_gadget_register_state_notifier(struct notifier_block *nb)\n"
        CHAIN = CHAIN "{\n"
        CHAIN = CHAIN "\treturn atomic_notifier_chain_register(&usb_gadget_state_notifier, nb);\n"
        CHAIN = CHAIN "}\n"
        CHAIN = CHAIN "EXPORT_SYMBOL_GPL(usb_gadget_register_state_notifier);\n"
        CHAIN = CHAIN "\n"
        CHAIN = CHAIN "void usb_gadget_unregister_state_notifier(struct notifier_block *nb)\n"
        CHAIN = CHAIN "{\n"
        CHAIN = CHAIN "\tatomic_notifier_chain_unregister(&usb_gadget_state_notifier, nb);\n"
        CHAIN = CHAIN "}\n"
        CHAIN = CHAIN "EXPORT_SYMBOL_GPL(usb_gadget_unregister_state_notifier);\n"
        CHAIN = CHAIN "\n"
    }
' "$CORE" > "$TMP/core.c" || { say "! core.c: could not place the notifier, nothing changed" >&2; exit 1; }

grep -q 'atomic_notifier_call_chain(&usb_gadget_state_notifier' "$TMP/core.c" || {
    say "! core.c: the call was not inserted, nothing changed" >&2
    exit 1
}
# The include has to be there for ATOMIC_NOTIFIER_HEAD. Upstream gets it
# transitively through device.h, which is not something to rely on in a fork.
if ! grep -q '^#include <linux/notifier.h>' "$TMP/core.c"; then
    awk '
        { print }
        !done && /^#include <linux\/(workqueue|module)\.h>/ {
            print "#include <linux/notifier.h>"
            done = 1
        }
    ' "$TMP/core.c" > "$TMP/core2.c" && mv "$TMP/core2.c" "$TMP/core.c"
fi

# --- gadget.h -------------------------------------------------------------
n=$(grep -c '^extern void usb_gadget_set_state(' "$HEADER" || true)
if [ "$n" != "1" ]; then
    say "! expected exactly one usb_gadget_set_state() prototype, found $n" >&2
    say "  in $HEADER. Nothing changed." >&2
    exit 1
fi

awk '
    # The prototype may be one line or two; either way it ends with ");". Print
    # the line first, then the block, so nothing of the original is dropped.
    inproto {
        print
        if ($0 ~ /\);[[:space:]]*$/) {
            inproto = 0
            print ""
            print "/*"
            print " * Fired from usb_gadget_set_state() with the new state as the event value."
            print " * Atomic context: schedule work, do not sleep."
            print " */"
            print "extern int usb_gadget_register_state_notifier(struct notifier_block *nb);"
            print "extern void usb_gadget_unregister_state_notifier(struct notifier_block *nb);"
        }
        next
    }
    { print }
    /^extern void usb_gadget_set_state\(/ { inproto = 1 }
' "$HEADER" > "$TMP/gadget.h" || { say "! gadget.h: nothing changed" >&2; exit 1; }

grep -q 'usb_gadget_register_state_notifier' "$TMP/gadget.h" || {
    say "! gadget.h: the prototype layout is not what was expected, nothing changed" >&2
    exit 1
}

# --- write, only now ------------------------------------------------------
cp "$CORE" "$CORE.usbadb_gate.orig"
cp "$HEADER" "$HEADER.usbadb_gate.orig"
cat "$TMP/core.c" > "$CORE"
cat "$TMP/gadget.h" > "$HEADER"

say "   core.c:   chain + register/unregister before usb_gadget_set_state(),"
say "             call inside it, #include <linux/notifier.h>"
say "   gadget.h: two externs after the prototype"
say "   originals kept as *.usbadb_gate.orig"
say ""
say "   review with:  diff -u $CORE.usbadb_gate.orig $CORE"
