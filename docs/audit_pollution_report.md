# Klogg Core Code Pollution Audit

**Date:** 2026-07-25

## Summary

| Category | Files Affected | Occurrences | Severity |
|---|---|---|---|
| Platform OS Macros | 25+ | ~55 | High |
| Qt Version Branches | 16 | ~30 | High |
| **Test Instrumentation** | **8** | **~50 methods + 4 counters** | **High** |
| Signal Overload Disambiguation (qOverload) | 10 | ~22 | Medium |
| Feature Flags (Vectorscan, mimalloc, etc.) | 9 | ~18 | Medium |
| Compiler-Specific Pragmas | 3 (same pattern) | 3 blocks | Low |
| Perf Measurement Instrumentation | 4 | ~10 | Low (dead code) |
| Build Config Injection | N/A (CMake) | 6 definitions | Medium | |

> ✅ No test-specific code, friend declarations, or `#ifdef` blocks exist in production source files.

---

## 1. Platform OS Macros — `Q_OS_WIN` / `Q_OS_MAC` / `Q_OS_MACOS` / `_WIN64`

These are scattered across the UI, settings, logdata, app, and utils layers, often for small behavioral differences.

### 1.1 Headers (most impactful — propagate to all includers)

| File | Lines | Macro | Purpose |
|---|---|---|---|
| `src/ui/include/encodings.h` | 8, 67 | `Q_OS_WIN` | `windows.h` include, `GetACP()` for system code page |
| `src/app/kloggapp.h` | 47, 51, 145, 253 | `Q_OS_MAC`, `Q_OS_WIN` | App menu bar, taskbar, file associations |
| `src/settings/include/configuration.h` | 697, 712 | `Q_OS_MACOS`, `Q_OS_WIN` | Default download path, file monitor timeout |
| `src/utils/include/openfilehelper.h` | 65, 70 | `Q_OS_WIN`, `Q_OS_MAC` | File opening command selection |

### 1.2 Source Files — UI Layer

| File | Lines | Macro | Purpose |
|---|---|---|---|
| `src/ui/src/abstractlogview.cpp` | 113, 119 | `Q_OS_WIN`, `_WIN64` | `countLeadingZeroes()` intrinsic (MSVC `_BitScanReverse64`) |
| `src/ui/src/abstractlogview.cpp` | 1099 | `Q_OS_MACOS` | Font size modifier key (Meta on macOS, Ctrl elsewhere) |
| `src/ui/src/abstractlogview.cpp` | 1744 | `!Q_OS_WIN` | Clipboard newline normalization |
| `src/ui/src/mainwindow.cpp` | 54, 638, 1301, 2392 | `Q_OS_WIN`, `Q_OS_MAC` | Menu bar mode, shortcut text, window flags, taskbar button |
| `src/ui/src/tabbedcrawlerwidget.cpp` | 364, 374, 943 | `Q_OS_MAC`, `Q_OS_WIN`, `Q_OS_MACOS` | Context menu font, keyboard accel keys |
| `src/ui/src/tabbedscratchpad.cpp` | 83 | `Q_OS_MACOS` | Splitter widget style |
| `src/ui/src/optionsdialog.cpp` | 157, 161, 325 | `Q_OS_WIN`, `Q_OS_MAC` | Font list, encoding defaults, codec listing |
| `src/ui/src/selection.cpp` | 175 | `Q_OS_WIN` | Clipboard format selection |
| `src/ui/src/livesourcetransport.cpp` | 15 | `Q_OS_WIN` | Signal includes for process transport |
| `src/ui/src/iosdevicelistprovider.cpp` | 35, 140, 154, 172 | `Q_OS_MAC` | macOS FSEvents for iOS device detection |
| `src/ui/src/ioslogprocesstransport.cpp` | 77, 113, 140 | `Q_OS_MAC`, `Q_OS_WIN` | iOS log process transport spawning |
| `src/ui/src/adbdevicelistprovider.cpp` | 40, 63, 66 | `Q_OS_WIN`, `Q_OS_MAC` | ADB path resolution |

### 1.3 Source Files — Settings Layer

