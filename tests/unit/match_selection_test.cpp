#include <catch2/catch.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include "abstractlogview.h"
#include "containers.h"
#include "quickfindpattern.h"
#include "test_utils.h"

namespace {
class MatchSelectionData : public AbstractLogData {
public:
    QStringList lines;
    mutable size_t reads = 0;

protected:
    QString doGetLineString( LineNumber line ) const override
    {
        ++reads;
        return lines.value( static_cast<klogg::ContainerIndex>( line.get() ) );
    }
    QString doGetExpandedLineString( LineNumber line ) const override
    {
        return untabify( doGetLineString( line ) );
    }
    klogg::vector<QString> doGetLines( LineNumber first, LinesCount count ) const override
    {
        klogg::vector<QString> result;
        for ( auto line = first; line < first + count; ++line ) {
            result.push_back( doGetLineString( line ) );
        }
        return result;
    }
    klogg::vector<QString> doGetExpandedLines( LineNumber first, LinesCount count ) const override
    {
        auto result = doGetLines( first, count );
        for ( auto& line : result ) {
            line = untabify( std::move( line ) );
        }
        return result;
    }
    LineNumber doGetLineNumber( LineNumber line ) const override
    {
        return line;
    }
    LinesCount doGetNbLine() const override
    {
        return LinesCount{ static_cast<uint64_t>( lines.size() ) };
    }
    LineLength doGetMaxLength() const override
    {
        return 200_length;
    }
    LineLength doGetLineLength( LineNumber line ) const override
    {
        return LineLength{ doGetExpandedLineString( line ).size() };
    }
    void doSetDisplayEncoding( const char* ) override {}
    QTextCodec* doGetDisplayEncoding() const override
    {
        return QTextCodec::codecForName( "UTF-8" );
    }
    void doAttachReader() const override {}
    void doDetachReader() const override {}
};

class MatchSelectionView : public AbstractLogView {
public:
    MatchSelectionView( MatchSelectionData* data, const QuickFindPattern* pattern )
        : AbstractLogView( data, pattern )
    {
        HighlighterSetCollection::getSynced();
    }
    std::vector<LineNumber> matches;

protected:
    AbstractLogData::LineType lineType( LineNumber line ) const override
    {
        return std::find( matches.begin(), matches.end(), line ) != matches.end()
                   ? AbstractLogData::LineTypeFlags::Match
                   : AbstractLogData::LineTypeFlags::Mark;
    }
};

RegularExpressionPattern filter( const QString& expression )
{
    RegularExpressionPattern result;
    result.pattern = expression;
    result.isCaseSensitive = true;
    return result;
}

void selectMatches( MatchSelectionView& view, bool quickFind = false, bool copy = false )
{
    SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
    REQUIRE( view.selectAllMatches( quickFind, copy ) );
    REQUIRE( finished.safeWait() );
    REQUIRE( finished.count() == 1 );
    REQUIRE( finished.at( 0 ).at( 0 ).toBool() );
}
} // namespace

TEST_CASE( "Select all filter matches copies fragments rather than marked or context rows",
           "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "[plt-installed] /a.so:f /b.so:g" ),
                              QStringLiteral( "[plt-installed] /marked.so:f" ),
                              QStringLiteral( "[plt-installed] /a.so:f" ) };
    QuickFindPattern quickFind;
    MatchSelectionView view{ &data, &quickFind };
    view.matches = { 0_lnum, 2_lnum };
    view.setSearchPattern( filter( QStringLiteral( R"((?<=\[plt-installed\] )[^:]*)" ) ) );
    selectMatches( view );
    REQUIRE( view.isPartialSelection() );
    REQUIRE( view.getSelectedLinesText()
             == QStringList{ QStringLiteral( "/a.so" ), QStringLiteral( "/a.so" ) } );
}

TEST_CASE( "Match selection uses capture projection and raw text with display tab positions",
           "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "prefix\t[a\tb] [c]" ) };
    QuickFindPattern quickFind;
    MatchSelectionView view{ &data, &quickFind };
    view.matches = { 0_lnum };
    view.setSearchPattern( filter( QStringLiteral( R"(\[([^]]+)\])" ) ) );
    selectMatches( view );
    REQUIRE( view.getSelectedLinesText()
             == QStringList{ QStringLiteral( "a\tb" ), QStringLiteral( "c" ) } );
}

