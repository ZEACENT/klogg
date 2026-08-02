#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <Qt executable> <TSan Qt prefix>" >&2
  exit 2
fi

executable=$1
qt_prefix=$2
qt_lib_prefix="${qt_prefix}/lib/"
offscreen_plugin="${qt_prefix}/plugins/platforms/libqoffscreen.so"
nm_command=${NM:-llvm-nm-14}

if [[ ! -x "${executable}" ]]; then
  echo "TSan Qt preflight executable is missing: ${executable}" >&2
  exit 1
fi
if [[ ! -f "${offscreen_plugin}" ]]; then
  echo "TSan Qt offscreen plugin is missing: ${offscreen_plugin}" >&2
  exit 1
fi
if ! command -v "${nm_command}" >/dev/null 2>&1; then
  echo "TSan Qt compiler-instrumentation verifier is missing: ${nm_command}" >&2
  exit 1
fi

ldd_output=$(ldd "${executable}")
printf '%s\n' "${ldd_output}"

qt_library_count=0
while IFS= read -r line; do
  [[ ${line} == *libQt5* ]] || continue
  qt_library_count=$((qt_library_count + 1))
  resolved=${line#*=> }
  resolved=${resolved%% *}
  if [[ ${resolved} != "${qt_lib_prefix}"* ]]; then
    echo "Qt library resolved outside the expected TSan prefix: ${line}" >&2
    exit 1
  fi
  qt_symbols=$( "${nm_command}" -D --undefined-only "${resolved}" )
  if ! grep -E '__tsan_(func_entry|read|write)' <<< "${qt_symbols}" >/dev/null; then
    echo "Qt library is not compiler-instrumented for TSan: ${resolved}" >&2
    exit 1
  fi
done <<< "${ldd_output}"

if [[ ${qt_library_count} -eq 0 ]]; then
  echo "No Qt 5 libraries were resolved by ldd" >&2
  exit 1
fi

debug_log=$(mktemp)
trap 'rm -f "${debug_log}"' EXIT
QT_DEBUG_PLUGINS=1 \
QT_QPA_PLATFORM=offscreen \
QT_PLUGIN_PATH="${qt_prefix}/plugins" \
  "${executable}" --help > /dev/null 2> "${debug_log}"

expected_load="loaded library \"${offscreen_plugin}\""
loaded_offscreen=$(grep -E 'loaded library .*libqoffscreen\.so' "${debug_log}" || true)
if ! grep -F "${expected_load}" "${debug_log}" >/dev/null; then
  printf '%s\n' "Qt plugin diagnostics:" >&2
  grep -E 'platforms|qoffscreen|loaded library' "${debug_log}" >&2 || true
  echo "Qt did not load the expected TSan offscreen plugin: ${offscreen_plugin}" >&2
  exit 1
fi

while IFS= read -r loaded_line; do
  [[ -z ${loaded_line} || ${loaded_line} == *"${expected_load}"* ]] && continue
  echo "Qt loaded an offscreen plugin outside the expected TSan prefix: ${loaded_line}" >&2
  exit 1
done <<< "${loaded_offscreen}"