| File | Lines | Macro | Purpose |
|---|---|---|---|
| `src/settings/src/styles.cpp` | 32, 616, 618, 633, 646, 721, 734 | `Q_OS_WIN`, `Q_OS_MACOS` | Font families, palette colors, dark/light mode |
| `src/settings/src/persistentinfo.cpp` | 68, 121, 153, 164 | `Q_OS_MAC`, `Q_OS_WIN` | Config file paths (`~/Library/...` vs `%APPDATA%`) |
| `src/settings/src/shortcuts.cpp` | 30 | `Q_OS_MACOS` | `Ctrl`→`⌘` shortcut display text |

### 1.4 Source Files — Other Layers

| File | Lines | Macro | Purpose |
|---|---|---|---|
| `src/app/main.cpp` | 44, 47, 49, 137, 180, 206, 237 | `Q_OS_WIN`, `Q_OS_MAC` | HighDPI, Windows console, NSException handler, dark mode detection |
| `src/logdata/src/logfiltereddata.cpp` | 86, 685 | `Q_OS_WIN` | File size retrieval |
| `src/logdata/src/fileholder.cpp` | 5, 21, 180 | `Q_OS_WIN` | File handle management, read-ahead control |
| `src/utils/src/cpu_info.cpp` | 23, 88 | `Q_OS_WIN`, `Q_OS_LINUX` | CPU feature detection |
| `src/versioncheck/src/versionchecker.cpp` | 94, 96 | `Q_OS_MAC`, `Q_OS_WIN` | Update URL platform suffix |

### 1.5 Crash Handler (acceptable — inherently platform-specific)

| File | Lines | Macro | Notes |
|---|---|---|---|
| `src/crash_handler/src/crashhandler.cpp` | 171, 183, 194, 258 | `Q_OS_WIN` | Stack trace, crash dump — **acceptable** |
| `src/crash_handler/src/memory_info.cpp` | 26, 47 | `Q_OS_WIN`, `Q_OS_APPLE` | Memory stats — **acceptable** |

**Abstraction candidates:**
- `countLeadingZeroes()` → should be a `PlatformIntrinsics` utility
- Font size modifier key → `PlatformInput::fontSizeModifier()`
- Config file paths → `PlatformPaths` (already partially abstracted via `persistentinfo.cpp`)
- Clipboard normalization → `PlatformClipboard`
- File handle management → `PlatformFileHandle`
- CPU feature detection → `PlatformCpuInfo`

---

## 2. Qt Version Branches — `QT_VERSION` / `QT_VERSION_CHECK`

### 2.1 Headers

| File | Lines | Check | Purpose |
|---|---|---|---|
| `src/ui/include/fontutils.h` | 38, 58 | `QT_VERSION < 6.0.0` | `QFontDatabase::families()` static vs instance method |
| `src/utils/include/active_screen.h` | 30 | `QT_VERSION >= 5.14.0` | `QWidget::screen()` availability |

### 2.2 Source Files

| File | Lines | Check | Purpose |
|---|---|---|---|
| `src/app/main.cpp` | 179, 189 | `< 6.0.0`, `>= 5.14.0` | Font resolution method, HighDPI policy name |
| `src/ui/src/quickfind.cpp` | 207, 240, 257, 272 | `< 6.0.0` | `QtConcurrent::run` overload resolution (4 sites) |
| `src/ui/src/quickfindwidget.cpp` | 133, 143 | `>= 6.7.0` | `QLineEdit::setClearButtonEnabled` |
| `src/ui/src/tabbedcrawlerwidget.cpp` | 540, 736 | `>= 5.15.0` | `QTabBar::tabAt()`, `setTabVisible()` |
| `src/ui/src/crawlerwidget.cpp` | 504 | `>= 5.15.0` | `QTabBar::tabAt()` |
| `src/ui/src/foldercrawlerwidget.cpp` | 1282 | `>= 5.15.0` | `QTabBar::tabAt()` |
| `src/ui/src/tabgroup.cpp` | 177 | `>= 5.15.0` | `QTabBar::tabAt()` |
| `src/ui/src/predefinedfilterscombobox.cpp` | 66 | `>= 5.15.0` | `QComboBox::setPlaceholderText()` |
| `src/ui/src/newversiondialog.cpp` | 75 | `>= 5.14.0` | `QLabel::setTextFormat(MarkdownText)` |
| `src/settings/src/configuration.cpp` | 161, 172 | `<= 6.4.0` | Qt 6.4 settings migration (version-specific workaround) |
| `src/settings/src/styles.cpp` | 86, 648 | `>= 6.5.0` | `QPalette::accent()` color role |
| `src/filewatch/src/filewatcher.cpp` | 33 | `< 6` | `QFSFileEngine` include |
| `src/versioncheck/src/versionchecker.cpp` | 67 | `>= 6.4.0` | `QNetworkInformation::reachability()` |
| `src/versioncheck/src/versionchecker.cpp` | 222, 230 | `>= 5.15.0` / `< 5.15.0` | `QNetworkRequest::setTransferTimeout()` |