TEST_CASE( "Nested captures merge within an occurrence but overlapping occurrences remain distinct",
           "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "aaa" ) };
    QuickFindPattern quickFind;
    MatchSelectionView view{ &data, &quickFind };
    view.matches = { 0_lnum };
    SECTION( "Nested captures" )
    {
        view.setSearchPattern( filter( QStringLiteral( "((aa))" ) ) );
        selectMatches( view );
        REQUIRE( view.getSelectedLinesText() == QStringList{ QStringLiteral( "aa" ) } );
    }
    SECTION( "Overlapping lookahead occurrences" )
    {
        view.setSearchPattern( filter( QStringLiteral( "(?=(aa))" ) ) );
        selectMatches( view );
        REQUIRE( view.getSelectedLinesText()
                 == QStringList{ QStringLiteral( "aa" ), QStringLiteral( "aa" ) } );
    }
}

TEST_CASE( "Quick Find selection uses full matches independently of filter classification",
           "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "prefixwanted prefixwanted" ),
                              QStringLiteral( "prefixwanted" ) };
    QuickFindPattern quickFind;
    quickFind.changeSearchPattern( QStringLiteral( "(?:prefix)(wanted)" ), false, true, false );
    MatchSelectionView view{ &data, &quickFind };
    view.setSearchPattern( filter( QStringLiteral( "missing" ) ) );
    selectMatches( view, true );
    REQUIRE( view.getSelectedLinesText()
             == QStringList{ QStringLiteral( "prefixwanted" ), QStringLiteral( "prefixwanted" ),
                             QStringLiteral( "prefixwanted" ) } );
}

TEST_CASE( "A pending Quick Find result cannot replace all-match selection", "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "match match" ) };
    QuickFindPattern quickFind;
    quickFind.changeSearchPattern( QStringLiteral( "match" ) );
    MatchSelectionView view{ &data, &quickFind };
    view.searchForward();
    selectMatches( view, true );
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    REQUIRE( view.getSelectedLinesText()
             == QStringList{ QStringLiteral( "match" ), QStringLiteral( "match" ) } );
}

TEST_CASE( "Unsupported filter projection leaves the ordinary selection unchanged",
           "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "keep" ) };
    QuickFindPattern quickFind;
    MatchSelectionView view{ &data, &quickFind };
    view.selectAll();
    auto expression = filter( QStringLiteral( "keep" ) );
    SECTION( "Boolean" )
    {
        expression.isBoolean = true;
    }
    SECTION( "Inverse" )
    {
        expression.isExclude = true;
    }
    SECTION( "Invalid" )
    {
        expression.pattern = QStringLiteral( "[" );
    }
    SECTION( "Empty" )
    {
        expression.pattern.clear();
    }
    view.setSearchPattern( expression );
    REQUIRE_FALSE( view.canSelectAllMatches() );
    REQUIRE_FALSE( view.selectAllMatches() );
    REQUIRE( view.getSelectedText() == QStringLiteral( "keep" ) );
}

TEST_CASE( "Multi-fragment selections survive wrapped rendering and Escape clears them",
           "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "prefix [first] padding [second] trailing" ) };
    QuickFindPattern quickFind;
    MatchSelectionView view{ &data, &quickFind };
    view.matches = { 0_lnum };
    view.setSearchPattern( filter( QStringLiteral( R"(\[([^]]+)\])" ) ) );
    view.resize( 220, 160 );
    view.textWrapSet( true );
    view.show();
    selectMatches( view );
    REQUIRE_FALSE( view.grab().isNull() );
    REQUIRE( view.getSelectedLinesText()
             == QStringList{ QStringLiteral( "first" ), QStringLiteral( "second" ) } );
    QTest::keyClick( &view, Qt::Key_Escape );
    REQUIRE( view.getSelectedText().isEmpty() );
}

