#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf 'usage: %s (--root DIRECTORY | --manifest FILE)\n' "$0" >&2
  exit 2
}

[ "$#" -eq 2 ] || usage
mode=$1
source_path=$2
required_paths=(
  "cpm_cache/croaring/ba5bf40909b6935a298d4d2231f2072e6de80041/roaring.pc.in"
  "cpm_cache/croaring/ba5bf40909b6935a298d4d2231f2072e6de80041/tests/config.h.in"
  "cpm_cache/croaring/ba5bf40909b6935a298d4d2231f2072e6de80041/src/CMakeLists.txt"
)

for required_path in "${required_paths[@]}"; do
  case "${mode}" in
    --root)
      if [ ! -f "${source_path}/${required_path}" ]; then
        printf 'CPM cache extraction is missing %s\n' "${required_path}" >&2
        exit 1
      fi
      ;;
    --manifest)
      if ! grep -Fx "${required_path}" "${source_path}" > /dev/null; then
        printf 'CPM cache archive is missing %s\n' "${required_path}" >&2
        exit 1
      fi
      ;;
    *) usage ;;
  esac
done
