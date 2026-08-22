#!/bin/bash
set -euo pipefail

KERNEL_ROOT="${1:?}"
KERNEL_VER="${2:?}"
SUFFIX="${3:-SukiSU}"

cd "$KERNEL_ROOT"

if [[ "$KERNEL_VER" == "5."* ]] || [[ "$KERNEL_VER" == "6.1" ]]; then
  perl -i -0777 -pe "s/(.*)echo \"\\\$res\"/\$1echo \"\\\$res-${SUFFIX}\"/s" ./common/scripts/setlocalversion
else
  perl -i -0777 -pe "s/(.*)echo \"\\\$\\{KERNELVERSION\\}\\\$\\{file_localversion\\}\\\$\\{config_localversion\\}\\\$\\{LOCALVERSION\\}\\\$\\{scm_version\\}\"/\$1echo \"\\\${KERNELVERSION}\\\${file_localversion}\\\${config_localversion}\\\${LOCALVERSION}-${SUFFIX}\\\${scm_version}\"/s" ./common/scripts/setlocalversion
fi

if [ -f "build/build.sh" ]; then
  sed -i 's/-dirty//' ./common/scripts/setlocalversion
else
  sed -i "/stable_scmversion_cmd/s/-maybe-dirty//g" ./build/kernel/kleaf/impl/stamp.bzl
  sed -i 's/-dirty//' ./common/scripts/setlocalversion
  rm -rf ./common/android/abi_gki_protected_exports_*
  perl -pi -e 's/^\s*"protected_exports_list"\s*:\s*"android\/abi_gki_protected_exports_aarch64",\s*$//;' ./common/BUILD.bazel
fi

cd common
git config --global user.name "github-actions[bot]"
git config --global user.email "github-actions[bot]@users.noreply.github.com"
git add .
git commit -m "${SUFFIX}: Clean Build" || true
