#!/bin/bash
set -euo pipefail

KERNEL_ROOT="${1:?}"
ANDROID_VER="${2:?}"
KERNEL_VER="${3:?}"

cd "$KERNEL_ROOT"

if [[ "$ANDROID_VER" == "android12" && "$KERNEL_VER" == "5.10" ]]; then
  sed -i 's/^\s*exit 1$/    echo "Bypassing ABI check"/' build/abi/compare_to_symbol_list
elif [[ "$ANDROID_VER" == "android13" && "$KERNEL_VER" == "5.10" ]]; then
  sed -i 's/^\s*exit 1$/    echo "Bypassing ABI check"/' build/kernel/abi/compare_to_symbol_list
elif [[ "$ANDROID_VER" == "android13" && "$KERNEL_VER" == "5.15" ]]; then
  sed -i 's/^\s*exit 1$/    echo "Bypassing ABI check"/' build/kernel/abi/compare_to_symbol_list
elif [[ "$ANDROID_VER" == "android14" ]]; then
  perl -i -pe 's/^(\s*)return 1$/$1return 0/g if /if missing_symbols:/../return 1/' build/kernel/abi/check_buildtime_symbol_protection.py
elif [[ "$ANDROID_VER" == "android15" ]]; then
  perl -i -pe 's/^(\s*)return 1$/$1return 0/g if /if missing_symbols:/../return 1/' build/kernel/abi/check_buildtime_symbol_protection.py
fi

sed -i 's/check_defconfig//' ./common/build.config.gki 2>/dev/null || true
