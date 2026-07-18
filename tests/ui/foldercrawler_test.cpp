/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

// End-to-end integration tests for the folder-search UI: drive a real
// FolderCrawlerWidget in the Qt event loop, run an async folder search, and
// assert the grouped-results display, the main-view-opens-on-select flow, and
// collapse/expand. Runs in klogg_itests (QApplication + product-like regex
// engine, registered LineNumber/LinesCount metatypes for queued signals).

#include <catch2/catch.hpp>

#include <QBoxLayout>
#include <QByteArray>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QShortcut>
#include <QSignalSpy>
#include <QSpinBox>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

#include "abstractcrawlerwidget.h"
#include "abstractlogview.h"
#include "configuration.h"
#include "foldercrawlerwidget.h"
#include "folderfilteredview.h"
#include "foldersearchresults.h"
#include "linetypes.h"
#include "logmainview.h"
#include "overview.h"
#include "overviewwidget.h"
#include "predefinedfilters.h"
#include "predefinedfilterscombobox.h"
#include "quickfindmux.h"
#include "quickfindpattern.h"
#include "regularexpressionpattern.h"
#include "savedsearches.h"
#include "searchtoolbar.h"
#include "shortcuts.h"
#include "test_utils.h"
#include "viewinterface.h"

namespace {
QString writeFile( const QTemporaryDir& dir, const QString& name, const QByteArray& bytes )
{
    const QString path = QDir( dir.path() ).absoluteFilePath( name );
    QFile f( path );
    REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    f.write( bytes );
    f.close();
    return path;
}

// Builds a file of `totalLines` lines where each line ends with '\n'. Lines
// whose 0-based index is in `matchLines` contain "ERROR" (so a search for
// "ERROR" produces exactly those localLine values); all other lines are filler.
QString makeFile( const QTemporaryDir& dir, const QString& name, int totalLines,
                  const std::vector<int>& matchLines )
{
    QByteArray bytes;
    for ( int i = 0; i < totalLines; ++i ) {
        const bool isMatch = std::find( matchLines.begin(), matchLines.end(), i ) != matchLines.end();
        bytes.append( isMatch ? "ERROR line\n" : "padding line\n" );
    }
    return writeFile( dir, name, bytes );
}

// Pump the event loop in small steps until `predicate` is true or `timeoutMs`
// elapses. Needed because the folder search runs on a worker thread and
// delivers its results via a queued signal processed by the event loop.
bool waitFor( const std::function<bool()>& predicate, int timeoutMs = 5000 )
{
    QElapsedTimer t;
    t.start();
    while ( !predicate() ) {
        if ( t.elapsed() > timeoutMs ) {
            return false;
        }
        QTest::qWait( 50 );
    }
    return true;
}

// RAII save/restore of the global Configuration fields the folder ctor reads,
// so a test can force deterministic non-default values without leaking state
// into sibling tests (which assert e.g. a default plain-text/regex pattern).
struct ConfigGuard {
    Configuration& cfg;
    bool mainLines;
    bool filteredLines;
    bool autoRefresh;
    bool ignoreCase;
    SearchRegexpType regexpType;
    bool logicalCombining;

    ConfigGuard()
        : cfg( Configuration::getSynced() )
        , mainLines( cfg.mainLineNumbersVisible() )
        , filteredLines( cfg.filteredLineNumbersVisible() )
        , autoRefresh( cfg.isSearchAutoRefreshDefault() )
        , ignoreCase( cfg.isSearchIgnoreCaseDefault() )
        , regexpType( cfg.mainRegexpType() )
        , logicalCombining( cfg.isSearchLogicalCombiningDefault() )
    {
        // Force values the unseeded folder ctor will NOT match, so every
        // assertion below is genuinely Red before the fix.
        cfg.setMainLineNumbersVisible( true );
        cfg.setFilteredLineNumbersVisible( true );
        cfg.setSearchAutoRefreshDefault( true );
        cfg.setSearchIgnoreCaseDefault( false );
        cfg.setMainRegexpType( SearchRegexpType::ExtendedRegexp );
        cfg.setSearchLogicalCombiningDefault( true );
    }
    ~ConfigGuard()
    {
        cfg.setMainLineNumbersVisible( mainLines );
        cfg.setFilteredLineNumbersVisible( filteredLines );
        cfg.setSearchAutoRefreshDefault( autoRefresh );
        cfg.setSearchIgnoreCaseDefault( ignoreCase );
        cfg.setMainRegexpType( regexpType );
        cfg.setSearchLogicalCombiningDefault( logicalCombining );
    }
};

// RAII save/restore of the shared PredefinedFiltersCollection (a Persistable ->
// QSettings on disk). A failed REQUIRE throws, so the restore must run on stack
// unwinding; otherwise a test filter leaks into the user's real settings. The
// ctor also strips any leftover test filter from prior failed runs so this test
// (and siblings) start from a clean slate.
struct PredefinedFiltersGuard {
    PredefinedFiltersCollection::Collection saved;
    explicit PredefinedFiltersGuard( const QString& testName )
    {
        auto& c = PredefinedFiltersCollection::getSynced();
        saved = c.getSyncedFilters();
        saved.erase( std::remove_if( saved.begin(), saved.end(),
                                     [ &testName ]( const PredefinedFilter& f ) {
                                         return f.name == testName;
                                     } ),
                     saved.end() );
        c.saveToStorage( saved ); // clean slate for construction
    }
    ~PredefinedFiltersGuard()
    {
        PredefinedFiltersCollection::getSynced().saveToStorage( saved );
    }
};
} // namespace

// Distinct access tag: both crawlerwidget_test.cpp and foldercrawler_test.cpp
// link into the same klogg_itests binary, so an explicit specialization of
// AbstractLogView::access_by MUST use a different tag in each translation unit
// (two definitions of access_by<AbstractLogViewPrivate> would be an ODR clash).
struct FolderViewTestAccess {
};

template <>
struct AbstractLogView::access_by<FolderViewTestAccess> {
    static bool selectionChanged( const AbstractLogView* view )
    {
        return view->selectionChanged_;
    }

    static int charHeight( const AbstractLogView* view )
    {
        return view->charHeight_;
    }

    static int bulletZoneWidth( const AbstractLogView* view )
    {
        return view->bulletZoneWidthPx_;
    }

    static int drawingTopOffset( const AbstractLogView* view )
    {
        return view->drawingTopOffset_;
    }

    static const std::vector<AbstractLogView::QuickHighlighters>&
    quickHighlighters( const AbstractLogView* view )
    {
        return view->quickHighlighters_;
    }

    static QMenu* popupMenu( const AbstractLogView* view )
    {
        return view->popupMenu_;
    }

    static bool lineNumbersVisible( const AbstractLogView* view )
    {
        return view->lineNumbersVisible_;
    }
};

TEST_CASE( "FolderCrawlerWidget plain click on a result row repaints the selection highlight",
           "[folder][selection]" )
{
    // AbstractLogView::mousePressEvent's plain-click branch sets selection state
    // + emits newSelection but historically did NOT call update() (unlike the
    // Shift-click branch at abstractlogview.cpp:513). Single-file CrawlerWidget
    // compensates with a host-side connect(FilteredView::newSelection ->
    // view->update()) (crawlerwidget.cpp:1543); FolderCrawlerWidget does NOT.
    // So a plain click on a folder result row changed selection_ state but never
    // scheduled a repaint of the filtered view, so no highlight appeared.
    // selectionChanged_ (set true in mousePressEvent, cleared only in paintEvent)
    // is the discriminator: 'false' after the click proves a repaint ran.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR a\nline2\nERROR b\nline4\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    // rows: [H0, D1(localLine 1 "ERROR a"), D2(localLine 3 "ERROR b")]

    // Open a.log up front so the later real click takes the synchronous same-file
    // branch of openFileInMainView (file cached): no async load, and the open
    // path touches mainView_ only, never repainting the filtered view.
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    auto* const view = widget.filteredView();
    REQUIRE( view != nullptr );

    // Give the filtered view focus BEFORE the click so the click produces no
    // focus-transfer repaint -- the incidental repaint that, in the real app,
    // masked the bug on the first click. With focus already held, the ONLY thing
    // that can repaint the view is an explicit update().
    view->setFocus();
    QTest::qWait( 100 );
    // Baseline: pump pending paints so the cache is clean before the click.
    REQUIRE_FALSE(
        AbstractLogView::access_by<FolderViewTestAccess>::selectionChanged( view ) );

    // Drive a REAL plain left-click on data row D2 (center of visible row index 2)
    // at an x safely past the bullet/mark zone so mousePressEvent takes the SELECT
    // branch, not the mark branch.
    using Access = AbstractLogView::access_by<FolderViewTestAccess>;
    const int charH = Access::charHeight( view );
    REQUIRE( charH > 0 );
    const int top = Access::drawingTopOffset( view );
    const int bullet = Access::bulletZoneWidth( view );
    const int xPos = bullet + ( view->viewport()->width() - bullet ) / 2;
    const int yPos = top + charH * 2 + charH / 2; // center of row index 2
    QTest::mouseClick( view->viewport(), Qt::LeftButton, {}, QPoint( xPos, yPos ) );
    QTest::qWait( 100 ); // process the queued update() if the fix scheduled one

    // selectionChanged_ is set true in mousePressEvent and cleared ONLY inside
    // paintEvent. 'false' here proves a repaint ran and rebuilt the highlight
    // pixmap from the freshly-selected line. RED before the base-class fix: no
    // update() is scheduled, no paint runs, the flag stays true.
    REQUIRE_FALSE( Access::selectionChanged( view ) );
}

TEST_CASE( "FolderCrawlerWidget places the Marks-and-matches combo leftmost in the toolbar",
           "[folder][parity]" )
{
    // The visibility filter combo (item 0 == "Marks and matches") is the literal
    // control the user sees. Single-file adds it FIRST in the results toolbar
    // (crawlerwidget.cpp:1287), left of the search bar; folder must match so the
    // control sits in the same place on both tab kinds. RED today: folder adds
    // searchToolbar_ + collapse/expand BEFORE the combo (foldercrawlerwidget.cpp
    // :221-225), so it is the 4th control, not the leftmost.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    QComboBox* visibilityBox = nullptr;
    const auto target = QStringLiteral( "Marks and matches" );
    for ( auto* box : widget.findChildren<QComboBox*>() ) {
        if ( box->count() > 0 && box->itemText( 0 ) == target ) {
            visibilityBox = box;
            break;
        }
    }
    REQUIRE( visibilityBox != nullptr );

    // Locate the toolbar sub-layout (a QHBoxLayout nested in the bottom window's
    // QVBoxLayout) that owns the combo, then assert the combo sits at index 0.
    auto toolbarIndexOf = []( QComboBox* child ) -> int {
        auto* parent = child->parentWidget();
        if ( parent == nullptr ) {
            return -1;
        }
        auto* outer = qobject_cast<QBoxLayout*>( parent->layout() );
        if ( outer == nullptr ) {
            return -1;
        }
        for ( int i = 0; i < outer->count(); ++i ) {
            auto* sub = outer->itemAt( i )->layout();
            if ( sub != nullptr && sub->indexOf( child ) != -1 ) {
                return sub->indexOf( child );
            }
        }
        return -1;
    };
    REQUIRE( toolbarIndexOf( visibilityBox ) == 0 );
}

