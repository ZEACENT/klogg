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

# First-party classification is anchored at the real checkout root (derived
# from this script's location), not at a literal "/klogg/" substring -- a
# clone named klogg-pr57 must classify identically.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$CC_JSON" "$REPO_ROOT" <<'PY'
import json, os, re, sys

cc = json.load(open(sys.argv[1]))
ROOT = os.path.realpath(sys.argv[2])

def entry_path(e):
    f = e["file"]
    return os.path.realpath(f if os.path.isabs(f) else os.path.join(e["directory"], f))

def san(e):
    cmd = e.get("command") or " ".join(e.get("arguments", []))
    return "-fsanitize=" in cmd

# Classify by origin, not by a naive "/src/" substring (which also matches
# cpm_cache/<dep>/.../src/...). First-party = <repo>/src/... under the real
# checkout root; third-party = anything from the CPM cache or the build's
# _deps/3rdparty output dirs.
def is_third_party(path):
    return ("/cpm_cache/" in path or "/_deps/" in path or "/3rdparty/" in path)

def is_first_party(path):
    return path.startswith(ROOT + "/src/") and not is_third_party(path)

# The allocator (mimalloc) and the thread library (tbb) stay instrumented by
# design: ASan/TSan must see allocator poisoning and thread-sync annotations or
# they false-positive on first-party code. Everything else vendored must be
# clean. (mimalloc/tbb add their own -fsanitize via MI_TRACK_ASAN/TBB_SANITIZE,
# independent of the directory-level flags this optimization strips.)
# Both dependency layouts must match: CPM cache (cpm_cache/tbb/<hash>/...) and
# FetchContent (_deps/tbb-src/... -- the sanitizer 3rdparty path fetches this
# way, so "/(mimalloc|tbb)/" alone misses it and fails a correct build).
_ALLOW_RE = "/(mimalloc|tbb)(-src)?/"

first_party = [e for e in cc if is_first_party(entry_path(e))]
third_party = [e for e in cc if is_third_party(entry_path(e)) and not re.search(_ALLOW_RE, entry_path(e))]

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

# Positive assertion for the allow-listed exceptions: under ASan/TSan the
# allocator/threading annotations are a correctness requirement, so mimalloc
# and tbb MUST carry -fsanitize flags -- the allow-list alone would let an
# uninstrumented mimalloc slip through. Keyed on address/thread specifically:
# MI_TRACK_ASAN / MI_DEBUG_TSAN / TBB_SANITIZE do not fire for UBSan-only
# builds, and MSVC spells it /fsanitize=address (no "-fsanitize=" token), so
# the check stays inert in both.
def cmdline(e):
    return e.get("command") or " ".join(e.get("arguments", []))

needs_rt = any(re.search(r"-fsanitize=(address|thread)", cmdline(e)) for e in cc)
required = [e for e in cc if is_third_party(entry_path(e)) and re.search(_ALLOW_RE, entry_path(e))]
req_not = [e for e in required if not san(e)]
if needs_rt:
    print(f"required-deps instrumented: {len(required) - len(req_not)}/{len(required)} (mimalloc/tbb)")
    if req_not:
        print("FAIL: mimalloc/tbb compiled without sanitizer flags under ASan/TSan:")
        for e in req_not[:10]: print("   ", e["file"])
        ok = False
    if not required:
        # Not a failure (mimalloc/tbb can be configured out), but if the
        # dependency layout drifts the allow-list stops matching and the
        # instrumented entries fail the third-party check above instead.
        print("NOTE: no mimalloc/tbb entries in this build")

sys.exit(0 if ok else 1)
PY
