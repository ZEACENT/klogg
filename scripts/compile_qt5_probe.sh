#!/usr/bin/env bash
# Compile-probe the files that have repeatedly broken the Qt 5 Linux CI legs
# with -Werror=conversion (qsizetype -> int narrowing), using the LOCAL Qt 5
# headers (brew qt@5) instead of a Docker container. This closes the gap where
# a dev on Qt 6 (macOS) cannot see a Qt-5-only narrowing until CI.
#
# Background: PR #48 (FolderSearchResults::ensureMarkLines), PR #56
# (readMarkLineSeek QByteArray::at, then the ContainerIndex binary-search chain
# in wrappedstring.h) all failed the Qt 5 Linux legs this way. The fix pattern
# is "index type must match the RECEIVER's size type on both Qt versions":
# QByteArray/QString -> klogg::ContainerIndex, QStringView -> plain qsizetype.
#
# Usage:  scripts/compile_qt5_probe.sh          # exit 0 = clean on Qt 5
# Requires: brew install qt@5

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QT5="$(brew --prefix qt@5 2>/dev/null || true)"
if [[ -z "$QT5" || ! -f "$QT5/include/QtCore/qbytearray.h" ]]; then
    echo "SKIP: qt@5 headers not found (brew install qt@5)" >&2
    exit 0
fi

# The cpm_cache dependency dirs are content-hash-named and change whenever a
# pinned dependency is upgraded, so they must be discovered, not hardcoded
# (hardcoding made this probe silently fail after a dep bump).
TS_ROOT="$(find "$REPO_ROOT/cpm_cache/type_safe" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | head -1)"
MI_ROOT="$(find "$REPO_ROOT/cpm_cache/mimalloc" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | head -1)"
if [[ -z "$TS_ROOT" || -z "$MI_ROOT" ]]; then
    echo "SKIP: cpm_cache type_safe/mimalloc not found (run a cmake configure first)" >&2
    exit 0
fi
TS_DIR="$TS_ROOT"
MI_DIR="$MI_ROOT/include"

COMMON_FLAGS=(
    -std=gnu++17 -Werror=conversion -Wsign-conversion
    -I"$REPO_ROOT/src/ui/include"
    -I"$REPO_ROOT/src/logdata/include"
    -I"$REPO_ROOT/src/utils/include"
    -I"$REPO_ROOT/src/logging/include"
    -isystem "$QT5/include"
    -isystem "$QT5/include/QtCore"
    -isystem "$QT5/include/QtGui"
    -isystem "$QT5/include/QtWidgets"
    -isystem "$QT5/include/QtNetwork"
    -isystem "$QT5/include/QtConcurrent"
    -isystem "$TS_DIR/include"
    -isystem "$TS_DIR/external/debug_assert"
    -isystem "$MI_DIR"
    -DTYPE_SAFE_ENABLE_ASSERTIONS=0
    -DTYPE_SAFE_ENABLE_WRAPPER=1
    -DTYPE_SAFE_ARITHMETIC_POLICY=1
)

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

probe() { # probe <name> <source-file>
    local name="$1" src="$2"
    if c++ "${COMMON_FLAGS[@]}" -c "$src" -o "$TMP/$name.o" 2>"$TMP/$name.err"; then
        echo "OK   $name (Qt 5 $( "$QT5/bin/qmake" -query QT_VERSION ))"
    else
        echo "FAIL $name -- Qt 5 -Werror=conversion:" >&2
        grep -E "error" "$TMP/$name.err" >&2 || cat "$TMP/$name.err" >&2
        return 1
    fi
}

fail=0
# A translation unit that instantiates the wrapped-string binary search.
cat > "$TMP/ws_probe.cpp" <<'CPP'
#include "wrappedstring.h"
#include <QString>
static int tw( QStringView s ) { return static_cast<int>( s.size() ) * 7; }
void run() {
    QString longLine( 500, QLatin1Char( 'x' ) );
    longLine.replace( 100, 1, QLatin1Char( ' ' ) );
    WrappedString w( longLine, 200, tw );
    (void)w.wrappedLinesCount();
}
CPP
probe wrappedstring "$TMP/ws_probe.cpp" || fail=1
probe foldersearchresults "$REPO_ROOT/src/logdata/src/foldersearchresults.cpp" || fail=1

if [[ $fail -eq 0 ]]; then
    echo "Qt 5 compile probe: all clean."
else
    echo "Qt 5 compile probe: FAILURES above would break the Qt 5 Linux CI legs." >&2
fi
exit $fail