TEST_CASE( "FolderCrawlerWidget search populates grouped results", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nnope\nERROR two\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR three\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.searchFor( "ERROR" );

    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    // a: header + 2 matches = 3 rows; b: header + 1 match = 2 rows; total 5.
    REQUIRE( widget.folderResults()->getNbLine() == 5_lcount );
    REQUIRE( widget.folderResults()->groupCount() == 2 );
    REQUIRE( widget.folderResults()->lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( widget.folderResults()->lineKind( 1_lnum ) == LineKind::Data );
    // Main view is empty until a row is selected.
    REQUIRE( widget.currentMainFilePath().isEmpty() );
}

TEST_CASE( "FolderCrawlerWidget selecting a result opens the source file", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "line0\nERROR here\nline2\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.searchFor( "ERROR" );

    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    // Visible rows: [header(0), match(1)].
    REQUIRE( widget.folderResults()->lineKind( 1_lnum ) == LineKind::Data );

    widget.selectResultRow( 1_lnum ); // opens a.log at the matched line

    REQUIRE( waitFor( [ & ]() { return !widget.currentMainFilePath().isEmpty(); } ) );
    REQUIRE( widget.currentMainFilePath() == a );
}

TEST_CASE( "FolderCrawlerWidget main view mark-navigation is safe without filtered data",
           "[folder]" )
{
    // LogMainView registers LogViewNextMark/LogViewPrevMark shortcuts whose
    // handlers used to dereference filteredData_, which is null in folder mode
    // (useNewFiltering is never called). Driving the same logic directly must
    // be a no-op, not a null-deref crash.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    REQUIRE( widget.mainView() != nullptr );
    REQUIRE_NOTHROW( widget.mainView()->selectNextMark() );
    REQUIRE_NOTHROW( widget.mainView()->selectPrevMark() );
}

TEST_CASE( "FolderCrawlerWidget main view search range spans the opened file",
           "[folder]" )
{
    // setDataSource must reset the search range to span the new document;
    // otherwise every body line renders as "out of search range" (gray), because
    // the view was constructed on an empty placeholder whose range ended at 0.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "line0\nERROR here\nline2\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    // No file open yet: placeholder (0 lines) -> search range end is 0.
    REQUIRE( widget.mainView()->searchEndLine() == 0_lnum );

    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    widget.selectResultRow( 1_lnum ); // opens a.log (3 lines) in the main view

    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 ); // settle: setDataSource runs in the loadingFinished queue

    // The whole 3-line file is in range -> body text is not grayed.
    REQUIRE( widget.mainView()->searchEndLine() == 3_lnum );
}

TEST_CASE( "FolderCrawlerWidget main view line map refreshes on demand after a data swap",
           "[folder]" )
{
    // A cached re-select runs setDataSource synchronously and clears the main
    // view's visible-line map; the map is only rebuilt on the next (async) paint.
    // A click delivered before that paint used to be swallowed
    // (convertCoordToLine -> nullopt) or resolve to a stale row (in the filtered
    // view, a Data row that shifted onto a Header index toggled collapse). The
    // mouse handlers now force a synchronous refresh (ensureLineMapFresh) before
    // converting coordinates.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "line0\nERROR here\nline2\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR in b\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    // Shown + sized so viewport()->repaint() (used by ensureLineMapFresh) actually
    // issues a paintEvent -- Qt skips painting hidden widgets.
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    // Open A then B (both async-indexed and cached), settling so maps rebuild.
    widget.selectResultRow( 1_lnum ); // a.log match -> row 1
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );
    widget.selectResultRow( 3_lnum ); // b.log match -> row 3
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == b; } ) );
    QTest::qWait( 200 );

    // Cached re-select of A: setDataSource runs synchronously and leaves the map
    // stale/empty (no paint is processed before selectResultRow returns).
    widget.selectResultRow( 1_lnum );
    REQUIRE( widget.currentMainFilePath() == a );
    REQUIRE_FALSE( widget.mainView()->isLineMapCurrent() );

    // The on-demand refresh the mouse handlers use rebuilds the map synchronously.
    widget.mainView()->ensureLineMapFresh();
    REQUIRE( widget.mainView()->isLineMapCurrent() );
}

TEST_CASE( "FolderCrawlerWidget main view caches the line map across unchanged mouse events",
           "[folder]" )
{
    // ensureLineMapFresh runs on every press/move/release. In wrap mode each call
    // used to re-read and re-wrap every line from the viewport to EOF. When two
    // consecutive mouse events share the same data/geometry (the common case --
    // the user is just moving the mouse), the rebuild is pure waste. The signature
    // cache must skip it, and must still rebuild when an input actually changes
    // (here: a data swap via setDataSource).
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "line0\nERROR here\nline2\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR in b\nmore\nmore2\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    widget.selectResultRow( 1_lnum ); // open a.log
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    auto* const mainView = widget.mainView();

    // First refresh builds the map.
    mainView->ensureLineMapFresh();
    REQUIRE( mainView->isLineMapCurrent() );
    const int buildsAfterFirst = mainView->visibleLineMapBuildCount();

    // A second refresh with no data/geometry change must be a cache hit: it must
    // NOT re-enter the paint-free rebuild. (RED before the cache: count rises.)
    mainView->ensureLineMapFresh();
    REQUIRE( mainView->visibleLineMapBuildCount() == buildsAfterFirst );

    // Swapping the underlying file changes the map's inputs (data ptr + line
    // count), so the cache must invalidate and the next refresh must rebuild.
    widget.selectResultRow( 3_lnum ); // open b.log
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == b; } ) );
    QTest::qWait( 200 );

    mainView->ensureLineMapFresh();
    REQUIRE( mainView->visibleLineMapBuildCount() > buildsAfterFirst );

    // ...and a follow-up refresh on the new file is again a cache hit.
    const int buildsAfterSwap = mainView->visibleLineMapBuildCount();
    mainView->ensureLineMapFresh();
    REQUIRE( mainView->visibleLineMapBuildCount() == buildsAfterSwap );
}

TEST_CASE( "FolderCrawlerWidget search toolbar shares the results pane (sits between the views)",
           "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    // The toolbar lives in the SAME pane as the results view (the composite
    // "bottom window"), in a DIFFERENT pane from the main view -> it renders
    // between the two views, matching single-file tabs (whose toolbar row sits
    // above the results, inside the splitter's bottom pane). The results live in
    // a QTabWidget (resultsTabs) that shares the toolbar's bottom pane.
    REQUIRE( widget.searchToolbar()->parentWidget() == widget.resultsTabs()->parentWidget() );
    REQUIRE( widget.mainView()->parentWidget() != widget.searchToolbar()->parentWidget() );
}

TEST_CASE( "FolderCrawlerWidget toolbar status never leaks the opened file path", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "line0\nERROR here\nline2\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    // setFolder surfaces the file count, not the folder path.
    REQUIRE_FALSE( widget.statusText().contains( dir.path() ) );

    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    widget.selectResultRow( 1_lnum ); // async load of a.log
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );
    // No "Opening <path>" leak while/after loading the file.
    REQUIRE_FALSE( widget.statusText().startsWith( QStringLiteral( "Opening" ) ) );
    REQUIRE_FALSE( widget.statusText().contains( a ) );
}

TEST_CASE( "FolderCrawlerWidget exposes main-view file info for the status bar", "[folder]" )
{
    // currentMainViewInfo() is the data path MainWindow's info line consumes for
    // folder tabs: path / size / modified-date / encoding / line-count of the
    // file currently in the main view, or nullopt (-> folder path) when none.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\npad\npad\n" ) ); // 3 lines
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR two\n" ) );           // 1 line

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    // No file open yet -> nullopt (MainWindow falls back to the folder path).
    REQUIRE_FALSE( widget.currentMainViewInfo().has_value() );

    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    // Visible rows: [H0(a), D1(a match), H2(b), D3(b match)].
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    const auto infoA = widget.currentMainViewInfo();
    REQUIRE( infoA.has_value() );
    REQUIRE( infoA->path == a );
    REQUIRE( infoA->nbLines == 3 );
    REQUIRE( infoA->size > 0 );
    REQUIRE_FALSE( infoA->encodingText.isEmpty() );

    widget.selectResultRow( 3_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == b; } ) );
    QTest::qWait( 200 );
    const auto infoB = widget.currentMainViewInfo();
    REQUIRE( infoB.has_value() );
    REQUIRE( infoB->path == b );
    REQUIRE( infoB->nbLines == 1 );
}

TEST_CASE( "FolderCrawlerWidget main-view marks are per-file and survive swaps", "[folder]" )
{
    // Folder mode has no LogFilteredData, so the widget owns a per-file mark
    // store and injects it into the main view via MarkProvider. The M shortcut
    // (markSelected -> markLines) and margin click toggle marks; marks render
    // (LineTypeFlags::Mark) and are kept per file path so they survive opening
    // other result files and returning.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\npad\npad\n" ) ); // 3 lines
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR two\n" ) );           // 1 line

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    // Open A (match at localLine 0).
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    // Mark line 1 in the current (A) file. markMainViewLine is the programmatic
    // equivalent of the M shortcut / margin click (which route through the
    // markLines signal to the same addMark path).
    widget.markMainViewLine( 1_lnum );
    REQUIRE( widget.isMainViewLineMarked( 1_lnum ) );

    // Remove it.
    widget.unmarkMainViewLine( 1_lnum );
    REQUIRE_FALSE( widget.isMainViewLineMarked( 1_lnum ) );

    // Re-mark, then switch to file B: the mark must NOT show in B (per-file)...
    widget.markMainViewLine( 1_lnum );
    REQUIRE( widget.isMainViewLineMarked( 1_lnum ) );

    widget.selectResultRow( 3_lnum ); // b.log match
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == b; } ) );
    QTest::qWait( 200 );
    REQUIRE_FALSE( widget.isMainViewLineMarked( 1_lnum ) );

    // ...and must reappear when A is reopened (cached swap).
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );
    REQUIRE( widget.isMainViewLineMarked( 1_lnum ) );
}

TEST_CASE( "FolderCrawlerWidget exposes predefined filters and favorites in the toolbar", "[folder]" )
{
    // Predefined filters + favorites are shared global pattern stores; they are
    // shown (not hidden) so folder search can reuse / save filters like
    // single-file (selecting a predefined filter applies its pattern + regex).
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    REQUIRE_FALSE( widget.searchToolbar()->predefinedFilters()->isHidden() );
    REQUIRE_FALSE( widget.searchToolbar()->favoriteFilterButton()->isHidden() );
    // Auto-refresh is file-search-only (folder search has no file watching) and
    // stays hidden; keep-results is now VISIBLE (folder search supports keeping
    // results across searches in separate tabs).
    REQUIRE( widget.searchToolbar()->searchRefreshButton()->isHidden() );
    REQUIRE_FALSE( widget.searchToolbar()->keepSearchResultsButton()->isHidden() );
}

TEST_CASE( "FolderCrawlerWidget reselecting the same file reuses it without reload churn",
           "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nERROR two\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    widget.selectResultRow( 1_lnum ); // first select -> async load + swap
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );

    // Selecting another row in the SAME file must keep the same file loaded.
    widget.selectResultRow( 2_lnum );
    QTest::qWait( 100 );
    REQUIRE( widget.currentMainFilePath() == a );
}

TEST_CASE( "FolderCrawlerWidget collapse-all and expand-all change visible rows", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nERROR two\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR three\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    const auto expanded = widget.folderResults()->getNbLine(); // 2 headers + 3 matches = 5
    REQUIRE( expanded == 5_lcount );

    widget.collapseAll();
    QTest::qWait( 100 );
    REQUIRE( widget.folderResults()->getNbLine() == 2_lcount ); // only the 2 headers
    REQUIRE( widget.folderResults()->lineKind( 0_lnum ) == LineKind::Header );
    REQUIRE( widget.folderResults()->lineKind( 1_lnum ) == LineKind::Header );

    widget.expandAll();
    QTest::qWait( 100 );
    REQUIRE( widget.folderResults()->getNbLine() == expanded );
}

TEST_CASE( "FolderCrawlerWidget header-click toggles a single group", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nERROR two\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR three\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    REQUIRE_FALSE( widget.folderResults()->isCollapsed( 0 ) );

    widget.clickHeaderRow( 0_lnum ); // toggle group a
    QTest::qWait( 100 );
    REQUIRE( widget.folderResults()->isCollapsed( 0 ) );
    // a collapsed: a-header(1) + b(header+1) = 1 + 2 = 3 visible rows.
    REQUIRE( widget.folderResults()->getNbLine() == 3_lcount );
    // The header text still reports the group's total match count.
    const auto headerText = widget.folderResults()->getLineString( 0_lnum );
    REQUIRE( headerText.contains( "2" ) );

    widget.clickHeaderRow( 0_lnum ); // expand again
    QTest::qWait( 100 );
    REQUIRE_FALSE( widget.folderResults()->isCollapsed( 0 ) );
}

TEST_CASE( "FolderCrawlerWidget search with no matches leaves results empty", "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "nothing here\nstill nothing\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.searchFor( "ERROR" );

    // searchFinished fires (with 0 matches) but getNbLine stays 0. Wait for the
    // search to complete before asserting so streaming timing is not a factor.
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    REQUIRE( widget.folderResults()->getNbLine() == 0_lcount );
    REQUIRE( widget.currentMainFilePath().isEmpty() );
}