**Key patterns that could be abstracted:**
- **`QFontDatabase` static vs instance** (2 sites in `fontutils.h`) → `QtCompat::fontFamilies()` / `QtCompat::isFixedPitch()`
- **`QtConcurrent::run` overload** (4 sites in `quickfind.cpp`) → `QtCompat::concurrentRun(this, &Class::method, args...)`
- **`QTabBar` 5.15 APIs** (5 sites across 4 files) → `QtCompat::tabBarTabAt()`, `QtCompat::setTabVisible()`
- **`QComboBox::setPlaceholderText`** (1 site) → `QtCompat::setPlaceholderText()`
- **`QNetworkRequest::setTransferTimeout`** (2 sites) → `QtCompat::setTransferTimeout()`

---

## 3. Signal Overload Disambiguation — `qOverload` / `QOverload`

All 22 occurrences are for Qt 5 compatibility (Qt6 can use regular member function pointers without overload resolution). These are **all in `.cpp` files** (not headers), so the impact is limited to compilation, not transitive include pollution.

| File | Count | Context |
|---|---|---|
| `src/ui/src/quickfind.cpp` | 4 | `QtConcurrent::run` + member function overloads |
| `src/ui/src/crawlerwidget.cpp` | 3 | `QComboBox::currentIndexChanged`, `QSpinBox::valueChanged` |
| `src/ui/src/foldercrawlerwidget.cpp` | 3 | Same — combobox + spinbox signals |
| `src/ui/src/viewsignalwiring.cpp` | 3 | `AbstractLogView::addToSearch` etc. |
| `src/ui/src/livesourcetransport.cpp` | 2 | `QProcess::finished` (int + ExitStatus overload) |
| `src/ui/src/highlighteredit.cpp` | 2 | `QSpinBox::valueChanged`, `QComboBox::currentIndexChanged` |
| `src/ui/src/searchtoolbar.cpp` | 1 | `QComboBox::currentIndexChanged` |
| `src/ui/src/adblogcatdialog.cpp` | 1 | `QComboBox::currentIndexChanged` |
| `src/ui/src/ioslogdialog.cpp` | 1 | `QComboBox::currentIndexChanged` |
| `src/ui/src/predefinedfilterscombobox.cpp` | 1 | `QComboBox::activated` |

**Abstraction candidate:** All `qOverload<int>(&QComboBox::currentIndexChanged)` and related patterns could be replaced by `QtCompat::connectComboBoxIndexChanged(combo, receiver, slot)` or similar helpers.

---

## 4. Feature Flags

### 4.1 `KLOGG_HAS_VECTORSCAN` — Optional Regex Engine

Defined in: `src/regex/CMakeLists.txt:28` (public, propagates to all consumers of `klogg_regex`)

| File | Lines | Impact |
|---|---|---|
| `src/regex/include/hsregularexpression.h` | 36, 75 | Entire class/namespace conditional on Vectorscan |
| `src/regex/include/regularexpression.h` | 94 | `HsBufferScanner` member in `PatternMatcher` |
| `src/regex/src/hsregularexpression.cpp` | 29 | Implementation file |
| `src/regex/src/regularexpression.cpp` | 272, 296, 308 | `scanBuffer` / `hasBufferScan` implementations |
| `src/app/main.cpp` | 142, 280 | CLI flag registration, debug output |
| `src/ui/src/optionsdialog.cpp` | 165 | Hides Vectorscan checkbox in options dialog |

