#!/bin/bash
set -euo pipefail

FRAGMENT_SRC="${1:?}"
FRAGMENT_DST="${2:?}"
DEFCONFIG="${3:?}"
shift 3

ADD_SUSFS=false
ADD_OVERLAYFS=false
ADD_ZRAM=false
ADD_KPM=false
USE_KLEAF=false

for arg in "$@"; do
  case "$arg" in
    --susfs) ADD_SUSFS=true ;;
    --overlayfs) ADD_OVERLAYFS=true ;;
    --zram) ADD_ZRAM=true ;;
    --kpm) ADD_KPM=true ;;
    --kleaf) USE_KLEAF=true ;;
  esac
done

extract_section() {
  awk "/^# \\[$1\\]/{found=1; next} /^# \\[/{found=0} found && NF" "$FRAGMENT_SRC"
}

extract_section "base" >> "$FRAGMENT_DST"
$ADD_SUSFS && extract_section "susfs" >> "$FRAGMENT_DST"
$ADD_OVERLAYFS && extract_section "overlayfs" >> "$FRAGMENT_DST"
$ADD_ZRAM && extract_section "zram" >> "$FRAGMENT_DST"
$ADD_KPM && extract_section "kpm" >> "$FRAGMENT_DST"

# dedup fragment: last-wins per CONFIG_ key
tac "$FRAGMENT_DST" | awk -F= '/^CONFIG_/{if(seen[$1]++)next} {print}' | tac > "${FRAGMENT_DST}.tmp"
mv "${FRAGMENT_DST}.tmp" "$FRAGMENT_DST"

if $USE_KLEAF; then
  # Kleaf applies fragment via --defconfig_fragment; don't touch gki_defconfig
  # Convert =n to "# is not set" format (Kleaf can't match =n against savedefconfig)
  sed -i 's/^\(CONFIG_[A-Za-z0-9_]*\)=n$/# \1 is not set/' "$FRAGMENT_DST"
else
  # Legacy build.sh doesn't merge fragments — configs must be in gki_defconfig
  grep '=n$' "$FRAGMENT_DST" >> "$DEFCONFIG" 2>/dev/null || true
  sed -i '/=n$/d' "$FRAGMENT_DST"
  cat "$FRAGMENT_DST" >> "$DEFCONFIG"
fi

if $ADD_ZRAM; then
  sed -i 's/CONFIG_ZRAM=m/CONFIG_ZRAM=y/g' "$DEFCONFIG" 2>/dev/null || true
  sed -i 's/CONFIG_ZSMALLOC=m/CONFIG_ZSMALLOC=y/g' "$DEFCONFIG" 2>/dev/null || true
fi

if ! $USE_KLEAF; then
  tac "$DEFCONFIG" | awk -F= '/^CONFIG_/{if(seen[$1]++)next} {print}' | tac > "${DEFCONFIG}.tmp"
  mv "${DEFCONFIG}.tmp" "$DEFCONFIG"
fi
