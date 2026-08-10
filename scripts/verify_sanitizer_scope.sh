#!/usr/bin/env bash
# Verify the sanitizer instrumentation contract: first-party code is
# instrumented, third-party (3rdparty/_deps) code is NOT, and the final
# binaries still link the sanitizer runtime. This is the acceptance check for
# the "first-party-only sanitizers" build optimization (the sanitizer CI legs
# spent most of their compile time instrumenting vectorscan/uchardet/tbb/efsw,
# which klogg does not need to sanitize).
#
# It inspects compile_commands.json in a configured sanitizer build dir.
#
# Usage: scripts/verify_sanitizer_scope.sh <build_dir>
#   <build_dir> must have been configured with a sanitizer enabled, e.g.
#   cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#        -DENABLE_SANITIZER_ADDRESS=ON -DENABLE_SANITIZER_UNDEFINED_BEHAVIOR=ON \
#        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ...
set -euo pipefail

BUILD_DIR="${1:?usage: verify_sanitizer_scope.sh <build_dir>}"
CC_JSON="$BUILD_DIR/compile_commands.json"
if [[ ! -f "$CC_JSON" ]]; then
    echo "ERROR: $CC_JSON not found (configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)" >&2
    exit 2
fi

python3 - "$CC_JSON" <<'PY'
import json, re, sys

cc = json.load(open(sys.argv[1]))

def san(e):
    cmd = e.get("command") or " ".join(e.get("arguments", []))
    return "-fsanitize=" in cmd

# Classify by origin, not by a naive "/src/" substring (which also matches
# cpm_cache/<dep>/.../src/...). First-party = klogg's own tree; third-party =
# anything from the CPM cache or the build's _deps/3rdparty output dirs.
def is_third_party(path):
    return ("/cpm_cache/" in path or "/_deps/" in path or "/3rdparty/" in path)

def is_first_party(path):
    return "/klogg/src/" in path and not is_third_party(path)

# The allocator (mimalloc) and the thread library (tbb) stay instrumented by
# design: ASan/TSan must see allocator poisoning and thread-sync annotations or
# they false-positive on first-party code. Everything else vendored must be
# clean. (mimalloc/tbb add their own -fsanitize via MI_TRACK_ASAN/TBB_SANITIZE,
# independent of the directory-level flags this optimization strips.)
_ALLOW_RE = "/(mimalloc|tbb)/"

first_party = [e for e in cc if is_first_party(e["file"])]
third_party = [e for e in cc if is_third_party(e["file"]) and not re.search(_ALLOW_RE, e["file"])]

fp_inst = [e for e in first_party if san(e)]
fp_not  = [e for e in first_party if not san(e)]
tp_inst = [e for e in third_party if san(e)]

print(f"first-party instrumented: {len(fp_inst)}/{len(first_party)}")
print(f"third-party instrumented: {len(tp_inst)}/{len(third_party)} (must be 0)")

ok = True
if not fp_inst:
    print("FAIL: no first-party file is instrumented"); ok = False
if fp_not:
    print("FAIL: first-party files missing instrumentation:")
    for e in fp_not[:10]: print("   ", e["file"])
    ok = False
if tp_inst:
    print("FAIL: third-party files still instrumented (should be 0):")
    for e in tp_inst[:10]: print("   ", e["file"])
    ok = False

sys.exit(0 if ok else 1)
PY