TEST_CASE( "Copy Matches and ordinary Copy share the selected text", "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "[a] [b]" ) };
    QuickFindPattern quickFind;
    MatchSelectionView view{ &data, &quickFind };
    view.matches = { 0_lnum };
    view.setSearchPattern( filter( QStringLiteral( R"(\[([^]]+)\])" ) ) );
    selectMatches( view, false, true );
    REQUIRE( QApplication::clipboard()->text() == view.getSelectedText() );
    REQUIRE( view.getSelectedLinesText()
             == QStringList{ QStringLiteral( "a" ), QStringLiteral( "b" ) } );
}

TEST_CASE( "Match selection cancellation never publishes partial fragments", "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "before" ), QStringLiteral( "match" ) };
    QuickFindPattern quickFind;
    MatchSelectionView view{ &data, &quickFind };
    view.matches = { 1_lnum };
    view.selectAndDisplayLine( 0_lnum );
    view.setSearchPattern( filter( QStringLiteral( "match" ) ) );
    SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
    REQUIRE( view.selectAllMatches() );
    view.cancelMatchSelection();
    REQUIRE( finished.safeWait() );
    REQUIRE_FALSE( finished.at( 0 ).at( 0 ).toBool() );
    REQUIRE( view.getSelectedText() == QStringLiteral( "before" ) );
}

TEST_CASE( "Match selection is a snapshot across append and is cancelled by replacement",
           "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "match" ) };
    QuickFindPattern quickFind;
    quickFind.changeSearchPattern( QStringLiteral( "match" ) );
    MatchSelectionView view{ &data, &quickFind };
    view.updateData();
    selectMatches( view, true );
    data.lines.append( QStringLiteral( "match" ) );
    view.updateData();
    REQUIRE( view.getSelectedLinesText() == QStringList{ QStringLiteral( "match" ) } );
    data.lines[ 0 ] = QStringLiteral( "other" );
    view.invalidateMatchSelection();
    REQUIRE( view.getSelectedText().isEmpty() );
}

TEST_CASE( "Large match selection yields and can cancel between batches", "[match-selection]" )
{
    MatchSelectionData data;
    for ( int i = 0; i < 4096; ++i ) {
        data.lines.append( QStringLiteral( "match" ) );
    }
    QuickFindPattern quickFind;
    quickFind.changeSearchPattern( QStringLiteral( "match" ) );
    MatchSelectionView view{ &data, &quickFind };
    data.reads = 0;
    quint64 processedRows = 0;
    bool yieldedBeforeCompletion = false;
    bool publishedPartialSelection = false;
    QObject::connect( &view, &AbstractLogView::matchSelectionProgress, &view,
                      [ & ]( quint64 processed, quint64 total ) {
                          if ( data.reads == 0 ) {
                              return;
                          }
                          processedRows = processed;
                          yieldedBeforeCompletion = processed < total;
                          publishedPartialSelection = !view.getSelectedText().isEmpty();
                          view.cancelMatchSelection();
                      } );
    SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
    REQUIRE( view.selectAllMatches( true ) );
    REQUIRE( finished.safeWait() );
    REQUIRE_FALSE( finished.at( 0 ).at( 0 ).toBool() );
    REQUIRE( yieldedBeforeCompletion );
    REQUIRE_FALSE( publishedPartialSelection );
    REQUIRE( data.reads > 0 );
    REQUIRE( processedRows < 4096 );
    REQUIRE( view.getSelectedText().isEmpty() );
}

TEST_CASE( "A single long line yields before all its occurrences are selected",
           "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "match " ).repeated( 8192 ) };
    QuickFindPattern quickFind;
    quickFind.changeSearchPattern( QStringLiteral( "match" ) );
    MatchSelectionView view{ &data, &quickFind };
    data.reads = 0;
    bool yieldedInsideLine = false;
    QObject::connect( &view, &AbstractLogView::matchSelectionProgress, &view,
                      [ & ]( quint64 processed, quint64 total ) {
                          if ( data.reads == 0 ) {
                              return;
                          }
                          yieldedInsideLine = processed == 0 && total == 1;
                          view.cancelMatchSelection();
                      } );
    SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
    REQUIRE( view.selectAllMatches( true ) );
    REQUIRE( finished.safeWait() );
    REQUIRE_FALSE( finished.at( 0 ).at( 0 ).toBool() );
    REQUIRE( yieldedInsideLine );
    REQUIRE( view.getSelectedText().isEmpty() );
}