TEST_CASE( "FolderCrawlerWidget forwards search pattern to filtered view for match highlighting",
           "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nnope\nERROR two\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR three\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.show(); // realize the widget tree so the filtered view can paint headlessly
    widget.searchFor( "ERROR" );

    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    // a: header + 2 matches = 3 rows; b: header + 1 match = 2 rows; total 5.
    REQUIRE( widget.folderResults()->getNbLine() == 5_lcount );

    // The folder search pattern must be forwarded to the filtered view so the
    // paint-time highlight path (AbstractLogView::drawTextArea, which rebuilds
    // patternHighlight from searchPattern_ on every repaint) highlights the
    // matched substring inside each result row, exactly like single-file
    // FilteredView. This is the wiring assertion; the highlight itself is a
    // paint-time pixel effect that cannot be asserted headlessly.
    REQUIRE( widget.filteredView() != nullptr );
    REQUIRE( widget.filteredView()->searchPattern().pattern == QStringLiteral( "ERROR" ) );

    // Force a headless repaint of the filtered view's viewport to prove the
    // paint path (which re-applies the search regex to every visible line) runs
    // to completion with searchPattern_ set, without throwing.
    REQUIRE_NOTHROW( widget.filteredView()->viewport()->repaint() );
    QTest::qWait( 200 ); // settle per CLAUDE.md after a paint-triggering action
}

TEST_CASE( "FolderCrawlerWidget forwards search pattern to main view for opened-file highlighting",
           "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nnope\nERROR two\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.show(); // realize the widget tree so the main view can paint headlessly
    // Drive explicit toggles so the assertion is independent of Configuration
    // search defaults (P0 now seeds those into the folder toolbar, so the
    // default useRegexp follows mainRegexpType rather than being unchecked).
    widget.searchToolbar()->setUseRegexp( false );
    widget.searchToolbar()->setMatchCase( false );
    widget.searchFor( "ERROR" );

    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    // PRE-open wiring: startSearch must forward the pattern to mainView_ (parity
    // with the filteredView assertion above). With useRegexp off + matchCase off
    // the pattern is a case-insensitive plain-text one.
    REQUIRE( widget.mainView() != nullptr );
    REQUIRE( widget.mainView()->searchPattern()
             == RegularExpressionPattern( "ERROR", /*caseSensitive=*/false, /*inverse=*/false,
                                          /*boolean=*/false, /*plainText=*/true ) );

    // Open a result row -> async load + setDataSource swap of mainView_.
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 ); // settle per CLAUDE.md

    // POST-open wiring: the pattern survived the setDataSource swap (re-applied
    // in openFileInMainView), so the opened file highlights its matches at the
    // next paint.
    REQUIRE( widget.mainView()->searchPattern().pattern == QStringLiteral( "ERROR" ) );
    REQUIRE_NOTHROW( widget.mainView()->viewport()->repaint() );
    QTest::qWait( 200 );
}

TEST_CASE( "FolderCrawlerWidget view context round-trips pattern and option toggles",
           "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nERROR two\n" ) );

    FolderCrawlerWidget source;
    source.setFolder( dir.path(), QStringList{ a } );
    // Drive a known pattern + non-default toggles via the shared toolbar.
    source.searchToolbar()->setUseRegexp( true );
    source.searchToolbar()->setInverse( true );
    source.searchFor( "WARNING" );
    REQUIRE( waitFor( [ & ]() { return !source.isSearchActive(); } ) );

    // doGetViewContext must return a non-null context (a null one crashes the
    // session save path via view_context->toString()).
    const auto context = source.context();
    REQUIRE( context != nullptr );
    const auto json = context->toString();
    REQUIRE( json.contains( "WARNING" ) );

    // Restore into a fresh widget via doSetViewContext (public setViewContext).
    FolderCrawlerWidget restored;
    restored.setFolder( dir.path(), QStringList{ a } );
    restored.setViewContext( json );

    REQUIRE( restored.searchToolbar()->currentSearchText() == QStringLiteral( "WARNING" ) );
    REQUIRE( restored.searchToolbar()->isUseRegexp() );
    REQUIRE( restored.searchToolbar()->isInverse() );
    // No auto-run kicked off by setViewContext.
    REQUIRE_FALSE( restored.isSearchActive() );
}

TEST_CASE( "FolderCrawlerWidget setViewContext prefills pattern without auto-running search",
           "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "PREFILLED one\n" ) );

    // Build a context string carrying a pattern from a source widget.
    FolderCrawlerWidget source;
    source.setFolder( dir.path(), QStringList{ a } );
    source.searchFor( "PREFILLED" );
    REQUIRE( waitFor( [ & ]() { return !source.isSearchActive(); } ) );
    const auto json = source.context()->toString();

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.setViewContext( json );

    // Pattern is prefilled into the toolbar...
    REQUIRE( widget.searchToolbar()->currentSearchText() == QStringLiteral( "PREFILLED" ) );
    // ...but NO search was started by setViewContext (one Enter re-runs).
    REQUIRE_FALSE( widget.isSearchActive() );

    // One Enter (searchFor) re-runs to completion.
    widget.searchFor( "PREFILLED" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
}

TEST_CASE( "FolderCrawlerWidget overview reflects the opened file and swaps on reselect",
           "[folder]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // Two files with DIFFERENT match sets so we can prove the overview repoints:
    //  - a.log: matches at localLine {0, 100}  -> 2 matches
    //  - b.log: matches at localLine {0, 50, 100} -> 3 matches
    const QString a = makeFile( dir, "a.log", 200, { 0, 100 } );
    const QString b = makeFile( dir, "b.log", 200, { 0, 50, 100 } );

    FolderCrawlerWidget widget;
    widget.resize( 800, 600 );
    widget.show(); // realize the widget tree so the overview widget can layout
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    // Visible rows: a(header 0, match 1, match 2), b(header 3, match 4, match 5,
    // match 6) = 7 rows.
    REQUIRE( widget.folderResults()->getNbLine() == 7_lcount );

    // The overview follows Configuration (default visible).
    REQUIRE( widget.overview()->isVisible() );

    // Helper to drive the Overview recompute deterministically. paintEvent would
    // call updateView(height) lazily; here we force it so the assertion does not
    // depend on headless widget sizing. A large height keeps every match on its
    // own y position (no collapse), so #weighted lines == #matches in the file.
    auto overviewMatchCount = []( FolderCrawlerWidget& w ) -> size_t {
        Overview* ov = w.overview();
        REQUIRE( ov != nullptr );
        ov->updateView( 10000 );
        return ov->getMatchLines()->size();
    };

    // --- Open file A (async first load) ---
    widget.selectResultRow( 1_lnum ); // a's first match (localLine 0)
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 ); // settle per CLAUDE.md

    // refreshOverview must have shown the overview widget alongside the viewport.
    auto* overviewWidget = widget.mainView()->findChild<OverviewWidget*>();
    REQUIRE( overviewWidget != nullptr );
    REQUIRE( overviewWidget->isVisible() );

    // The overview now reflects a.log's matches (2), not b.log's (3), and no marks.
    REQUIRE( overviewMatchCount( widget ) == 2 );
    REQUIRE( widget.overview()->getMarkLines()->empty() );

    // --- Open file B (different match set -> cached/async swap repoints) ---
    widget.selectResultRow( 4_lnum ); // b's first match (localLine 0)
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == b; } ) );
    QTest::qWait( 200 ); // settle

    // The overview must now reflect b.log's matches (3): the repoint happened.
    REQUIRE( overviewMatchCount( widget ) == 3 );

    // --- Switch back to A (now served from the cache) -> overview repoints again.
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );
    REQUIRE( overviewMatchCount( widget ) == 2 );
}

TEST_CASE( "FolderCrawlerWidget overview click emits lineClicked for jump parity",
           "[folder]" )
{
    // Proves the single-file click-to-jump wiring needs no new code in folder
    // mode: OverviewWidget::lineClicked is already connected to jumpToLine in
    // AbstractLogView::setOverview, so emitting it must reach the main view.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = makeFile( dir, "a.log", 200, { 0, 100 } );

    FolderCrawlerWidget widget;
    widget.resize( 800, 600 );
    widget.show();
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    const auto firstVisibleBefore = widget.mainView()->getTopLine();

    // Emit the overview click signal the same way a real mouse press does
    // (AbstractLogView connects OverviewWidget::lineClicked -> jumpToLine).
    auto* overviewWidget = widget.mainView()->findChild<OverviewWidget*>();
    REQUIRE( overviewWidget != nullptr );
    Q_EMIT overviewWidget->lineClicked( 150_lnum );
    QTest::qWait( 200 );

    // jumpToLine centers the target line, so the new top line is the clicked
    // line minus half a viewport (NOT exactly 150). What we assert is that the
    // click reached jumpToLine: the first visible line CHANGED and moved down
    // toward the clicked line (which started near 0 after opening at localLine 0).
    const auto topAfter = widget.mainView()->getTopLine();
    REQUIRE( topAfter != firstVisibleBefore );
    REQUIRE( topAfter > firstVisibleBefore ); // scrolled down toward 150
    REQUIRE( topAfter <= 150_lnum );          // centering keeps top at/above the target
}

TEST_CASE( "FolderCrawlerWidget seeds view config and search toggles from Configuration",
           "[folder]" )
{
    // The folder ctor must seed line-number visibility on BOTH views and seed
    // the toolbar toggles from Configuration -- mirroring CrawlerWidget
    // (crawlerwidget.cpp:842,852,1311-1314). Without it, the filtered view
    // (whose config default is line-numbers-ON) showed no gutter, and a fresh
    // folder tab opened with all toggles unchecked regardless of Configuration.
    ConfigGuard guard;
    auto& config = Configuration::get();

    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    REQUIRE( widget.mainView()->isLineNumbersVisible() == config.mainLineNumbersVisible() );
    REQUIRE( widget.filteredView()->isLineNumbersVisible()
             == config.filteredLineNumbersVisible() );

    REQUIRE( widget.searchToolbar()->isAutoRefresh() == config.isSearchAutoRefreshDefault() );
    REQUIRE( widget.searchToolbar()->isMatchCase() == !config.isSearchIgnoreCaseDefault() );
    REQUIRE( widget.searchToolbar()->isUseRegexp()
             == ( config.mainRegexpType() == SearchRegexpType::ExtendedRegexp ) );
    REQUIRE( widget.searchToolbar()->isBoolean() == config.isSearchLogicalCombiningDefault() );
}

TEST_CASE( "FolderCrawlerWidget is an AbstractCrawlerWidget (polymorphic dispatch target)",
           "[folder]" )
{
    // MainWindow obtains a single AbstractCrawlerWidget* per tab (via
    // dynamic_cast from QWidget*) and dispatches applyConfiguration /
    // registerShortcuts through it, instead of qobject_cast-ing to each concrete
    // type at every site -- the cast-branch pattern that caused the original
    // folder-tab crash (a static_cast<CrawlerWidget*> on a FolderCrawlerWidget).
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    // The folder tab must be reachable as the common dispatch base.
    auto* base = dynamic_cast<AbstractCrawlerWidget*>( &widget );
    REQUIRE( base != nullptr );

    // The default hooks are no-ops for the folder widget today (P1 overrides
    // them); calling them through the base must not throw or crash.
    REQUIRE_NOTHROW( base->applyConfiguration() );
    REQUIRE_NOTHROW( base->registerShortcuts() );

    // It is still a ViewInterface (Session stores ViewInterface*).
    REQUIRE( dynamic_cast<const ViewInterface*>( &widget ) != nullptr );
}

TEST_CASE( "FolderCrawlerWidget applyConfiguration re-applies view config on demand",
           "[folder]" )
{
    // applyConfiguration() (mirrors CrawlerWidget::applyConfiguration) must
    // re-read Configuration and re-apply line-number visibility / font /
    // overview to both views whenever MainWindow emits optionsChanged (the
    // View-menu toggles). Without the override, the inherited no-op leaves the
    // views stuck at their ctor-seeded state, so toggling line numbers from the
    // menu has no effect on a folder tab.
    ConfigGuard guard;
    auto& config = Configuration::get();

    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    // Flip config AFTER construction so the ctor's initial seeding is not what
    // is under test -- applyConfiguration() must re-read Configuration itself.
    config.setMainLineNumbersVisible( false );
    config.setFilteredLineNumbersVisible( false );
    widget.applyConfiguration();
    REQUIRE_FALSE( widget.mainView()->isLineNumbersVisible() );
    REQUIRE_FALSE( widget.filteredView()->isLineNumbersVisible() );

    config.setMainLineNumbersVisible( true );
    config.setFilteredLineNumbersVisible( true );
    widget.applyConfiguration();
    REQUIRE( widget.mainView()->isLineNumbersVisible() );
    REQUIRE( widget.filteredView()->isLineNumbersVisible() );

    // registerShortcuts() registers the views' keyboard navigation; it must be
    // callable and not throw (called by applyConfiguration and the ctor).
    REQUIRE_NOTHROW( widget.registerShortcuts() );
}

