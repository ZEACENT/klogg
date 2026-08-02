#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

if (( $# != 1 )); then
    printf 'usage: %s <runtime-root>\n' "$0" >&2
    exit 2
fi

runtime_root=$1
if [[ ! -d "${runtime_root}" ]]; then
    printf 'runtime root does not exist: %s\n' "${runtime_root}" >&2
    exit 2
fi

artifact_manifest=$( mktemp )
trap 'rm -f "${artifact_manifest}"' EXIT
if ! find "${runtime_root}" -type f -print0 > "${artifact_manifest}"; then
    printf 'failed to enumerate runtime artifacts under %s\n' "${runtime_root}" >&2
    exit 1
fi

status=0
while IFS= read -r -d '' artifact; do
    description=$( file -- "${artifact}" )
    if ! grep -q ELF <<< "${description}"; then
        continue
    fi

    if ! dependencies=$( ldd "${artifact}" 2>&1 ); then
        if grep -Eq 'not a dynamic executable|statically linked' <<< "${dependencies}"; then
            continue
        fi
        printf 'failed to inspect runtime dependencies for %s:\n%s\n' \
            "${artifact}" "${dependencies}" >&2
        status=1
        continue
    fi

    missing=$( grep 'not found' <<< "${dependencies}" || true )
    if [[ -n "${missing}" ]]; then
        printf 'missing runtime dependencies for %s:\n%s\n' \
            "${artifact}" "${missing}" >&2
        status=1
    fi
done < "${artifact_manifest}"

exit "${status}"