### 4.2 `KLOGG_USE_MIMALLOC` — Optional Allocator

Defined in: `3rdparty/CMakeLists.txt`

| File | Lines | Impact |
|---|---|---|
| `src/app/klogg_grep.cpp` | 35 | mimalloc override include |
| `src/app/main.cpp` | 243 | mimalloc override include |
| `src/crash_handler/src/crashhandler.cpp` | 43, 301 | mimalloc stats in crash dumps |

### 4.3 `KLOGG_PORTABLE` — Portable Mode

Defined in: `src/app/CMakeLists.txt:93`

| File | Lines | Impact |
|---|---|---|
| `src/app/main.cpp` | 159 | Portable config path |
| `src/crash_handler/src/crashhandler.cpp` | 64 | Portable crash dump path |

### 4.4 `KLOGG_KARCHIVE` — Archive Support

Defined by: `cpm_cache/kf5archive/.../CMakeLists.txt:34`

| File | Lines | Impact |
|---|---|---|
| `src/ui/src/decompressor.cpp` | 182 | Archive decompression code path |

### 4.5 `KLOGG_USE_SENTRY` — Crash Reporting

Defined in: `src/crash_handler/CMakeLists.txt:25`

Used only in crash_handler layer (acceptable).

**Abstraction approach:** The Vectorscan flag is the most intrusive — it mutates the class layout in headers. Could be addressed via a strategy pattern or compile-time polymorphism behind a `RegexEngine` interface.

---

## 5. Compiler-Specific Pragmas

Three identical blocks suppressing `-Wsign-conversion` for `simdutf.h`:

| File | Lines |
|---|---|
| `src/ui/src/highlighterset.cpp` | 50–56 |
| `src/logdata/src/logdata.cpp` | 53–59 |
| `src/logdata/src/searchablelogdata.cpp` | 5–11 |

All three follow the same pattern:
```cpp
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif
#include <simdutf.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif
```

This should be centralized into a single `simdutf_wrapper.h` header.

Additionally in `src/ui/src/abstractlogview.cpp:115`:
```cpp
#pragma warning( disable : 4244 )  // MSVC only
```
This can be wrapped into a platform-intrinsics abstraction.

---

## 6. Performance Measurement Instrumentation

### 6.1 `GLOGG_PERF_MEASURE_FPS` — **DEAD CODE (Orphaned)**

| File | Lines |
|---|---|
| `src/ui/include/abstractlogview.h` | 59–61, 573–576 |
| `src/ui/src/abstractlogview.cpp` | 1243–1251 |

**No CMake option defines this macro.** This is orphaned code from upstream glogg that can never be activated. The `PerfCounter` header include and member variable are always excluded.

**Recommendation:** Remove entirely.

### 6.2 `KLOGG_PERF_MEASURE_STREAMING` — Debug Instrumentation

Controlled by CMake option (default OFF):

| File | Lines |
|---|---|
| `src/logdata/src/streaminglogdata.cpp` | 45, 57, 63, 81, 100 |
| `src/logdata/src/logfiltereddataworker.cpp` | 744, 753 |

These are timing/logging probes. Low-impact since they're off by default and in `.cpp` files.

---

## 7. Build Config Injection

From `CMakeLists.txt`:

| Definition | Scope | Notes |
|---|---|---|
| `KLOGG_PERF_MEASURE_STREAMING` | `project_options` INTERFACE | Only active when explicitly opted in |
| `KLOGG_BUILD_TYPE=$<CONFIG>` | `project_options` INTERFACE | Injected into every TU; used only in `issuereporter.cpp` for crash reports |
| `KLOGG_PORTABLE` | `klogg_portable` target PUBLIC | Only on portable build variant |
| `KLOGG_HAS_VECTORSCAN` | `klogg_regex` target PUBLIC | Propagates to all regex consumers |
| `KLOGG_KARCHIVE` | `klogg_karchive` target PUBLIC | Propagates via 3rdparty |
| `KLOGG_USE_SENTRY` | `klogg_crash_handler` target PUBLIC | Crash handler only |
| `QT_NO_KEYWORDS` | `klogg_logdata` target PUBLIC | Needed for Boost header compatibility |
| `QT_MESSAGELOGCONTEXT` | `klogg_logging` target PUBLIC | Enables file/line/function in Qt log messages |