TEST_CASE( "FolderCrawlerWidget copy/selectAll delegate to the active view", "[folder]" )
{
    // FolderCrawlerWidget must override getSelectedText/selectAll/isPartialSelection
    // (AbstractCrawlerWidget) so MainWindow::copy/selectAll work on folder tabs.
    // Previously they gated on the current crawler widget (a CrawlerWidget cast,
    // null for a folder tab) and the Edit-menu items were enabled but silent
    // no-ops. Through the AbstractCrawlerWidget* dispatch base, copy/selectAll
    // must reach the folder's active view.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nnope\nERROR two\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.show();
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    auto* base = dynamic_cast<AbstractCrawlerWidget*>( &widget );
    REQUIRE( base != nullptr );

    // Drive the active (filtered) view: selectAll selects every visible line,
    // getSelectedText then returns its text.
    widget.filteredView()->setFocus();
    REQUIRE_NOTHROW( base->selectAll() );
    QTest::qWait( 50 );
    const auto text = base->getSelectedText();
    REQUIRE_FALSE( text.isEmpty() );
    REQUIRE( text.contains( QStringLiteral( "ERROR" ) ) );
    REQUIRE_NOTHROW( base->isPartialSelection() );
}

TEST_CASE( "FolderCrawlerWidget is a QuickFind selector (Ctrl+F dispatch target)", "[folder]" )
{
    // FolderCrawlerWidget must implement QuickFindMuxSelectorInterface so the
    // main-window Ctrl+F bar dispatches searchForward/searchBackward to the
    // folder's views. Previously the folder tab registered a null selector and
    // the QuickFind bar was inert.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nnope\nERROR two\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    auto* selector = dynamic_cast<QuickFindMuxSelectorInterface*>( &widget );
    REQUIRE( selector != nullptr );
    REQUIRE( selector->getActiveSearchable() != nullptr );
    const auto searchables = selector->getAllSearchables();
    REQUIRE( searchables.size() == 2 ); // main view + filtered view
}

TEST_CASE( "FolderCrawlerWidget rebinds its views to the session QuickFindPattern", "[folder]" )
{
    // doSetQuickFindPattern must re-point both views to the passed pattern via
    // AbstractLogView::setQuickFindPattern, so the app-wide QuickFindMux (which
    // drives the session pattern) actually drives the folder's views. Without
    // this, the views keep the ctor-local pattern the mux never updates and
    // typing in the QuickFind bar does nothing.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nnope\nERROR two\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    // Before rebind, the views hold the ctor-local pattern (not the session one).
    auto sessionQfp = std::make_shared<QuickFindPattern>();
    REQUIRE( widget.filteredView()->quickFindPattern() != sessionQfp.get() );

    widget.setQuickFindPattern( sessionQfp ); // doSetQuickFindPattern -> views re-pointed

    REQUIRE( widget.filteredView()->quickFindPattern() == sessionQfp.get() );
    REQUIRE( widget.mainView()->quickFindPattern() == sessionQfp.get() );
}

TEST_CASE( "FolderCrawlerWidget setEncoding applies to the opened file", "[folder]" )
{
    // setEncoding (Edit -> Encoding) must route to the folder and apply to the
    // file currently shown in the main view. No-op (safe) before a file is
    // opened; applies without throwing once one is.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nnope\nERROR two\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    // No file opened yet -> safe no-op (currentMainData_ is the placeholder).
    REQUIRE_NOTHROW( widget.setEncoding( 106 ) ); // UTF-8 MIB

    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return !widget.currentMainFilePath().isEmpty(); } ) );

    // File opened -> applies (re-displays the opened file); must not throw for
    // both an explicit MIB and the detected encoding.
    REQUIRE_NOTHROW( widget.setEncoding( 106 ) );          // UTF-8
    REQUIRE_NOTHROW( widget.setEncoding( std::nullopt ) ); // detected
}

TEST_CASE( "FolderCrawlerWidget records folder searches into the shared history", "[folder]" )
{
    // doSetSavedSearches must wire the session-wide SavedSearches into the
    // toolbar, and startSearch must record the pattern into it -- parity with
    // single-file search (recent grep patterns appear in the dropdown).
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\n" ) );

    SavedSearches ss;
    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.setSavedSearches( &ss ); // doSetSavedSearches wires history

    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    // The pattern was recorded into the shared history AND surfaced in the
    // toolbar dropdown.
    REQUIRE( ss.recentSearches().contains( QStringLiteral( "ERROR" ) ) );
    REQUIRE( widget.searchToolbar()->searchLineEdit()->findText( QStringLiteral( "ERROR" ) )
             >= 0 );
}

TEST_CASE( "FolderCrawlerWidget results visibility: Marks shows only marked rows",
           "[folder]" )
{
    // The Marks/Marks-and-matches/Matches combo must work on folder results
    // (parity with single-file). Since every folder Data row is a match,
    // MarksAndMatches and Matches both show all rows; Marks shows only the
    // marked rows (plus a header for each group that has a mark).
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "ERROR one\nERROR two\nERROR three\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    // rows: [H0, D1, D2, D3] = 4 rows.
    REQUIRE( widget.folderResults()->getNbLine() == 4_lcount );

    // Mark result row 2 (a.log localLine 1) only.
    widget.filteredView()->selectAndDisplayLine( 2_lnum );
    widget.filteredView()->markSelected();
    QTest::qWait( 50 );
    REQUIRE( widget.isFilteredResultRowMarked( 2_lnum ) );

    // Marks view: header + the single marked row.
    widget.setResultsVisibility( FolderSearchResults::Visibility::Marks );
    QTest::qWait( 100 );
    REQUIRE( widget.folderResults()->getNbLine() == 2_lcount );

    // Unmarking under Marks view hides the row (and, with no marks left, the
    // group header too) via refreshForMarksChange.
    widget.filteredView()->selectAndDisplayLine( 1_lnum ); // the marked row is now at index 1
    widget.filteredView()->deleteMarksSelected();
    QTest::qWait( 100 );
    REQUIRE( widget.folderResults()->getNbLine() == 0_lcount );

    // Back to MarksAndMatches: all rows return (new search state not needed).
    widget.setResultsVisibility( FolderSearchResults::Visibility::MarksAndMatches );
    QTest::qWait( 100 );
    REQUIRE( widget.folderResults()->getNbLine() == 4_lcount );
}

TEST_CASE( "FolderCrawlerWidget filtered-view M shortcut marks the selected result row",
           "[folder]" )
{
    // The M shortcut (markSelected -> markLines) on the folder results view was
    // a dead signal: filteredView_->markLines was never connected, so pressing M
    // did nothing. Marking a result row must record (file, localLine) in the
    // shared per-file store AND render the mark bullet (lineType Mark flag).
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nERROR two\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR three\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    // rows: [H0(a), D1(a:0), D2(a:1), H3(b), D4(b:0)]

    // Select result row 1 (a.log localLine 0) and drive the M-shortcut path
    // (markSelected -> emits markLines for the selection).
    widget.filteredView()->selectAndDisplayLine( 1_lnum );
    widget.filteredView()->markSelected();
    QTest::qWait( 50 );

    // The mark landed on result row 1 (resolved to a.log:0)...
    REQUIRE( widget.isFilteredResultRowMarked( 1_lnum ) );
    // ...and NOT on a neighbouring row.
    REQUIRE_FALSE( widget.isFilteredResultRowMarked( 2_lnum ) );

    // The bullet will render: row 1's lineType now carries the Mark flag.
    REQUIRE( widget.filteredView()->lineTypeForTest( 1_lnum )
                 .testFlag( AbstractLogData::LineTypeFlags::Mark ) );

    // The mark is shared with the main view: open a.log and line 0 is marked.
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );
    REQUIRE( widget.isMainViewLineMarked( 0_lnum ) );

    // deleteMark clears it.
    widget.filteredView()->selectAndDisplayLine( 1_lnum );
    widget.filteredView()->deleteMarksSelected();
    QTest::qWait( 50 );
    REQUIRE_FALSE( widget.isFilteredResultRowMarked( 1_lnum ) );
}

TEST_CASE( "FolderCrawlerWidget main-view map rebuilds paint-free on an unrealized viewport",
           "[folder]" )
{
    // The visible-line map (wrappedLinesInfo_) is a side effect of drawTextArea
    // (paintEvent). On a hidden/unrealized viewport Qt skips repaint(), and a
    // streaming updateData() can leave a stale map, so a click delivered in that
    // window resolved to nullopt and selected nothing. ensureLineMapFresh must
    // rebuild the map WITHOUT a paint so hit-testing works headlessly too.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR here\nline2\nline3\n" ) );

    FolderCrawlerWidget widget; // deliberately NOT shown -> viewport never realized
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    widget.selectResultRow( 1_lnum ); // opens a.log -> setDataSource clears the map
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    // No paint was ever delivered, so the map is empty and hit-testing fails.
    REQUIRE_FALSE( widget.mainView()->isLineMapCurrent() );
    REQUIRE_FALSE( widget.mainView()->lineAtYForTest( 5 ).has_value() );

    // The on-demand refresh mousePressEvent calls must rebuild paint-free.
    widget.mainView()->ensureLineMapFresh();
    REQUIRE( widget.mainView()->isLineMapCurrent() );
    // And the rebuilt map now resolves a viewport coordinate.
    REQUIRE( widget.mainView()->lineAtYForTest( 5 ).has_value() );
}

TEST_CASE( "FolderCrawlerWidget announces the main-view line position when a result opens",
           "[folder]" )
{
    // Opening a result must select + announce the match line (newSelection) so
    // MainWindow's lineNumberHandler renders "Ln: x/y" immediately. Single-file
    // tabs get this via signalMux::doSendAllStateSignals; the folder is not a
    // mux document, so the announce must come from the open path itself.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "line0\nERROR here\nline2\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    QSignalSpy spy( widget.mainView(), &LogMainView::newSelection );
    widget.selectResultRow( 1_lnum ); // opens a.log at the match line (localLine 1)
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 ); // settle: setDataSource + announce run in the loadingFinished queue

    REQUIRE( spy.count() >= 1 );
    const auto first = spy.takeFirst();
    REQUIRE( first.at( 0 ).value<LineNumber>() == 1_lnum );

    // The line count needed to render "Ln:2/3" is exposed alongside.
    const auto info = widget.currentMainViewInfo();
    REQUIRE( info.has_value() );
    REQUIRE( info->nbLines == 3 );
}

TEST_CASE( "FolderCrawlerWidget main view shows each opened file's detected encoding",
           "[folder]" )
{
    // The auto-open path must sync the display codec to the detected encoding
    // (parity with CrawlerWidget::updateEncoding). Without it a non-UTF-8 file is
    // indexed with correct line positions but DISPLAYED decoded as UTF-8
    // (mojibake) and the info line wrongly reports UTF-8.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // Pure-ASCII is detected as US-ASCII by uchardet; embed a multibyte UTF-8
    // sequence (é = 0xC3 0xA9) so detection is unambiguously UTF-8 and the
    // display codec mirrors it after applyDetectedEncoding.
    const QString utf8 = writeFile( dir, "utf8.log", QByteArray( "ERROR \xc3\xa9 tag\n" ) );

    // UTF-16LE with BOM: the indexer detects UTF-16LE; display must follow.
    QByteArray utf16;
    utf16.append( "\xff\xfe" ); // BOM
    const QByteArray msg = "ERROR utf16\n";
    for ( const char c : msg ) {
        utf16.append( c );
        utf16.append( '\0' );
    }
    const QString utf16File = writeFile( dir, "utf16.log", utf16 );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ utf8, utf16File } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    // Rows: [H0(utf8), D1(utf8 match), H2(utf16), D3(utf16 match)].
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == utf8; } ) );
    QTest::qWait( 200 );
    REQUIRE( widget.currentMainViewInfo()->encodingText.toLower().contains( "utf-8" ) );

    widget.selectResultRow( 3_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == utf16File; } ) );
    QTest::qWait( 200 );
    REQUIRE( widget.currentMainViewInfo()->encodingText.toLower().contains( "utf-16" ) );
}

TEST_CASE( "FolderCrawlerWidget clearHistoryRequested clears the shared search history",
           "[folder]" )
{
    // The toolbar's clear-history action (context menu on the search line) must
    // clear the shared SavedSearches like single-file. The signal is currently
    // dropped in the folder ctor, so emitting it is a no-op (RED).
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\n" ) );

    SavedSearches ss;
    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.setSavedSearches( &ss );

    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    REQUIRE( ss.recentSearches().contains( QStringLiteral( "ERROR" ) ) );

    Q_EMIT widget.searchToolbar()->clearHistoryRequested();
    QTest::qWait( 50 );

    REQUIRE( ss.recentSearches().isEmpty() );
}

