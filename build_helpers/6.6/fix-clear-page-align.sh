# fix-clear-page-align.sh — apply regardless of surrounding comment differences
FILE="arch/arm64/lib/clear_page.S"
if ! grep -q "p2align 4" "$FILE"; then
    # insert alignment directive immediately before the function start,
    # matching either SYM_FUNC_START_PI or the older ENTRY() macro
    sed -i '/^SYM_FUNC_START_PI(clear_page)/i\\t.p2align 4' "$FILE"
    if ! grep -q "p2align 4" "$FILE"; then
        sed -i '/^ENTRY(clear_page)/i\\t.p2align 4' "$FILE"
    fi
fi