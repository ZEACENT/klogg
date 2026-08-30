#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf 'usage: %s (--root DIRECTORY | --manifest FILE)\n' "$0" >&2
  exit 2
}

[ "$#" -eq 2 ] || usage
mode=$1
source_path=$2
required_suffixes=(
  "roaring.pc.in"
  "tests/config.h.in"
  "src/CMakeLists.txt"
)

case "${mode}" in
  --root)
    croaring_root="${source_path}/cpm_cache/croaring"
    found_package=false
    for package_root in "${croaring_root}"/*; do
      [ -d "${package_root}" ] || continue
      found_package=true
      relative_root="cpm_cache/croaring/$( basename "${package_root}" )"
      for suffix in "${required_suffixes[@]}"; do
        if [ ! -f "${package_root}/${suffix}" ]; then
          printf 'CPM cache extraction is missing %s/%s\n' "${relative_root}" "${suffix}" >&2
          exit 1
        fi
      done
    done
    if [ "${found_package}" != true ]; then
      printf 'CPM cache extraction contains no CRoaring package\n' >&2
      exit 1
    fi
    ;;
  --manifest)
    package_roots=$( grep -E '^cpm_cache/croaring/[^/]+/' "${source_path}" \
      | cut -d/ -f1-3 | sort -u )
    if [ -z "${package_roots}" ]; then
      printf 'CPM cache archive contains no CRoaring package\n' >&2
      exit 1
    fi
    while IFS= read -r package_root; do
      for suffix in "${required_suffixes[@]}"; do
        required_path="${package_root}/${suffix}"
        if ! grep -Fx "${required_path}" "${source_path}" > /dev/null; then
          printf 'CPM cache archive is missing %s\n' "${required_path}" >&2
          exit 1
        fi
      done
    done <<< "${package_roots}"
    ;;
  *) usage ;;
esac
