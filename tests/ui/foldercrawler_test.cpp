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

#include <QByteArray>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

#include "abstractcrawlerwidget.h"
#include "configuration.h"
#include "foldercrawlerwidget.h"
#include "folderfilteredview.h"
#include "foldersearchresults.h"
#include "linetypes.h"
#include "logmainview.h"
#include "overview.h"
#include "overviewwidget.h"
#include "quickfindmux.h"
#include "quickfindpattern.h"
#include "regularexpressionpattern.h"
#include "savedsearches.h"
#include "searchtoolbar.h"
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
} // namespace

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
    // Previously they gated on currentCrawlerWidget() (qobject_cast<CrawlerWidget*>,
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