TEST_CASE( "FolderCrawlerWidget applyConfiguration repopulates predefined filters",
           "[folder]" )
{
    // applyConfiguration must re-sync the predefined-filters combo from the
    // shared collection (parity with CrawlerWidget, crawlerwidget.cpp:866), so a
    // filter saved after construction appears after optionsChanged. Currently
    // the folder's applyConfiguration skips reloadPredefinedFilters (RED).
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\n" ) );

    // RAII: strips any leftover test filter and restores the user's real
    // collection on scope exit (even if a REQUIRE throws). Must be constructed
    // BEFORE the widget so the ctor's reloadPredefinedFilters reads a clean
    // store.
    const QString testName = QStringLiteral( "zzz_late_filter_test" );
    PredefinedFiltersGuard guard{ testName };

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    REQUIRE( widget.searchToolbar()->predefinedFilters()->findText( testName ) < 0 );

    PredefinedFiltersCollection::getSynced().saveToStorage(
        { { testName, QStringLiteral( "WARN" ), false } } );

    widget.applyConfiguration();
    REQUIRE( widget.searchToolbar()->predefinedFilters()->findText( testName ) >= 0 );
}


TEST_CASE( "FolderCrawlerWidget context controls map -A/-B/-C to (before,after)", "[folder][context]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );

    REQUIRE( widget.contextLinesComboBox() != nullptr );
    REQUIRE( widget.contextLinesSpinBox() != nullptr );
    // Default is None -> (0,0).
    REQUIRE( widget.currentContext() == std::make_pair( 0, 0 ) );

    // Before -B with value 2 -> (2,0). No pattern -> no search kicked off.
    widget.contextLinesComboBox()->setCurrentIndex( 1 ); // "Before (-B)"
    widget.contextLinesSpinBox()->setValue( 2 );
    QTest::qWait( 30 );
    REQUIRE( widget.currentContext() == std::make_pair( 2, 0 ) );

    // After -A with value 3 -> (0,3).
    widget.contextLinesComboBox()->setCurrentIndex( 2 ); // "After (-A)"
    widget.contextLinesSpinBox()->setValue( 3 );
    QTest::qWait( 30 );
    REQUIRE( widget.currentContext() == std::make_pair( 0, 3 ) );

    // Around -C with value 1 -> (1,1).
    widget.contextLinesComboBox()->setCurrentIndex( 3 ); // "Around (-C)"
    widget.contextLinesSpinBox()->setValue( 1 );
    QTest::qWait( 30 );
    REQUIRE( widget.currentContext() == std::make_pair( 1, 1 ) );

    // Value 0 clears the window but keeps the chosen mode.
    widget.contextLinesSpinBox()->setValue( 0 );
    QTest::qWait( 30 );
    REQUIRE( widget.currentContext() == std::make_pair( 0, 0 ) );
    REQUIRE( widget.contextLinesComboBox()->currentIndex() == 3 );
}