---

## 8. Test Instrumentation in Production Code

This is a significant category — the codebase has ~50 test-only methods and 4 mutable test counters embedded in production headers. These are deliberately designed test seams, not accidental pollution, but they still contaminate the production API surface.

### 8.1 `access_by<T>` Template — Private-Access Test Hook (3 classes)

Production classes declare a public template struct that test code can specialize to reach **all** private members. This is the most powerful hook pattern.

| File | Lines | Class | Impact |
|---|---|---|---|
| `src/ui/include/abstractlogview.h` | 117–118 | `AbstractLogView` | Tests specialize `access_by<AbstractLogViewPrivate>` with dozens of static methods reaching `wrappedLinesInfo_`, `charHeight_`, `textAreaCache_`, `searchEnd_`, `selectionChanged_`, `leftMargin_`, `drawingTopOffset_`, shortcut members, etc. |
| `src/ui/include/crawlerwidget.h` | 142–143 | `CrawlerWidget` | Allows test code to reach `logMainView_`, `filteredView_`, scroll state |
| `src/logdata/include/logfiltereddataworker.h` | 224–225 | `LogFilteredDataWorker` | Allows test code to reach private search-worker internals |

### 8.2 Test-Specific Method Names (`ForTest` / `ForTesting` suffixes)

| File | Method | Purpose |
|---|---|---|
| `src/ui/include/abstractlogview.h:163` | `lineAtYForTest(int yPos)` | Coordinate-to-line resolution for headless tests |
| `src/ui/include/folderfilteredview.h:49–56` | `lineTypeForTest(LineNumber)` | Wraps protected `lineType()` so tests can verify mark-bullet rendering |
| `src/ui/include/sessioninfo.h:61–67` | `saveCountForTesting()` → `std::atomic<uint64_t>` | Counter for debounce verification |
| `src/logdata/include/encodingdetector.h:79–86` | `uchardetInvocationsForTesting()` → `std::atomic<uint64_t>` | Counter for encoding detector fallback verification |

### 8.3 "Exposed for testing" Public Accessors (AbstractLogView — ~6 methods)

| Line | Method | What it exposes |
|---|---|---|
| 147 | `searchEndLine()` | Private `searchEnd_` member |
| 148–153 | `isLineMapCurrent()` | Private `wrappedLinesInfo_` empty check |
| 153–159 | `ensureLineMapFresh()` | Forces synchronous visible-line map rebuild |
| 164–168 | `visibleLineMapBuildCount()` | Counter for map rebuild verification |
| 206–213 | `searchPattern()` | Currently-wired search pattern |
| 341–346 | `markSelected()` / `deleteMarksSelected()` | Programmatic mark toggle for tests |

### 8.4 Dedicated Test Accessor Block — FolderCrawlerWidget (~20 methods)

`src/ui/include/foldercrawlerwidget.h:100–157` contains a dedicated block headed by:

```cpp
// --- Test access / programmatic driving (no UI events needed) ---
```

Exposing: `folderResults()`, `filteredView()`, `paneCount()`, `resultsTabs()`, `mainView()`, `searchToolbar()`, `overview()`, `currentMainFilePath()`, `currentMainViewLine()`, `statusText()`, `isMainViewLineMarked()`, `isLineMarkedInFile()`, `overviewModel()`, `isFilteredResultRowMarked()`, `markMainViewLine()`, `unmarkMainViewLine()`, `isSearchActive()`, `setResultsVisibility()`, `contextLinesComboBox()`, `contextLinesSpinBox()`, `currentContext()`, `visibilityCombo()`, `viewsSplitter()`

### 8.5 Dedicated Test Accessor Block — SearchToolbar (~13 methods)