TEST_CASE( "Ordinary selection cancels pending match selection", "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "match" ), QStringLiteral( "ordinary" ) };
    QuickFindPattern quickFind;
    quickFind.changeSearchPattern( QStringLiteral( "match" ) );
    MatchSelectionView view{ &data, &quickFind };
    SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
    REQUIRE( view.selectAllMatches( true ) );
    view.selectAndDisplayLine( 1_lnum );
    REQUIRE( finished.safeWait() );
    REQUIRE_FALSE( finished.at( 0 ).at( 0 ).toBool() );
    REQUIRE( view.getSelectedText() == QStringLiteral( "ordinary" ) );
}

TEST_CASE( "Hiding a view cancels pending match selection without taking focus back",
           "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "match" ) };
    QuickFindPattern quickFind;
    quickFind.changeSearchPattern( QStringLiteral( "match" ) );
    MatchSelectionView view{ &data, &quickFind };
    view.show();
    SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
    REQUIRE( view.selectAllMatches( true ) );
    view.hide();
    REQUIRE( finished.safeWait() );
    REQUIRE_FALSE( finished.at( 0 ).at( 0 ).toBool() );
    REQUIRE( view.getSelectedText().isEmpty() );
}

TEST_CASE( "Empty sources reject Copy Matches without changing the clipboard", "[match-selection]" )
{
    MatchSelectionData data;
    QuickFindPattern quickFind;
    quickFind.changeSearchPattern( QStringLiteral( "match" ) );
    MatchSelectionView view{ &data, &quickFind };
    QApplication::clipboard()->setText( QStringLiteral( "keep clipboard" ) );
    REQUIRE_FALSE( view.selectAllMatches( true, true ) );
    REQUIRE( QApplication::clipboard()->text() == QStringLiteral( "keep clipboard" ) );
}

TEST_CASE( "Changing search or source invalidates even a single match selection",
           "[match-selection]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "match" ) };
    QuickFindPattern quickFind;
    MatchSelectionView view{ &data, &quickFind };
    view.matches = { 0_lnum };
    view.setSearchPattern( filter( QStringLiteral( "match" ) ) );
    selectMatches( view );
    SECTION( "Search" )
    {
        view.setSearchPattern( filter( QStringLiteral( "other" ) ) );
    }
    SECTION( "Reordered source" )
    {
        view.invalidateMatchSelection();
    }
    SECTION( "Search range" )
    {
        view.setSearchLimits( 0_lnum, 0_lnum );
    }
    SECTION( "Truncation" )
    {
        data.lines.clear();
        view.updateData();
    }
    REQUIRE( view.getSelectedText().isEmpty() );
}

TEST_CASE( "Selecting and copying forty thousand fragments stays interactive",
           "[.perf][match-selection-perf]" )
{
    MatchSelectionData data;
    for ( int row = 0; row < 20000; ++row ) {
        data.lines.append( QStringLiteral( "prefix [value] and [value] suffix" ) );
    }
    QuickFindPattern quickFind;
    quickFind.changeSearchPattern( QStringLiteral( R"((?<=\[)[^]]+(?=\]))" ), false, true, false );
    MatchSelectionView view{ &data, &quickFind };
    qint64 bestSelectionMs = std::numeric_limits<qint64>::max();
    for ( int sample = 0; sample < 3; ++sample ) {
        QElapsedTimer elapsed;
        elapsed.start();
        selectMatches( view, true );
        const auto fragments = view.getSelectedLinesText();
        bestSelectionMs = std::min( bestSelectionMs, elapsed.elapsed() );
        REQUIRE( fragments.size() == 40000 );
        REQUIRE( fragments.first() == QStringLiteral( "value" ) );
        REQUIRE( fragments.last() == QStringLiteral( "value" ) );
    }
    INFO( "Best selection and copy time: " << bestSelectionMs << " ms" );
    KLOGG_CHECK_PERF_BUDGET( bestSelectionMs
                             < 2000 ); // lint-allow: perf-budget -- Completeness and cancellation
                                       // have deterministic coverage above.
}