TEST_CASE( "FolderCrawlerWidget -A search emits context rows that render plain", "[folder][context]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    // match on line 1 (HIT); -A2 -> context lines 2 and 3 follow it.
    const QString a = writeFile( dir, "a.log", QByteArray( "line0\nHIT\nline2\nline3\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.contextLinesComboBox()->setCurrentIndex( 2 ); // After -A
    widget.contextLinesSpinBox()->setValue( 2 );

    widget.searchFor( "HIT" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    // Visible rows: [H0, D1(Match HIT), D2(Context line2), D3(Context line3)].
    REQUIRE( widget.folderResults()->getNbLine() == 4_lcount );
    REQUIRE( widget.folderResults()->lineKind( 1_lnum ) == LineKind::Data );
    REQUIRE( widget.folderResults()->lineKind( 2_lnum ) == LineKind::Data );

    // The match row carries the Match bullet; context rows render PLAIN.
    REQUIRE( widget.filteredView()->lineTypeForTest( 1_lnum )
                 .testFlag( AbstractLogData::LineTypeFlags::Match ) );
    REQUIRE_FALSE( widget.filteredView()->lineTypeForTest( 2_lnum )
                       .testFlag( AbstractLogData::LineTypeFlags::Match ) );
    REQUIRE_FALSE( widget.filteredView()->lineTypeForTest( 3_lnum )
                       .testFlag( AbstractLogData::LineTypeFlags::Match ) );

    // isMatchRow distinguishes them.
    REQUIRE( widget.folderResults()->isMatchRow( 1_lnum ) );
    REQUIRE_FALSE( widget.folderResults()->isMatchRow( 2_lnum ) );

    // Clicking a context row still resolves to its source line (openable).
    const auto ctx = widget.folderResults()->sourceForLine( 2_lnum );
    REQUIRE( ctx.filePath == a );
    REQUIRE( ctx.localLine == 2_lnum );
}

TEST_CASE( "FolderCrawlerWidget context-line change preserves collapsed groups",
           "[folder][context]" )
{
    // Context (-A/-B/-C) is a scan-time property of folder search, so changing
    // the context window re-scans via startSearch -> FolderSearchResults::beginSearch.
    // beginSearch used to collapsed_.clear() (foldersearchresults.cpp:59), wiping
    // the user's per-file collapse set, so any context tweak expanded every
    // previously-collapsed group. The model must snapshot collapse state (keyed by
    // the stable filePath, NOT the unstable FileId) across the re-scan.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "ERROR one\nctx1\nERROR two\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR three\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    QTest::qWait( 200 ); // settle per CLAUDE.md
    // rows: [H0(a), D1(a:0), D2(a:2), H3(b), D4(b:0)] = 5.
    REQUIRE( widget.folderResults()->getNbLine() == 5_lcount );

    // Collapse group a (fileId 0): only its header stays visible.
    widget.clickHeaderRow( 0_lnum );
    QTest::qWait( 100 );
    REQUIRE( widget.folderResults()->isCollapsed( 0 ) );
    REQUIRE( widget.folderResults()->getNbLine() == 3_lcount ); // a-header + b(header+match)

    // Reproduce the gesture: switch to After(-A) and increment N from 0 to 1.
    // Both fire onContextControlsChanged -> startSearch -> beginSearch (pattern
    // present), the destructive re-scan path.
    widget.contextLinesComboBox()->setCurrentIndex( 2 ); // "After (-A)"
    widget.contextLinesSpinBox()->setValue( 1 );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    QTest::qWait( 200 ); // settle: re-scan + streaming commit + layoutChanged

    // Group a must STAY collapsed across the re-scan. RED before the fix:
    // isCollapsed(0) is false and getNbLine() is 6 (a fully expanded: header +
    // ERROR one / ctx1(context) / ERROR two = 4, plus b header+match = 2).
    REQUIRE( widget.folderResults()->isCollapsed( 0 ) );
    REQUIRE( widget.folderResults()->getNbLine() == 3_lcount );
}

TEST_CASE( "FolderCrawlerWidget keep results freezes the current pane on a new search",
           "[folder][keep]" )
{
    // Toggling Keep + running a new search snapshots the current pane (it becomes
    // a frozen tab) and starts the new search in a fresh pane. Mirrors
    // CrawlerWidget::startNewSearch.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "foo line\nbar line\nfoo again\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    REQUIRE( widget.paneCount() == 1 );

    // First search: "foo" -> 2 matches in pane 0.
    widget.searchFor( "foo" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    REQUIRE( widget.paneCount() == 1 );
    REQUIRE( widget.folderResults()->getNbLine() == 3_lcount ); // header + 2 matches

    // Keep + search "bar" -> pane 0 frozen (foo), pane 1 active (bar).
    widget.searchToolbar()->setKeepResultsChecked( true );
    widget.searchFor( "bar" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    QTest::qWait( 100 );
    REQUIRE( widget.paneCount() == 2 );

    // The active pane holds the bar match.
    REQUIRE( widget.folderResults()->getNbLine() == 2_lcount ); // header + 1 match

    // The frozen pane (tab 0) retains the foo matches.
    widget.resultsTabs()->setCurrentIndex( 0 );
    QTest::qWait( 100 );
    REQUIRE( widget.folderResults()->getNbLine() == 3_lcount );

    // Keep auto-resets after each search, so a plain follow-up overwrites the
    // active pane (no new tab).
    widget.resultsTabs()->setCurrentIndex( 1 );
    QTest::qWait( 100 );
    widget.searchFor( "foo" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    QTest::qWait( 100 );
    REQUIRE( widget.paneCount() == 2 ); // still 2, not 3
}

// RAII save/restore for the Configuration fields the F2 wiring tests mutate
// (font size zoom, line-number toggle, splitter sizes). Restores on unwind so
// a failed REQUIRE cannot leak state into sibling tests.
struct WiringConfigGuard {
    Configuration& cfg;
    QFont font;
    bool mainLines;
    QList<int> splitterSizes;

    WiringConfigGuard()
        : cfg( Configuration::get() )
        , font( cfg.mainFont() )
        , mainLines( cfg.mainLineNumbersVisible() )
        , splitterSizes( cfg.splitterSizes() )
    {
    }
    ~WiringConfigGuard()
    {
        cfg.setMainFont( font );
        cfg.setMainLineNumbersVisible( mainLines );
        cfg.setSplitterSizes( splitterSizes );
    }
};

TEST_CASE( "FolderCrawlerWidget shared view-signal wiring", "[folder][wiring]" )
{
    // Single-file parity (cluster A of the 2026-07-18 audit): CrawlerWidget
    // wires every view's scratchpad / search-composition / splitter / font /
    // exitView / highlightersChange signals (crawlerwidget.cpp:1390-1600), but
    // FolderCrawlerWidget wired none of them, so those context-menu actions and
    // shortcuts were dead in folder mode. The fix routes both hosts through a
    // shared ViewSignalWiring component.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    auto* const mainView = widget.mainView();
    auto* const filteredView = widget.filteredView();
    REQUIRE( mainView != nullptr );
    REQUIRE( filteredView != nullptr );

    mainView->selectPortionAndDisplayLine( 1_lnum, LinesCount( 1 ), LineColumn( 0 ),
                                           LineLength( 5 ) );
    REQUIRE( mainView->getSelectedText() == QStringLiteral( "ERROR alpha" ) );

    SECTION( "scratchpad actions forward the selection" )
    {
        QSignalSpy sendSpy( &widget, &FolderCrawlerWidget::sendToScratchpad );
        QSignalSpy replaceSpy( &widget, &FolderCrawlerWidget::replaceDataInScratchpad );

        Q_EMIT mainView->sendSelectionToScratchpad();
        QTest::qWait( 20 );
        REQUIRE( sendSpy.count() == 1 );
        REQUIRE( sendSpy.first().first().toString() == QStringLiteral( "ERROR alpha" ) );

        Q_EMIT mainView->replaceScratchpadWithSelection();
        QTest::qWait( 20 );
        REQUIRE( replaceSpy.count() == 1 );
        REQUIRE( replaceSpy.first().first().toString() == QStringLiteral( "ERROR alpha" ) );
    }

    SECTION( "search composition actions update the folder search pattern" )
    {
        auto* toolbar = widget.searchToolbar();
        REQUIRE( toolbar != nullptr );

        Q_EMIT mainView->replaceSearch( QStringLiteral( "beta" ) );
        QTest::qWait( 20 );
        REQUIRE( toolbar->currentSearchText() == QStringLiteral( "beta" ) );

        Q_EMIT mainView->addToSearch( QStringLiteral( "gamma" ) );
        QTest::qWait( 20 );
        REQUIRE( toolbar->currentSearchText().contains( QStringLiteral( "gamma" ) ) );

        Q_EMIT mainView->excludeFromSearch( QStringLiteral( "delta" ) );
        QTest::qWait( 20 );
        REQUIRE( toolbar->isBoolean() );
        REQUIRE( toolbar->currentSearchText().contains(
            QStringLiteral( "not(" ) ) );
    }

    SECTION( "font zoom steps the configured font and re-renders the views" )
    {
        const WiringConfigGuard guard;
        const int before = Configuration::get().mainFont().pointSize();

        Q_EMIT mainView->changeFontSize( true );
        QTest::qWait( 20 );
        REQUIRE( Configuration::get().mainFont().pointSize() > before );

        Q_EMIT mainView->changeFontSize( false );
        QTest::qWait( 20 );
        REQUIRE( Configuration::get().mainFont().pointSize() == before );
    }

    SECTION( "exitView swaps focus between the main and results views" )
    {
        filteredView->viewport()->setFocus();
        QTest::qWait( 20 );
        Q_EMIT filteredView->exitView();
        QTest::qWait( 20 );
        REQUIRE( mainView->hasFocus() );

        Q_EMIT mainView->exitView();
        QTest::qWait( 20 );
        REQUIRE( filteredView->hasFocus() );
    }

    SECTION( "highlightersChange re-applies the configuration" )
    {
        const WiringConfigGuard guard;
        const bool before = Configuration::get().mainLineNumbersVisible();
        Configuration::get().setMainLineNumbersVisible( !before );

        using Access = AbstractLogView::access_by<FolderViewTestAccess>;
        Q_EMIT mainView->highlightersChange();
        QTest::qWait( 20 );
        REQUIRE( Access::lineNumbersVisible( mainView ) == !before );
    }

    SECTION( "save splitter sizes persists the folder splitter" )
    {
        const WiringConfigGuard guard;
        Configuration::get().setSplitterSizes( QList<int>{ 3, 7 } );

        Q_EMIT mainView->saveDefaultSplitterSizes();
        QTest::qWait( 20 );
        const auto sizes = Configuration::get().splitterSizes();
        REQUIRE( sizes.size() == 2 );
        REQUIRE( sizes != QList<int>( { 3, 7 } ) );
        REQUIRE( sizes.first() > 0 );
    }
}

TEST_CASE( "FolderCrawlerWidget hides search-limit actions its search cannot honor",
           "[folder][wiring]" )
{
    // "Set search start/end" + "Clear search limits" limit a LogFilteredData
    // search. Folder search is a streaming engine scan with no range support,
    // so leaving the entries enabled grays the views without limiting anything
    // (audit: "Set search start/end and Clear search limits mislead in folder
    // views"). Folder views must not offer them.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "line0\nERROR a\nline2\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.show();
    QTest::qWait( 100 );

    using Access = AbstractLogView::access_by<FolderViewTestAccess>;
    const auto searchLimitsActionsVisible = []( const AbstractLogView* view ) {
        const auto* menu = Access::popupMenu( view );
        if ( menu == nullptr ) {
            return false;
        }
        bool anyVisible = false;
        const auto actions = menu->actions();
        for ( const auto* action : actions ) {
            if ( action->text().startsWith( QStringLiteral( "Set search st" ) )
                 || action->text().startsWith( QStringLiteral( "Clear search limits" ) ) ) {
                anyVisible = anyVisible || action->isVisible();
            }
        }
        return anyVisible;
    };

    REQUIRE( widget.mainView() != nullptr );
    REQUIRE( widget.filteredView() != nullptr );
    REQUIRE_FALSE( searchLimitsActionsVisible( widget.mainView() ) );
    REQUIRE_FALSE( searchLimitsActionsVisible( widget.filteredView() ) );
}

TEST_CASE( "FolderCrawlerWidget mirrors a portion selection into the main view",
           "[folder][wiring]" )
{
    // Single-file: a portion selection in the filtered view is mirrored to the
    // main view via CrawlerWidget::jumpToMatchingLine -> selectPortionAndDisplayLine,
    // which selects the whole line but forwards startCol/nSymbols in the
    // re-emitted newSelection. Folder's onResultSelected used to drop the
    // portion entirely.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    // Row 1 = a.log local line 1 ("ERROR alpha"). Single-file parity: the main
    // view selects the whole line (selectPortionAndDisplayLine keeps selection_
    // line-based) while the portion rides along in the re-emitted newSelection
    // payload (startCol/nSymbols) for the status bar and incremental search.
    QSignalSpy mainSelectionSpy( widget.mainView(), &AbstractLogView::newSelection );
    Q_EMIT widget.filteredView()->newSelection( 1_lnum, LinesCount( 1 ), LineColumn( 2 ),
                                                LineLength( 3 ) );
    QTest::qWait( 100 );
    REQUIRE( widget.mainView()->getSelectedText() == QStringLiteral( "ERROR alpha" ) );
    REQUIRE( mainSelectionSpy.count() >= 1 );
    const auto payload = mainSelectionSpy.last();
    REQUIRE( payload.at( 0 ).value<LineNumber>() == 1_lnum );
    REQUIRE( payload.at( 2 ).value<LineColumn>() == LineColumn( 2 ) );
    REQUIRE( payload.at( 3 ).value<LineLength>() == LineLength( 3 ) );

    // A multi-row drag emits its MOVING-END row with nLines>1 (downward drag
    // ends on the bottom row, upward drag on the top row). Parity with
    // single-file: the payload is forwarded unchanged -- NOT recomputed from
    // neighboring result rows (which would inflate the span for non-contiguous
    // matches and invert upward drags).
    Q_EMIT widget.filteredView()->newSelection( 2_lnum, LinesCount( 2 ), LineColumn( 0 ),
                                                LineLength( 0 ) );
    QTest::qWait( 100 );
    REQUIRE( widget.mainView()->getSelectedText() == QStringLiteral( "ERROR beta" ) );
    const auto rangePayload = mainSelectionSpy.last();
    REQUIRE( rangePayload.at( 0 ).value<LineNumber>() == 3_lnum ); // a.log local line 3
    REQUIRE( rangePayload.at( 1 ).value<LinesCount>() == LinesCount( 2 ) );
}

TEST_CASE( "FolderCrawlerWidget highlights the hovered result line in the minimap",
           "[folder][wiring]" )
{
    // Single-file: hovering a filtered view's line-number margin highlights the
    // corresponding line in the overview (CrawlerWidget::mouseHoveredOverMatch).
    // Folder mode resolves the hovered row to (file, localLine) and highlights
    // it when that file is open in the main view.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    auto* const overviewWidget = widget.mainView()->findChild<OverviewWidget*>();
    REQUIRE( overviewWidget != nullptr );

    // Result rows: 0 = group header, 1 = "ERROR alpha" (local line 1),
    // 2 = "ERROR beta" (local line 3). Hover row 2: the row-to-line mapping
    // is NOT identity here, so a resolution that confused rows with source
    // lines would highlight the wrong line.
    Q_EMIT widget.filteredView()->mouseHoveredOverLine( 2_lnum );
    QTest::qWait( 20 );
    REQUIRE( overviewWidget->highlightedLine().has_value() );
    REQUIRE( overviewWidget->highlightedLine()->get() == 3 );

    // Hovering a group-header row (or any row that does not resolve to the
    // open file) clears the highlight instead of leaving a stale marker.
    Q_EMIT widget.filteredView()->mouseHoveredOverLine( 0_lnum );
    QTest::qWait( 20 );
    REQUIRE_FALSE( overviewWidget->highlightedLine().has_value() );

    Q_EMIT widget.filteredView()->mouseHoveredOverLine( 2_lnum );
    QTest::qWait( 20 );
    REQUIRE( overviewWidget->highlightedLine().has_value() );
    REQUIRE( overviewWidget->highlightedLine()->get() == 3 );

    Q_EMIT widget.filteredView()->mouseLeftHoveringZone();
    QTest::qWait( 20 );
    REQUIRE_FALSE( overviewWidget->highlightedLine().has_value() );
}

TEST_CASE( "FolderCrawlerWidget seeds the splitter from the saved default",
           "[folder][wiring]" )
{
    // Parity with CrawlerWidget::setup (setSizes(config.splitterSizes())):
    // "Save splitter position" writes the global default; NEW folder tabs must
    // open with those proportions too (a session-restored per-tab context
    // still overrides per tab).
    WiringConfigGuard guard;
    guard.cfg.setSplitterSizes( { 420, 180 } );

    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "line0\nERROR alpha\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    // Resize BEFORE the first show: the seed is applied by the event filter on
    // the splitter's first Resize (its first real geometry); a post-show
    // resize would redistribute the sizes (bottom pane clamps at its minimum),
    // masking the seeded ratio.
    widget.resize( 800, 600 );
    widget.show();
    QTest::qWait( 100 );

    const auto sizes = widget.viewsSplitter()->sizes();
    REQUIRE( sizes.size() == 2 );
    REQUIRE( sizes.at( 0 ) > 0 );
    REQUIRE( sizes.at( 1 ) > 0 );
    // The splitter scales the seeded sizes to the laid-out height, so compare
    // ratios: 420:180 ~= 2.33 vs the 3:2 (1.5) ctor default.
    const double ratio = static_cast<double>( sizes.at( 0 ) ) / sizes.at( 1 );
    REQUIRE( ratio > 2.0 );
    REQUIRE( ratio < 2.7 );
}

TEST_CASE( "FolderCrawlerWidget session splitter sizes beat the saved default",
           "[folder][wiring]" )
{
    // Session restore delivers per-tab sizes via setViewContext while the tab
    // is still hidden. Those sizes must win over the saved global default --
    // and pre-layout setSizes must not silently drop them (the sizes are
    // stashed and applied on the splitter's first real geometry).
    WiringConfigGuard guard;
    guard.cfg.setSplitterSizes( { 420, 180 } );

    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log", QByteArray( "line0\nERROR alpha\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.setViewContext( QStringLiteral(
        R"({"P":"","IC":false,"RE":false,"IR":false,"BC":false,"S":[200,400]})" ) );
    widget.resize( 800, 600 );
    widget.show();
    QTest::qWait( 100 );

    const auto sizes = widget.viewsSplitter()->sizes();
    REQUIRE( sizes.size() == 2 );
    REQUIRE( sizes.at( 0 ) > 0 );
    REQUIRE( sizes.at( 1 ) > 0 );
    // 200:400 = 0.5 -- clearly distinct from the 420:180 global default (2.33)
    // and from the unseeded minimum-clamped layout (~3.4).
    const double ratio = static_cast<double>( sizes.at( 0 ) ) / sizes.at( 1 );
    REQUIRE( ratio > 0.4 );
    REQUIRE( ratio < 0.65 );
}

TEST_CASE( "FolderCrawlerWidget marks appear in the overview", "[folder][overview]" )
{
    // Single-file parity: marks show as ticks in the minimap. Folder mode fed
    // the overview only the match list, so marks never appeared (cluster D of
    // the 2026-07-18 audit). The ticks belong to the file currently shown and
    // follow result-row switches.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR gamma\nline1\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    auto* const overview = widget.overviewModel();
    REQUIRE( overview != nullptr );
    // The overview refresh path no-ops while hidden; visibility follows the
    // machine's Configuration, so force it (single-file tests do the same).
    overview->setVisible( true );

    // Mark a NON-match line: it must appear as a mark tick.
    widget.markMainViewLine( 2_lnum );
    QTest::qWait( 50 );
    overview->updateView( 100 );
    REQUIRE( !overview->getMarkLines()->empty() );

    widget.unmarkMainViewLine( 2_lnum );
    QTest::qWait( 50 );
    overview->updateView( 100 );
    // Re-fetch after every update*() call: the returned pointer is documented
    // valid only until the next update (overview.h).
    REQUIRE( overview->getMarkLines()->empty() );

    // Single-file precedence: a line that is BOTH a match and a mark is drawn
    // as a match (red), so it must NOT be duplicated into the mark list.
    widget.markMainViewLine( 1_lnum ); // "ERROR alpha" is a match
    QTest::qWait( 50 );
    overview->updateView( 100 );
    REQUIRE( overview->getMarkLines()->empty() );
    REQUIRE( !overview->getMatchLines()->empty() );
    widget.unmarkMainViewLine( 1_lnum );
    QTest::qWait( 50 );

    // Rows: 0 = header(a), 1 = alpha, 2 = beta, 3 = header(b), 4 = gamma.
    // Mark a non-match line in b, then switch between the files: the minimap
    // ticks must track the file currently shown in the main view.
    widget.selectResultRow( 4_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == b; } ) );
    QTest::qWait( 100 );
    widget.markMainViewLine( 1_lnum ); // b.log:1 is "line1", not a match
    QTest::qWait( 50 );
    overview->updateView( 100 );
    REQUIRE( !overview->getMarkLines()->empty() );

    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 100 );
    overview->updateView( 100 );
    // a.log has no marks anymore (unmarked above) -> no stale ticks from b.
    REQUIRE( overview->getMarkLines()->empty() );
}

TEST_CASE( "FolderCrawlerWidget marks survive a view-context round-trip",
           "[folder][session]" )
{
    // Single-file parity: marks persist in the session (CrawlerWidgetContext
    // serializes them). Folder mode dropped them: the per-file mark store
    // lived only in memory (cluster D of the 2026-07-18 audit). Marks are
    // serialized RELATIVE to the folder root so a moved folder restores.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR gamma\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    // Mark a main-view line in a.log and a results row from b.log
    // (rows: 0 = header(a), 1 = alpha, 2 = beta, 3 = header(b), 4 = gamma).
    widget.markMainViewLine( 1_lnum );
    Q_EMIT widget.filteredView()->markLines( { 4_lnum } );
    QTest::qWait( 50 );
    REQUIRE( widget.isLineMarkedInFile( a, 1_lnum ) );
    REQUIRE( widget.isLineMarkedInFile( b, 0_lnum ) );

    const auto json = widget.context()->toString();
    // The serialized context carries the marks under a dedicated key, with
    // paths RELATIVE to the folder root (so a moved folder restores) -- it
    // must not embed the (machine-specific) absolute folder path.
    REQUIRE( json.contains( QStringLiteral( "\"M\"" ) ) );
    REQUIRE( json.contains( QStringLiteral( "a.log" ) ) );
    REQUIRE_FALSE( json.contains( dir.path() ) );

    FolderCrawlerWidget restored;
    restored.setFolder( dir.path(), QStringList{ a, b } );
    restored.setViewContext( json );
    restored.show();
    QTest::qWait( 100 );

    REQUIRE( restored.isLineMarkedInFile( a, 1_lnum ) );
    REQUIRE( restored.isLineMarkedInFile( b, 0_lnum ) );
}

TEST_CASE( "FolderCrawlerWidget restores marks into a moved folder",
           "[folder][session]" )
{
    // The mark keys are relative to the folder root precisely so that a folder
    // MOVED between save and restore still gets its marks: the same relative
    // paths must resolve against the NEW root.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );
    widget.markMainViewLine( 1_lnum );
    QTest::qWait( 20 );

    const auto json = widget.context()->toString();

    // Same folder content at a NEW location (the "moved" folder).
    QTemporaryDir movedDir;
    REQUIRE( movedDir.isValid() );
    const QString movedA = writeFile( movedDir, "a.log",
                                      QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );
    FolderCrawlerWidget restored;
    restored.setFolder( movedDir.path(), QStringList{ movedA } );
    restored.setViewContext( json );
    restored.show();
    QTest::qWait( 100 );

    REQUIRE( restored.isLineMarkedInFile( movedA, 1_lnum ) );
    // And not keyed to the original (now-gone) location.
    REQUIRE_FALSE( restored.isLineMarkedInFile( a, 1_lnum ) );
}

TEST_CASE( "FolderCrawlerWidget keeps restored marks for files absent at restore",
           "[folder][session]" )
{
    // Contract (pinned, mirrors single-file's blind mark restore): a session
    // carrying marks for a file the folder no longer lists restores them
    // anyway; they re-render if the file returns. Restore does not prune.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR gamma\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    Q_EMIT widget.filteredView()->markLines( { 4_lnum } ); // b.log row
    QTest::qWait( 50 );
    REQUIRE( widget.isLineMarkedInFile( b, 0_lnum ) );

    const auto json = widget.context()->toString();

    // Restore with b.log absent from the folder's file list.
    FolderCrawlerWidget restored;
    restored.setFolder( dir.path(), QStringList{ a } );
    restored.setViewContext( json );
    restored.show();
    QTest::qWait( 100 );

    REQUIRE( restored.isLineMarkedInFile( b, 0_lnum ) );
}

TEST_CASE( "FolderCrawlerWidget document-level actions", "[folder][actions]" )
{
    // F5: MainWindow dispatches these through AbstractCrawlerWidget for every
    // tab kind (focus-search shortcut, View->Wrap, QuickFind focus save /
    // restore, encoding override). On folder tabs they were dead because
    // MainWindow routed them via currentCrawlerWidget() / the SignalMux.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );
    const QString b = writeFile( dir, "b.log", QByteArray( "ERROR gamma\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a, b } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    SECTION( "focusSearchEdit focuses the search input" )
    {
        widget.mainView()->viewport()->setFocus();
        QTest::qWait( 20 );
        widget.focusSearchEdit();
        QTest::qWait( 20 );
        REQUIRE( widget.searchToolbar()->searchLineEdit()->lineEdit()->hasFocus() );
    }

    SECTION( "textWrapSet wraps every view and reports its state" )
    {
        widget.textWrapSet( true );
        QTest::qWait( 20 );
        REQUIRE( widget.isTextWrapEnabled() );
        REQUIRE( widget.mainView()->isTextWrapEnabled() );
        REQUIRE( widget.filteredView()->isTextWrapEnabled() );

        widget.textWrapSet( false );
        QTest::qWait( 20 );
        REQUIRE_FALSE( widget.isTextWrapEnabled() );
        REQUIRE_FALSE( widget.mainView()->isTextWrapEnabled() );
        REQUIRE_FALSE( widget.filteredView()->isTextWrapEnabled() );
    }

    SECTION( "quickfind entry and exit save and restore the view focus" )
    {
        widget.filteredView()->viewport()->setFocus();
        QTest::qWait( 20 );
        widget.enteringQuickFind();

        // The QuickFind bar grabs the focus; simulate by moving it elsewhere.
        widget.mainView()->viewport()->setFocus();
        QTest::qWait( 20 );

        widget.exitingQuickFind();
        QTest::qWait( 20 );
        REQUIRE( widget.filteredView()->viewport()->hasFocus() );
    }

    SECTION( "quickfind exit does not yank focus from a non-view widget" )
    {
        // Focus is in the search box (common when Ctrl+F is pressed): nothing
        // is saved, so exiting must NOT move the focus anywhere.
        widget.searchToolbar()->searchLineEdit()->lineEdit()->setFocus();
        QTest::qWait( 20 );
        widget.enteringQuickFind();

        widget.mainView()->viewport()->setFocus();
        QTest::qWait( 20 );

        widget.exitingQuickFind();
        QTest::qWait( 20 );
        // No restore happened: the focus is still where the QF bar left it.
        REQUIRE( widget.mainView()->viewport()->hasFocus() );
    }

    SECTION( "encodingMib reflects the encoding override" )
    {
        // ISO 8859-1 (Latin-1).
        constexpr int latin1Mib = 4;

        REQUIRE_FALSE( widget.encodingMib().has_value() );
        widget.setEncoding( latin1Mib );
        REQUIRE( widget.encodingMib().has_value() );
        REQUIRE( *widget.encodingMib() == latin1Mib );
        widget.setEncoding( std::nullopt );
        REQUIRE_FALSE( widget.encodingMib().has_value() );
    }

    SECTION( "encodingMib resets when the main-view file changes" )
    {
        constexpr int latin1Mib = 4;

        widget.setEncoding( latin1Mib );
        REQUIRE( widget.encodingMib().has_value() );

        // Rows: 0 = header(a), 1 = ERROR alpha, 2 = ERROR beta, 3 = header(b),
        // 4 = ERROR gamma. Selecting a b.log row swaps the main-view file.
        widget.selectResultRow( 4_lnum );
        REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == b; } ) );
        QTest::qWait( 100 );
        REQUIRE_FALSE( widget.encodingMib().has_value() );
    }
}

TEST_CASE( "FolderCrawlerWidget search history and status guards",
           "[folder][history]" )
{
    // Single-file parity (cluster F of the 2026-07-18 audit): folder searches
    // were never persisted to the shared SavedSearches (in-memory only, wiped
    // on restart), an invalid regex silently scanned to zero matches, an empty
    // Enter left stale results, a new search kept a marks-only visibility, and
    // an option toggle replaced the match count with a bare hint.
    struct RegexpTypeGuard {
        Configuration& cfg;
        SearchRegexpType previous;
        RegexpTypeGuard()
            : cfg( Configuration::get() )
            , previous( cfg.mainRegexpType() )
        {
            cfg.setMainRegexpType( SearchRegexpType::ExtendedRegexp );
        }
        ~RegexpTypeGuard()
        {
            cfg.setMainRegexpType( previous );
        }
    } regexpGuard;

    // Snapshot/restore the shared SavedSearches singleton (Persistable ->
    // session settings on disk). A failed REQUIRE throws, so the restore must
    // run on stack unwinding; otherwise the test pattern leaks into sibling
    // tests and into the portable session file.
    struct SavedSearchesGuard {
        SavedSearches& searches;
        QStringList previous;
        SavedSearchesGuard()
            : searches( SavedSearches::getSynced() )
            , previous( searches.recentSearches() )
        {
        }
        ~SavedSearchesGuard()
        {
            searches.clear();
            for ( auto it = previous.crbegin(); it != previous.crend(); ++it ) {
                searches.addRecent( *it );
            }
            searches.save();
        }
    };

    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );

    SECTION( "folder searches persist to the shared history" )
    {
        // In production MainWindow injects the SavedSearches singleton; inject
        // it here and verify the pattern survives a disk reload (getSynced
        // re-reads the storage, so an in-memory-only addRecent would vanish).
        SavedSearchesGuard searchesGuard;
        auto& searches = SavedSearches::getSynced();
        searches.clear();
        searches.save();
        widget.setSavedSearches( &searches );

        widget.searchFor( "UNIQ_F4_HISTORY_PATTERN" );
        REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

        const auto reloaded = SavedSearches::getSynced().recentSearches();
        REQUIRE( reloaded.contains( QStringLiteral( "UNIQ_F4_HISTORY_PATTERN" ) ) );
    }

    SECTION( "invalid regex surfaces an error and does not scan" )
    {
        widget.searchFor( "ERROR" );
        REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
        REQUIRE( widget.folderResults()->getNbLine() == 3_lcount );

        widget.searchFor( "[invalid" );
        QTest::qWait( 100 );

        REQUIRE_FALSE( widget.isSearchActive() );
        REQUIRE( widget.folderResults()->getNbLine() == 0_lcount );
        REQUIRE( widget.statusText().contains( QStringLiteral( "Error in expression" ) ) );
    }

    SECTION( "empty search clears the results pane" )
    {
        widget.searchFor( "ERROR" );
        REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
        REQUIRE( widget.folderResults()->getNbLine() == 3_lcount );

        widget.searchFor( "" );
        QTest::qWait( 50 );

        REQUIRE( widget.folderResults()->getNbLine() == 0_lcount );
        REQUIRE( widget.statusText().startsWith( QStringLiteral( "Ready" ) ) );
    }

    SECTION( "new search resets a marks-only visibility" )
    {
        widget.searchFor( "ERROR" );
        REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

        auto* const combo = widget.visibilityCombo();
        REQUIRE( combo != nullptr );
        combo->setCurrentIndex( 1 ); // "Marks"
        QTest::qWait( 20 );
        REQUIRE( combo->currentIndex() == 1 );

        widget.searchFor( "ERROR" );
        REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
        REQUIRE( combo->currentIndex() == 0 );
    }

    SECTION( "invalid regex supersedes an in-flight search" )
    {
        // No waitFor between the two searches: the first scan's queued
        // progress/finish signals arrive AFTER the invalid pattern was
        // rejected, and must be treated as stale (generation bumped) instead
        // of streaming results back into the cleared pane and overwriting the
        // error. Back-to-back searchFor calls are deterministic here: the
        // queued signals need an event-loop turn the synchronous calls do not
        // give them.
        widget.searchFor( "ERROR" );
        widget.searchFor( "[invalid" );
        QTest::qWait( 200 );

        REQUIRE_FALSE( widget.isSearchActive() );
        REQUIRE( widget.folderResults()->getNbLine() == 0_lcount );
        REQUIRE( widget.statusText().contains( QStringLiteral( "Error in expression" ) ) );
    }

    SECTION( "empty search supersedes an in-flight search" )
    {
        widget.searchFor( "ERROR" );
        widget.searchFor( "" );
        QTest::qWait( 200 );

        REQUIRE_FALSE( widget.isSearchActive() );
        REQUIRE( widget.folderResults()->getNbLine() == 0_lcount );
        REQUIRE( widget.statusText().startsWith( QStringLiteral( "Ready" ) ) );
    }

    SECTION( "option toggle before any search shows the bare hint" )
    {
        auto* const toolbar = widget.searchToolbar();
        REQUIRE( toolbar != nullptr );
        toolbar->matchCaseButton()->toggle();
        QTest::qWait( 20 );

        REQUIRE( widget.statusText().contains( QStringLiteral( "Options changed" ) ) );
        REQUIRE_FALSE( widget.statusText().contains( QStringLiteral( "match" ) ) );
    }

    SECTION( "option toggle after an invalid pattern shows no stale count" )
    {
        widget.searchFor( "ERROR" );
        REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
        REQUIRE( widget.statusText().contains( QStringLiteral( "2 match" ) ) );

        widget.searchFor( "[invalid" );
        QTest::qWait( 50 );
        REQUIRE( widget.statusText().contains( QStringLiteral( "Error in expression" ) ) );

        auto* const toolbar = widget.searchToolbar();
        REQUIRE( toolbar != nullptr );
        toolbar->matchCaseButton()->toggle();
        QTest::qWait( 20 );

        REQUIRE( widget.statusText().contains( QStringLiteral( "Options changed" ) ) );
        REQUIRE_FALSE( widget.statusText().contains( QStringLiteral( "2 match" ) ) );
    }

    SECTION( "option toggle preserves the match count" )
    {
        widget.searchFor( "ERROR" );
        REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
        REQUIRE( widget.statusText().contains( QStringLiteral( "2 match" ) ) );

        auto* const toolbar = widget.searchToolbar();
        REQUIRE( toolbar != nullptr );
        toolbar->matchCaseButton()->toggle();
        QTest::qWait( 20 );

        // The stale-result hint may appear, but the count from the last
        // finished search must not be wiped by it.
        REQUIRE( widget.statusText().contains( QStringLiteral( "2 match" ) ) );
        REQUIRE( widget.statusText().contains( QStringLiteral( "re-run" ) ) );
    }
}

TEST_CASE( "FolderCrawlerWidget registers the crawler widget shortcut family",
           "[folder][shortcuts]" )
{
    // Single-file: CrawlerWidget::registerShortcuts binds the crawler family
    // (visibility cycling, option toggles, keep-results, top-view resize, Esc
    // refocus) with WidgetWithChildrenShortcut. Folder mode registered none of
    // them -- every key below was dead on a folder tab.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    auto* const mainView = widget.mainView();
    REQUIRE( mainView != nullptr );

    SECTION( "V / Shift+V cycle the visibility combo" )
    {
        auto* const combo = widget.visibilityCombo();
        REQUIRE( combo != nullptr );
        REQUIRE( combo->currentIndex() == 0 );

        mainView->viewport()->setFocus();
        QTest::qWait( 20 );
        pressConfiguredShortcut( mainView->viewport(),
                                 ShortcutAction::CrawlerChangeVisibilityForward );
        REQUIRE( combo->currentIndex() == 1 );

        pressConfiguredShortcut( mainView->viewport(),
                                 ShortcutAction::CrawlerChangeVisibilityBackward );
        REQUIRE( combo->currentIndex() == 0 );
    }

    SECTION( "option toggle shortcuts flip the toolbar buttons" )
    {
        auto* const toolbar = widget.searchToolbar();
        REQUIRE( toolbar != nullptr );

        // Assert each keypress FLIPS the button rather than landing on a fixed
        // state: the toolbar restores its toggles from the machine's saved
        // Configuration, so the initial checked state is not portable (e.g. a
        // non-plain default regexp type starts the regex button checked).
        mainView->viewport()->setFocus();
        QTest::qWait( 20 );

        const bool caseBefore = toolbar->matchCaseButton()->isChecked();
        pressConfiguredShortcut( mainView->viewport(), ShortcutAction::CrawlerEnableCaseMatching );
        REQUIRE( toolbar->matchCaseButton()->isChecked() == !caseBefore );

        const bool regexBefore = toolbar->useRegexpButton()->isChecked();
        pressConfiguredShortcut( mainView->viewport(), ShortcutAction::CrawlerEnableRegex );
        REQUIRE( toolbar->useRegexpButton()->isChecked() == !regexBefore );

        const bool inverseBefore = toolbar->inverseButton()->isChecked();
        pressConfiguredShortcut( mainView->viewport(), ShortcutAction::CrawlerEnableInverseMatching );
        REQUIRE( toolbar->inverseButton()->isChecked() == !inverseBefore );

        // Toggling case again restores the initial state (round-trip).
        pressConfiguredShortcut( mainView->viewport(), ShortcutAction::CrawlerEnableCaseMatching );
        REQUIRE( toolbar->matchCaseButton()->isChecked() == caseBefore );
    }

    SECTION( "keep-results shortcut toggles the toolbar button" )
    {
        auto* const toolbar = widget.searchToolbar();
        REQUIRE( toolbar != nullptr );
        REQUIRE( toolbar->keepSearchResultsButton()->isVisible() );

        mainView->viewport()->setFocus();
        QTest::qWait( 20 );
        const bool keepBefore = toolbar->keepSearchResultsButton()->isChecked();
        pressConfiguredShortcut( mainView->viewport(), ShortcutAction::CrawlerKeepResults );
        REQUIRE( toolbar->keepSearchResultsButton()->isChecked() == !keepBefore );
    }

    SECTION( "Esc moves focus from the search input back to the active view" )
    {
        auto* const toolbar = widget.searchToolbar();
        REQUIRE( toolbar != nullptr );
        auto* const edit = toolbar->searchLineEdit()->lineEdit();
        REQUIRE( edit != nullptr );

        edit->setFocus();
        QTest::qWait( 20 );
        REQUIRE( edit->hasFocus() );

        QTest::keyClick( edit, Qt::Key_Escape );
        QTest::qWait( 20 );
        REQUIRE_FALSE( edit->hasFocus() );
        // Folder's active view defaults to the active results pane (the view
        // itself takes focus, as in single-file).
        REQUIRE( widget.filteredView()->hasFocus() );
    }

    SECTION( "+/- resize the top view through the splitter" )
    {
        auto* const splitter = widget.viewsSplitter();
        REQUIRE( splitter != nullptr );
        // Give the bottom pane slack above its minimum height (~130px) so the
        // top can actually grow.
        widget.resize( 800, 1000 );
        QTest::qWait( 50 );
        const auto before = splitter->sizes();
        REQUIRE( before.size() == 2 );
        REQUIRE( before.at( 1 ) > 150 );

        mainView->viewport()->setFocus();
        QTest::qWait( 20 );
        pressConfiguredShortcut( mainView->viewport(),
                                 ShortcutAction::CrawlerIncreseTopViewSize );
        const auto grown = splitter->sizes();
        REQUIRE( grown.at( 0 ) > before.at( 0 ) );

        pressConfiguredShortcut( mainView->viewport(),
                                 ShortcutAction::CrawlerDecreaseTopViewSize );
        REQUIRE( splitter->sizes().at( 0 ) <= before.at( 0 ) );
    }
}

TEST_CASE( "Folder results view navigates marks with bracket shortcuts",
           "[folder][shortcuts]" )
{
    // Single-file: FilteredView registers LogViewNextMark/LogViewPrevMark ([ /
    // ]). FolderFilteredView derives straight from AbstractLogView, which never
    // registered them, so bracket navigation was dead in folder result panes.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    auto* const view = widget.filteredView();
    REQUIRE( view != nullptr );

    // Rows: 0 = group header, 1 = "ERROR alpha" (a.log:1), 2 = "ERROR beta"
    // (a.log:3). Mark both match rows and select the first one.
    Q_EMIT view->markLines( { 1_lnum, 2_lnum } );
    QTest::qWait( 20 );
    REQUIRE( widget.isFilteredResultRowMarked( 1_lnum ) );
    REQUIRE( widget.isFilteredResultRowMarked( 2_lnum ) );

    QSignalSpy selectionSpy( view, &AbstractLogView::newSelection );
    view->selectAndDisplayLine( 1_lnum );
    QTest::qWait( 20 );

    view->viewport()->setFocus();
    QTest::qWait( 20 );

    auto spyCount = selectionSpy.count();
    pressConfiguredShortcut( view->viewport(), ShortcutAction::LogViewNextMark );
    REQUIRE( selectionSpy.count() == spyCount + 1 );
    REQUIRE( selectionSpy.last().at( 0 ).value<LineNumber>() == 2_lnum );

    spyCount = selectionSpy.count();
    pressConfiguredShortcut( view->viewport(), ShortcutAction::LogViewPrevMark );
    REQUIRE( selectionSpy.count() == spyCount + 1 );
    REQUIRE( selectionSpy.last().at( 0 ).value<LineNumber>() == 1_lnum );
}

TEST_CASE( "FolderCrawlerWidget color labels apply to the selection in every view",
           "[folder][colorlabels]" )
{
    // Single-file parity: CrawlerWidget wires the views' color-label signals to
    // a ColorLabelsManager and pushes the resulting quick highlighters into BOTH
    // views (crawlerwidget.cpp:1456-1464, :1586-1602), and registers the
    // widget-level label shortcuts 1..9 (add) / 0 (remove) / Cmd+D (next) /
    // Cmd+Shift+0 (clear) (crawlerwidget.cpp:1704-1727). The AbstractLogView base
    // builds the "Color labels" context menu for folder views too, so picking a
    // label (or pressing a digit shortcut) in a folder tab must highlight the
    // selected text in the main view AND in the results view.
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const QString a = writeFile( dir, "a.log",
                                 QByteArray( "line0\nERROR alpha\nline2\nERROR beta\nline4\n" ) );

    FolderCrawlerWidget widget;
    widget.setFolder( dir.path(), QStringList{ a } );
    widget.show();
    widget.resize( 800, 600 );
    QTest::qWait( 100 );
    widget.searchFor( "ERROR" );
    REQUIRE( waitFor( [ & ]() { return !widget.isSearchActive(); } ) );

    // Open a.log in the main view and select the "ERROR" portion of line 1.
    widget.selectResultRow( 1_lnum );
    REQUIRE( waitFor( [ & ]() { return widget.currentMainFilePath() == a; } ) );
    QTest::qWait( 200 );

    auto* const mainView = widget.mainView();
    auto* const filteredView = widget.filteredView();
    REQUIRE( mainView != nullptr );
    REQUIRE( filteredView != nullptr );

    using Access = AbstractLogView::access_by<FolderViewTestAccess>;
    const auto hasLabelledText = []( const AbstractLogView* view, size_t label,
                                     const QString& text ) {
        const auto& labels = Access::quickHighlighters( view );
        return label < labels.size()
               && std::any_of( labels[ label ].cbegin(), labels[ label ].cend(),
                               [ & ]( const QuickLabelEntry& e ) { return e.text == text; } );
    };
    const auto hasAnyLabelledText = []( const AbstractLogView* view ) {
        const auto& labels = Access::quickHighlighters( view );
        return std::any_of( labels.cbegin(), labels.cend(),
                            []( const auto& entries ) { return !entries.isEmpty(); } );
    };

    // selectPortionAndDisplayLine selects the whole line (selection_.selectLine),
    // so the labelled text is the full line content.
    mainView->selectPortionAndDisplayLine( 1_lnum, LinesCount( 1 ), LineColumn( 0 ),
                                           LineLength( 5 ) );
    const QString selectedText = QStringLiteral( "ERROR alpha" );
    REQUIRE( mainView->getSelectedText() == selectedText );

    SECTION( "context-menu signal applies and removes the label in every view" )
    {
        Q_EMIT mainView->addColorLabel( 0 );
        QTest::qWait( 20 );
        REQUIRE( hasLabelledText( mainView, 0, selectedText ) );
        REQUIRE( hasLabelledText( filteredView, 0, selectedText ) );

        // The "None" menu action emits removeColorLabel.
        Q_EMIT mainView->removeColorLabel();
        QTest::qWait( 20 );
        REQUIRE_FALSE( hasLabelledText( mainView, 0, selectedText ) );
        REQUIRE_FALSE( hasLabelledText( filteredView, 0, selectedText ) );
    }

    SECTION( "digit shortcut applies and removes the label" )
    {
        mainView->viewport()->setFocus();
        QTest::qWait( 20 );
        QTest::keyClick( mainView->viewport(), Qt::Key_1 );
        QTest::qWait( 20 );
        REQUIRE( hasLabelledText( mainView, 0, selectedText ) );
        REQUIRE( hasLabelledText( filteredView, 0, selectedText ) );

        QTest::keyClick( mainView->viewport(), Qt::Key_0 );
        QTest::qWait( 20 );
        REQUIRE_FALSE( hasLabelledText( mainView, 0, selectedText ) );
    }

    SECTION( "clear removes every label from every view" )
    {
        Q_EMIT mainView->addColorLabel( 0 );
        QTest::qWait( 20 );
        REQUIRE( hasAnyLabelledText( mainView ) );

        Q_EMIT mainView->clearColorLabels();
        QTest::qWait( 20 );
        REQUIRE_FALSE( hasAnyLabelledText( mainView ) );
        REQUIRE_FALSE( hasAnyLabelledText( filteredView ) );
    }
}