`src/ui/include/searchtoolbar.h:128–141` and `src/ui/src/searchtoolbar.cpp:481–545` expose internal Qt widgets via a `// --- QTest access ---` block: `searchLineEdit()`, `matchCaseButton()`, `inverseButton()`, `booleanButton()`, `searchButton()`, `stopButton()`, `useRegexpButton()`, `searchRefreshButton()`, `clearButton()`, `keepSearchResultsButton()`, `favoriteFilterButton()`, `predefinedFilters()`, `searchLineCompleter()`

### 8.6 Test Instrumentation Counters in Production Logic

| File | Member | Increment Site |
|---|---|---|
| `src/ui/include/abstractlogview.h:598–599` | `mutable int getSelectedTextCallCount_` | `abstractlogview.cpp:1975` inside `getSelectedText()` |
| `src/ui/include/abstractlogview.h:632–633` | `int visibleLineMapBuildCount_` | `abstractlogview.cpp:2181` inside `buildVisibleLineMap()` |
| `src/ui/src/sessioninfo.cpp:109` | `saveCountForTesting()` counter | Incremented on session write |
| `src/logdata/src/encodingdetector.cpp:117` | `uchardetInvocationsForTesting()` counter | Incremented on uchardet fallback |

### 8.7 Test-Exposed Constants

| File | Constant | Notes |
|---|---|---|
| `src/ui/include/newversiondialog.h:52–53` | `static constexpr int kMaxChangesHeight` | Promoted to `public` for test assertions |

### 8.8 Notable: No `#ifdef`, No `friend` for Tests, No `QTest::` in src/

- Zero `#ifdef KLOGG_TESTS` / `#ifndef KLOGG_TESTS` guards
- Zero `friend` declarations for test classes (all 8 `friend` declarations are architectural)
- Zero `QTest::` calls or `#include <QTest>` in `src/`
- Zero `#define` macros that change behavior for test builds

The instrumentation is entirely "always-on" — all test accessors and counters are compiled into every build, including release.

---

## 9. Non-Pollution Items (Reviewed, Found Clean) ✅

- **`friend` declarations:** All 8 are architectural (class-to-class access control), e.g., `WindowSession` ↔ `Session`, `PatternMatcher` ↔ `RegularExpression`. No test classes.
- **`static_assert`:** 3 occurrences in `linetypes.h`, all for type-safety of `LinesCount`/`LineNumber`/`LineColumn` — business logic, not platform/compiler workarounds.
- **`Q_DECLARE_METATYPE`:** ~10 standard Qt usages — required for `QVariant` integration, not pollution.
- **`Q_UNUSED`:** ~15 usages in signal/slot parameter lists — standard Qt pattern, not pollution.
- **Include guards (`#ifndef KLOGG_XXX_H`):** Standard pattern, not pollution.

---

## Priority Recommendations

| Priority | Action | Impact |
|---|---|---|
| **P0** | Remove dead `GLOGG_PERF_MEASURE_FPS` code | Cleanup, no risk |
| **P1** | Create `simdutf_wrapper.h` to eliminate 3 duplicate pragma blocks | DRY, prevents future copy-paste |
| **P1** | Create `QtCompat` namespace for Qt version abstractions | Eliminates ~30 version branches, prevents future Qt5→Qt6 mistakes |
| **P1** | Extract test accessors into `*_testapi.h` / test-only friend classes | Removes ~50 test methods + 4 counters from production headers |
| **P2** | Replace `access_by<T>` with `friend` + test-only `#ifdef` guard macro | Eliminates unrestricted private access template from public API |
| **P2** | Create `PlatformIntrinsics` / `PlatformInput` / `PlatformPaths` abstraction layers | Encapsulates ~55 platform `#ifdef` sites |
| **P2** | Replace `qOverload`/`QOverload` with `QtCompat` signal helpers | Eliminates ~22 overload disambiguations |
| **P3** | Abstract `KLOGG_HAS_VECTORSCAN` behind a `RegexEngine` interface | Reduces header-level conditional compilation |
| **P3** | Abstract `KLOGG_USE_MIMALLOC` behind an allocator hook | Removes allocator ifdefs from app code |
