#!/usr/bin/env bash
set -euo pipefail

workspace=${GITHUB_WORKSPACE:?GITHUB_WORKSPACE is required}
runner_temp=${RUNNER_TEMP:?RUNNER_TEMP is required}
script_dir=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )
case "${workspace}" in
  [A-Za-z]:\\* | [A-Za-z]:/*)
    if ! command -v cygpath > /dev/null; then
      printf 'cygpath is required for Windows workspace paths\n' >&2
      exit 1
    fi
    workspace=$( cygpath -u "${workspace}" )
    runner_temp=$( cygpath -u "${runner_temp}" )
    ;;
esac
archive="${workspace}/cpm-cache.tar.gz"
manifest="${runner_temp}/cpm-cache-manifest.txt"
staging="${workspace}/.cpm-cache-restore"
trap 'rm -rf "${staging}"' EXIT

test -f "${archive}"
gzip -t "${archive}"
tar -tzf "${archive}" > "${manifest}"

"${script_dir}/check_cpm_cache_contract.sh" --manifest "${manifest}"

rm -rf "${staging}"
mkdir -p "${staging}"
tar -xzf "${archive}" -C "${staging}"
"${script_dir}/check_cpm_cache_contract.sh" --root "${staging}"

rm -rf "${workspace}/cpm_cache"
mv "${staging}/cpm_cache" "${workspace}/cpm_cache"
