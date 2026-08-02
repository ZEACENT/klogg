#!/usr/bin/env bash
set -euo pipefail

workspace=${GITHUB_WORKSPACE:?GITHUB_WORKSPACE is required}
runner_temp=${RUNNER_TEMP:?RUNNER_TEMP is required}
archive="${workspace}/cpm-cache.tar.gz"
manifest="${runner_temp}/cpm-cache-manifest.txt"
staging="${workspace}/.cpm-cache-restore"
trap 'rm -rf "${staging}"' EXIT

test -f "${archive}"
gzip -t "${archive}"
tar -tzf "${archive}" > "${manifest}"

required_paths=(
  "cpm_cache/croaring/ba5bf40909b6935a298d4d2231f2072e6de80041/roaring.pc.in"
  "cpm_cache/croaring/ba5bf40909b6935a298d4d2231f2072e6de80041/tests/config.h.in"
  "cpm_cache/croaring/ba5bf40909b6935a298d4d2231f2072e6de80041/src/CMakeLists.txt"
)
for required_path in "${required_paths[@]}"; do
  if ! grep -Fx "${required_path}" "${manifest}" > /dev/null; then
    printf 'CPM cache archive is missing %s\n' "${required_path}" >&2
    exit 1
  fi
done

rm -rf "${staging}"
mkdir -p "${staging}"
tar -xzf "${archive}" -C "${staging}"

for required_path in "${required_paths[@]}"; do
  if [ ! -f "${staging}/${required_path}" ]; then
    printf 'CPM cache extraction is missing %s\n' "${required_path}" >&2
    exit 1
  fi
done

rm -rf "${workspace}/cpm_cache"
mv "${staging}/cpm_cache" "${workspace}/cpm_cache"
