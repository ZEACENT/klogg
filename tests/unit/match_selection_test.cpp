#include <catch2/catch.hpp>

#include <algorithm>
#include <atomic>
#include <memory>

#include <QLineEdit>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QShortcut>
#include <QScrollBar>
#include <QTest>
#include <QSemaphore>
#include <QThreadPool>
#include "ownedtaskfuture.h"

#include "abstractlogview.h"
#include "containers.h"
#include "quickfindpattern.h"
#include "test_utils.h"

class MatchPaintWindowAccess;
template <>
struct AbstractLogView::access_by<MatchPaintWindowAccess> {
    static int cacheHeight( const AbstractLogView& view )
    {
        return static_cast<int>( view.textAreaCache_.pixmap_.height()
                                 / view.textAreaCache_.pixmap_.devicePixelRatioF() );
    }
    static uint64_t drawnUnits( const AbstractLogView& view )
    {
        return view.paintedTextUnitsForTest_;
    }
    static uint64_t paintWindowUnits( const AbstractLogView& view )
    {
        uint64_t widestRow = 0;
        for ( const auto& row : view.wrappedLinesInfo_ ) {
            widestRow = std::max( widestRow, static_cast<uint64_t>(
                row.wrappedString->wrappedLine( row.wrappedLineIndex ).size() ) );
        }
        // Use actual shaped row lengths: average font width is not a bound for
        // narrow glyphs. Include the partial row at either cache-window edge.
        const int height = std::max( 512, 2 * view.viewport()->height() );
        const auto rows = static_cast<uint64_t>( ( height + view.charHeight_ - 1 ) / view.charHeight_ + 1 );
        return widestRow * rows;
    }
    static std::shared_ptr<const WrappedString> layout( const AbstractLogView& view )
    {
        return view.wrappedLinesInfo_.empty() ? nullptr
            : view.wrappedLinesInfo_.front().wrappedString;
    }
    static FilePosition hit( const AbstractLogView& view, int yPosition )
    {
        return view.convertCoordToFilePos( QPoint{ view.leftMarginPx_ + 2, yPosition } );
    }
};

namespace {
class MatchSelectionData : public AbstractLogData {
public:
    QStringList lines;
    std::vector<LineNumber> matches;
    std::optional<std::vector<LineNumber>> layout;
    uint64_t incarnation = 1;
    QTextCodec* codec = QTextCodec::codecForName( "UTF-8" );
    std::optional<QString> readError;
    std::shared_ptr<std::atomic<size_t>> counter = std::make_shared<std::atomic<size_t>>( 0 );
    std::atomic<size_t>& reads = *counter;
    std::shared_ptr<std::atomic<size_t>> mappingReads = std::make_shared<std::atomic<size_t>>( 0 );

    class Snapshot final : public klogg::LogSnapshot {
    public:
        QStringList lines;
        std::optional<QString> readError;
        uint64_t incarnation = 1;
        std::vector<LineNumber> rows;
        std::shared_ptr<std::atomic<size_t>> counter;
        std::shared_ptr<std::atomic<size_t>> mappingReads;
        bool preservesPresentationOf( const klogg::LogSnapshot& previous ) const override
        {
            const auto* before = dynamic_cast<const Snapshot*>( &previous );
            return before && incarnation == before->incarnation
                && rows.size() >= before->rows.size()
                && lines.size() >= before->lines.size()
                && std::equal( before->rows.begin(), before->rows.end(), rows.begin() )
                && std::all_of( rows.begin() + static_cast<std::ptrdiff_t>( before->rows.size() ),
                                rows.end(), [ & ]( LineNumber row ) {
                    return row.get() >= static_cast<uint64_t>( before->lines.size() );
                } )
                && std::equal( before->lines.begin(), before->lines.end(), lines.begin() );
        }
        LinesCount lineCount() const override { return LinesCount{ rows.size() }; }
        klogg::SnapshotRow row( LineNumber index ) const override
        {
            const auto source = rows.at( static_cast<size_t>( index.get() ) );
            return { { klogg::LogRowId::Origin::Memory, incarnation, 0, source.get() },
                     source + 1_lcount, true, true };
        }
        QString lineString( LineNumber index ) const override
        {
            ++*counter;
            if ( readError ) {
                throw klogg::SnapshotError{ readError->toStdString() };
            }
            return lines.at( static_cast<klogg::ContainerIndex>(
                rows.at( static_cast<size_t>( index.get() ) ).get() ) );
        }
        OptionalLineNumber presentedRowForIdentity( const klogg::LogRowId& id ) const override
        {
            ++*mappingReads;
            const auto found = std::find( rows.begin(), rows.end(), LineNumber{ id.row } );
            return id.incarnation == incarnation && found != rows.end()
                ? OptionalLineNumber{ LineNumber{ static_cast<uint64_t>( found - rows.begin() ) } }
                : OptionalLineNumber{};
        }
    };
    klogg::LogSnapshot::Ptr createReadSnapshot( klogg::SnapshotScope scope ) const override
    {
        auto snapshot = std::make_shared<Snapshot>();
        snapshot->lines = lines;
        snapshot->readError = readError;
        snapshot->incarnation = incarnation;
        snapshot->counter = counter;
        snapshot->mappingReads = mappingReads;
        if ( scope == klogg::SnapshotScope::Matches ) {
            for ( const auto row : matches ) {
                if ( row.get() < static_cast<uint64_t>( lines.size() ) ) {
                    snapshot->rows.push_back( row );
                }
            }
        }
        else if ( layout ) {
            snapshot->rows = *layout;
        }
        else {
            for ( uint64_t row = 0; row < static_cast<uint64_t>( lines.size() ); ++row ) {
                snapshot->rows.push_back( LineNumber{ row } );
            }
        }
        return snapshot;
    }
    OptionalLineNumber rowForIdentity( const klogg::LogRowId& id ) const override
    {
        if ( !containsRowIdentity( id ) ) {
            return {};
        }
        if ( layout ) {
            const auto found = std::find( layout->begin(), layout->end(), LineNumber{ id.row } );
            return found != layout->end()
                ? OptionalLineNumber{ LineNumber{ static_cast<uint64_t>( found - layout->begin() ) } }
                : OptionalLineNumber{};
        }
        return LineNumber{ id.row };
    }
    bool containsRowIdentity( const klogg::LogRowId& id ) const override
    {
        return id.incarnation == incarnation && id.row < static_cast<uint64_t>( lines.size() );
    }

protected:
    QString doGetLineString( LineNumber line ) const override
    {
        ++reads;
        const auto source = layout ? layout->at( static_cast<size_t>( line.get() ) ) : line;
        return lines.value( static_cast<klogg::ContainerIndex>( source.get() ) );
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
        return LinesCount{ layout ? layout->size() : static_cast<size_t>( lines.size() ) };
    }
    LineLength doGetMaxLength() const override
    {
        return 200_length;
    }
    LineLength doGetLineLength( LineNumber line ) const override
    {
        return LineLength{ doGetExpandedLineString( line ).size() };
    }
    void doSetDisplayEncoding( const char* name ) override { codec = QTextCodec::codecForName( name ); }
    QTextCodec* doGetDisplayEncoding() const override { return codec; }
    void doAttachReader() const override {}
    void doDetachReader() const override {}
};

class MatchSelectionView : public AbstractLogView {
public:
    MatchSelectionView( MatchSelectionData* logData, const QuickFindPattern* pattern )
        : AbstractLogView( logData, pattern ), matches( logData->matches )
    {
        HighlighterSetCollection::getSynced();
    }
    std::vector<LineNumber>& matches;
    QPixmap renderFreshViewport()
    {
        forceRefresh();
        return viewport()->grab();
    }

protected:
    AbstractLogData::LineType lineType( LineNumber line ) const override
    {
        return std::find( matches.begin(), matches.end(), line ) != matches.end()
                   ? AbstractLogData::LineTypeFlags::Match
                   : AbstractLogData::LineTypeFlags::Mark;
    }
};

struct WorkerGate {
    QThreadPool* pool = QThreadPool::globalInstance();
    int previousThreads = pool->maxThreadCount();
    QSemaphore entered;
    QSemaphore release;
    klogg::OwnedTaskFuture<void> worker;
    WorkerGate()
    {
        pool->setMaxThreadCount( 1 );
        worker = klogg::runOwnedTask( [ this ] {
            entered.release();
            release.acquire();
        } );
    }
    ~WorkerGate()
    {
        release.release();
        worker.waitForFinished();
        pool->waitForDone();
        pool->setMaxThreadCount( previousThreads );
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

TEST_CASE( "Extraction read failures terminate even when their diagnostic is empty",
           "[match-selection][match-executor-error]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "keep" ) };
    data.readError = GENERATE( QString{}, QStringLiteral( "read failed" ) );
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "keep" ) );
    MatchSelectionView view{ &data, &pattern };
    view.selectAll();
    QApplication::clipboard()->setText( QStringLiteral( "untouched" ) );
    data.reads = 0;
    QObject::connect( &view, &AbstractLogView::matchSelectionProgress, &view,
                      [ & ]( quint64, quint64 ) {
                          if ( data.reads.load() >= 2 ) view.cancelMatchSelection();
                      } );
    SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
    REQUIRE( view.selectAllMatches( true, true ) );
    REQUIRE( finished.safeWait() );
    REQUIRE_FALSE( finished.at( 0 ).at( 0 ).toBool() );
    REQUIRE( data.reads.load() == 1 );
    REQUIRE( view.getSelectedText() == QStringLiteral( "keep" ) );
    REQUIRE( QApplication::clipboard()->text() == QStringLiteral( "untouched" ) );
}

TEST_CASE( "Proven append-only presentation avoids remapping every captured row",
           "[match-selection][match-layout-work]" )
{
    MatchSelectionData data;
    for ( int row = 0; row < 100; ++row ) {
        data.lines.append( QStringLiteral( "match" ) );
    }
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "match" ) );
    MatchSelectionView view{ &data, &pattern };
    selectMatches( view, true );
    data.mappingReads->store( 0 );
    data.lines.append( QStringLiteral( "later match" ) );
    view.updateData();
    REQUIRE( data.mappingReads->load() == 0 );
    REQUIRE( view.getSelectedLinesText().size() == 100 );
}

TEST_CASE( "Match selection remaps presentation without replacing its semantic payload",
           "[match-selection][match-lifecycle]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "selected" ), QStringLiteral( "other" ) };
    data.matches = { 0_lnum };
    QuickFindPattern pattern;
    MatchSelectionView view{ &data, &pattern };
    view.setSearchPattern( filter( QStringLiteral( "selected" ) ) );
    selectMatches( view );
    SECTION( "Reorder" ) { data.layout = std::vector<LineNumber>{ 1_lnum, 0_lnum }; }
    SECTION( "Hidden" ) { data.layout = std::vector<LineNumber>{}; }
    view.updateData();
    REQUIRE( view.getSelectedLinesText() == QStringList{ QStringLiteral( "selected" ) } );
    std::vector<LineNumber> marked;
    QObject::connect( &view, &AbstractLogView::markLines, &view,
                      [ & ]( const klogg::vector<LineNumber>& rows ) {
                          marked.assign( rows.begin(), rows.end() );
                      } );
    view.markSelected();
    REQUIRE( marked == ( data.layout->empty() ? std::vector<LineNumber>{}
                                             : std::vector<LineNumber>{ 1_lnum } ) );
}

TEST_CASE( "Source incarnation and interpretation changes retire captured selection and copy",
           "[match-selection][match-lifecycle]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "selected" ) };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "selected" ) );
    MatchSelectionView view{ &data, &pattern };
    selectMatches( view, true );
    QApplication::clipboard()->setText( QStringLiteral( "unchanged" ) );
    SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
    REQUIRE( view.deferCopyUntilMatchSelection() );
    SECTION( "Same-sized replacement" ) {
        ++data.incarnation;
        data.lines[ 0 ] = QStringLiteral( "replaced" );
        view.updateData();
    }
    SECTION( "Encoding" ) { data.setDisplayEncoding( "ISO-8859-1" ); }
    REQUIRE( view.getSelectedText().isEmpty() );
    REQUIRE( finished.safeWait() );
    REQUIRE_FALSE( finished.at( 0 ).at( 0 ).toBool() );
    REQUIRE( QApplication::clipboard()->text() == QStringLiteral( "unchanged" ) );
}

TEST_CASE( "Multiple captured fragments cannot become one search operand",
           "[match-selection][match-operand-actions]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "[one] [two]" ) };
    data.matches = { 0_lnum };
    QuickFindPattern pattern;
    MatchSelectionView view{ &data, &pattern };
    view.setSearchPattern( filter( QStringLiteral( R"(\[([^]]+)\])" ) ) );
    selectMatches( view );
    int operands = 0;
    QObject::connect( &view, qOverload<const QString&>( &AbstractLogView::replaceSearch ),
                      &view, [ & ]( const QString& ) { ++operands; } );
    QObject::connect( &view, qOverload<const QString&>( &AbstractLogView::addToSearch ),
                      &view, [ & ]( const QString& ) { ++operands; } );
    QObject::connect( &view, qOverload<const QString&>( &AbstractLogView::excludeFromSearch ),
                      &view, [ & ]( const QString& ) { ++operands; } );
    for ( const auto* action : { "replaceSearch", "addToSearch", "excludeFromSearch" } ) {
        REQUIRE( QMetaObject::invokeMethod( &view, action, Qt::DirectConnection ) );
    }
    REQUIRE( operands == 0 );
}

TEST_CASE( "Quick Find follows a view source replacement",
           "[match-selection][match-source-switch]" )
{
    MatchSelectionData first;
    first.lines = QStringList{ QStringLiteral( "old" ) };
    MatchSelectionData second;
    second.lines = QStringList{ QStringLiteral( "new" ) };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "new" ) );
    MatchSelectionView view{ &first, &pattern };
    view.setDataSource( &second );
    SafeQSignalSpy selected{ &view, &AbstractLogView::newSelection };
    view.searchForward();
    REQUIRE( selected.safeWait() );
    REQUIRE( view.getSelectedText() == QStringLiteral( "new" ) );
}

TEST_CASE( "Match extraction owns the accepted content before asynchronous work starts",
           "[match-selection][match-executor]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "[accepted]" ) };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "\\[([^]]+)\\]" ), false, true, false );
    MatchSelectionView view{ &data, &pattern };
    SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
    REQUIRE( view.selectAllMatches( true ) );
    data.lines[ 0 ] = QStringLiteral( "[replaced]" );
    REQUIRE( finished.safeWait() );
    REQUIRE( finished.at( 0 ).at( 0 ).toBool() );
    REQUIRE( view.getSelectedLinesText() == QStringList{ QStringLiteral( "accepted" ) } );
}

TEST_CASE( "Completed match extraction does not steal newer input focus",
           "[match-selection][match-executor]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "match" ) };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "match" ) );
    QWidget window;
    MatchSelectionView view{ &data, &pattern };
    view.setParent( &window );
    QLineEdit input{ &window };
    window.show();
    window.activateWindow();
    view.show();
    input.show();
    QCoreApplication::processEvents();
    SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
    REQUIRE( view.selectAllMatches( true ) );
    input.setFocus();
    REQUIRE( input.hasFocus() );
    QTest::keyClicks( &input, "typed during extraction" );
    REQUIRE( finished.safeWait() );
    REQUIRE( input.hasFocus() );
    REQUIRE( input.text() == QStringLiteral( "typed during extraction" ) );
    view.setParent( nullptr );
}

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
    REQUIRE_FALSE( view.canUseSelectionAsSearchOperand() );
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

TEST_CASE( "Quick Find selection shares projection independently of filter classification",
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
             == QStringList{ QStringLiteral( "wanted" ), QStringLiteral( "wanted" ),
                             QStringLiteral( "wanted" ) } );
}

TEST_CASE( "Quick Find navigation selects projected captures of one occurrence",
           "[match-selection][quickfind-occurrence]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "prefixwantedsuffix" ) };
    QuickFindPattern quickFind;
    quickFind.changeSearchPattern( QStringLiteral( "(prefix)(wanted)suffix" ), false, true, false );
    MatchSelectionView view{ &data, &quickFind };
    SafeQSignalSpy selected{ &view, &AbstractLogView::newSelection };
    view.searchForward();
    REQUIRE( selected.safeWait() );
    REQUIRE( view.getSelectedLinesText()
             == QStringList{ QStringLiteral( "prefix" ), QStringLiteral( "wanted" ) } );
}

TEST_CASE( "Quick Find navigates occurrences rather than projected fragment ends",
           "[match-selection][quickfind-occurrence]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "aaa" ) };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "(?=(aa))" ), false, true, false );
    QuickFind finder{ data };
    SafeQSignalSpy completed{ &finder, &QuickFind::searchDone };
    Selection cursor;
    for ( const auto start : { 0_lcol, 1_lcol } ) {
        finder.searchForward( cursor, pattern.getMatcher() );
        REQUIRE( completed.safeWait() );
        const auto result = qvariant_cast<QuickFindResult>( completed.takeFirst().at( 0 ) );
        REQUIRE( result.found );
        REQUIRE( result.selection.getSelectedText( nullptr ) == QStringLiteral( "aa" ) );
        REQUIRE( result.selection.getPreviousPosition() == FilePosition{ 0_lnum, start } );
        cursor = result.selection;
    }
    finder.searchForward( cursor, pattern.getMatcher() );
    REQUIRE( completed.safeWait() );
    REQUIRE_FALSE( qvariant_cast<QuickFindResult>( completed.takeFirst().at( 0 ) ).found );

    finder.searchBackward( cursor, pattern.getMatcher() );
    REQUIRE( completed.safeWait() );
    const auto previous = qvariant_cast<QuickFindResult>( completed.takeFirst().at( 0 ) );
    REQUIRE( previous.found );
    REQUIRE( previous.selection.getPreviousPosition() == FilePosition{ 0_lnum, 0_lcol } );
}

TEST_CASE( "Quick Find occurrence navigation survives presentation remapping",
           "[match-selection][quickfind-layout]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "abcd" ), QStringLiteral( "xy" ),
                              QStringLiteral( "last" ) };
    QuickFindPattern pattern;
    const bool zeroWidth = GENERATE( false, true );
    pattern.changeSearchPattern( zeroWidth ? QStringLiteral( "$" )
                                          : QStringLiteral( "(?=(..))" ), false, true, false );
    MatchSelectionView view{ &data, &pattern };
    SafeQSignalSpy selected{ &view, &AbstractLogView::newSelection };
    view.searchForward();
    REQUIRE( selected.safeWait() );
    REQUIRE( view.hasMatchSelection() );
    selected.clear();

    data.layout = std::vector<LineNumber>{};
    view.updateData();
    REQUIRE( view.hasMatchSelection() );
    data.layout = std::vector<LineNumber>{ 1_lnum, 0_lnum, 2_lnum };
    view.updateData();
    view.searchForward();
    REQUIRE( selected.safeWait() );
    if ( zeroWidth ) {
        REQUIRE( view.getSelectedText().isEmpty() );
        REQUIRE( selected.at( 0 ).at( 0 ).value<LineNumber>() == 2_lnum );
    }
    else {
        REQUIRE( view.getSelectedText() == QStringLiteral( "bc" ) );
        REQUIRE( selected.at( 0 ).at( 0 ).value<LineNumber>() == 1_lnum );
    }
}

TEST_CASE( "Quick Find restoration validates captured source and presentation state",
           "[match-selection][quickfind-restoration]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "selected" ), QStringLiteral( "other" ) };
    data.matches = { 0_lnum };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "missing" ) );
    MatchSelectionView view{ &data, &pattern };
    view.setSearchPattern( filter( QStringLiteral( "selected" ) ) );
    selectMatches( view );
    SafeQSignalSpy selected{ &view, &AbstractLogView::newSelection };
    view.incrementallySearchForward();
    REQUIRE( selected.safeWait() );

    bool retired = false;
    SECTION( "A layout change remaps the saved selection" ) {
        data.layout = std::vector<LineNumber>{ 1_lnum, 0_lnum };
        view.updateData();
    }
    SECTION( "Pattern reset routing cannot erase the restored selection" ) {
        data.layout = std::vector<LineNumber>{ 1_lnum, 0_lnum };
        view.updateData();
        QObject::connect( &view, &AbstractLogView::changeQuickFind, &pattern,
            [ &pattern ]( const QString& text, QuickFindMux::QFDirection ) {
                pattern.changeSearchPattern( text );
            } );
    }
    SECTION( "A replaced source cannot resurrect an old selection" ) {
        ++data.incarnation;
        data.lines[ 0 ] = QStringLiteral( "replacement" );
        view.updateData();
        retired = true;
    }
    SECTION( "Changed interpretation retires the saved selection too" ) {
        data.setDisplayEncoding( "ISO-8859-1" );
        retired = true;
    }
    SECTION( "A new Filter query retires the saved selection too" ) {
        view.setSearchPattern( filter( QStringLiteral( "other" ) ) );
        retired = true;
    }
    view.incrementalSearchAbort();
    if ( retired ) {
        REQUIRE( view.getSelectedText().isEmpty() );
        REQUIRE_FALSE( view.hasMatchSelection() );
    }
    else {
        REQUIRE( view.getSelectedText() == QStringLiteral( "selected" ) );
        std::vector<LineNumber> marked;
        QObject::connect( &view, &AbstractLogView::markLines, &view,
            [ & ]( const klogg::vector<LineNumber>& rows ) {
                marked.assign( rows.begin(), rows.end() );
            } );
        view.markSelected();
        REQUIRE( marked == std::vector<LineNumber>{ 1_lnum } );
    }
}

TEST_CASE( "Stopping Quick Find preserves a zero-fragment occurrence anchor",
           "[match-selection][quickfind-restoration]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "abc" ), QStringLiteral( "def" ) };
    data.matches = { 0_lnum };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "$" ), false, true, false );
    MatchSelectionView view{ &data, &pattern };
    view.setSearchPattern( filter( QStringLiteral( "abc" ) ) );
    selectMatches( view );
    SafeQSignalSpy selected{ &view, &AbstractLogView::newSelection };
    view.incrementallySearchForward();
    REQUIRE( selected.safeWait() );
    REQUIRE( view.getSelectedText().isEmpty() );
    view.incrementalSearchStop();
    REQUIRE( view.getSelectedText().isEmpty() );
    REQUIRE( view.hasMatchSelection() );
    selected.clear();
    view.searchForward();
    REQUIRE( selected.safeWait() );
    REQUIRE( selected.at( 0 ).at( 0 ).value<LineNumber>() == 1_lnum );
}

TEST_CASE( "Quick Find zero-width occurrences advance without selecting fake text",
           "[match-selection][quickfind-occurrence]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "value" ) };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "$" ), false, true, false );
    QuickFind finder{ data };
    SafeQSignalSpy completed{ &finder, &QuickFind::searchDone };
    finder.searchForward( Selection{}, pattern.getMatcher() );
    REQUIRE( completed.safeWait() );
    const auto result = qvariant_cast<QuickFindResult>( completed.takeFirst().at( 0 ) );
    REQUIRE( result.found );
    REQUIRE( result.selection.isEmpty() );
    REQUIRE( result.selection.getSelectedText( nullptr ).isEmpty() );
    REQUIRE( result.selection.getNextPosition() == FilePosition{ 1_lnum, 0_lcol } );
    finder.searchForward( result.selection, pattern.getMatcher() );
    REQUIRE( completed.safeWait() );
    REQUIRE_FALSE( qvariant_cast<QuickFindResult>( completed.takeFirst().at( 0 ) ).found );
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

TEST_CASE( "End of line navigation with empty match selections is safe",
           "[match-selection][empty-navigation]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "match" ) };
    QuickFindPattern quickFind;
    MatchSelectionView view{ &data, &quickFind };
    view.matches = { 0_lnum };
    view.setSearchPattern( filter( QStringLiteral( "match" ) ) );
    view.show();
    view.activateWindow();
    view.setFocus();
    QCoreApplication::processEvents();
    SECTION( "Escape clears a captured selection" ) {
        selectMatches( view );
        QTest::keyClick( &view, Qt::Key_Escape );
    }
    SECTION( "Batch extraction has zero projected fragments" ) {
        view.setSearchPattern( filter( QStringLiteral( "()" ) ) );
        selectMatches( view );
    }
    SECTION( "Quick Find has a zero-fragment occurrence anchor" ) {
        quickFind.changeSearchPattern( QStringLiteral( "$" ), false, true, false );
        SafeQSignalSpy selected{ &view, &AbstractLogView::newSelection };
        view.searchForward();
        REQUIRE( selected.safeWait() );
        REQUIRE( view.hasMatchSelection() );
    }
    REQUIRE( view.getSelectedText().isEmpty() );
    REQUIRE( view.hasFocus() );
    view.registerShortcuts();
    const auto previousScroll = view.horizontalScrollBar()->value();
    bool invoked = false;
    for ( auto* shortcut : view.findChildren<QShortcut*>() ) {
        if ( shortcut->key() == QKeySequence( Qt::Key_Dollar ) ) {
            REQUIRE( QMetaObject::invokeMethod( shortcut, "activated", Qt::DirectConnection ) );
            invoked = true;
            break;
        }
    }
    REQUIRE( invoked );
    REQUIRE( view.getSelectedText().isEmpty() );
    REQUIRE( view.horizontalScrollBar()->value() == previousScroll );
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

TEST_CASE( "A newer numbered-copy command supersedes an already dispatched format",
           "[match-selection][match-copy-format]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "match" ) };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "match" ) );
    QWidget window;
    MatchSelectionView view{ &data, &pattern };
    view.setParent( &window );
    QLineEdit input{ &window };
    window.show();
    window.activateWindow();
    view.show();
    input.show();
    QCoreApplication::processEvents();
    selectMatches( view, true );

    WorkerGate gate;
    REQUIRE( gate.entered.tryAcquire( 1, 5000 ) );
    const bool numberedFirst = GENERATE( false, true );
    SafeQSignalSpy copied{ &view, &AbstractLogView::matchSelectionFinished };
    REQUIRE( QMetaObject::invokeMethod( &view, numberedFirst ? "copyWithLineNumbers" : "copy",
                                        Qt::DirectConnection ) );
    // Dispatch the zero-delay continuation while the sole pool worker is held.
    // Its serialization policy is captured, but no serialization can complete.
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    REQUIRE( copied.count() == 0 );
    REQUIRE( QMetaObject::invokeMethod( &view, numberedFirst ? "copy" : "copyWithLineNumbers",
                                        Qt::DirectConnection ) );
    input.setFocus();
    REQUIRE( input.hasFocus() );
    QTest::keyClicks( &input, "typed during serialization" );
    gate.release.release();
    REQUIRE( copied.safeWait() );
    REQUIRE( input.hasFocus() );
    REQUIRE( input.text() == QStringLiteral( "typed during serialization" ) );
    REQUIRE( copied.count() == 1 );
    REQUIRE( copied.at( 0 ).at( 0 ).toBool() );
    REQUIRE( QApplication::clipboard()->text()
             == ( numberedFirst ? QStringLiteral( "match" ) : QStringLiteral( "1: match" ) ) );
}

TEST_CASE( "Retiring a view drains dispatched work without late copy or retained leases",
           "[match-selection][match-lifetime]" )
{
    struct LeasedData final : MatchSelectionData {
        mutable std::vector<std::weak_ptr<const klogg::LogSnapshot>> snapshots;
        klogg::LogSnapshot::Ptr createReadSnapshot( klogg::SnapshotScope scope ) const override
        {
            auto snapshot = MatchSelectionData::createReadSnapshot( scope );
            snapshots.emplace_back( snapshot );
            return snapshot;
        }
    } data;
    data.lines = QStringList{ QStringLiteral( "match" ) };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "match" ) );
    auto view = std::make_unique<MatchSelectionView>( &data, &pattern );
    view->show();
    QCoreApplication::processEvents();
    const bool serializing = GENERATE( false, true );
    if ( serializing ) {
        selectMatches( *view, true );
    }
    WorkerGate gate;
    REQUIRE( gate.entered.tryAcquire( 1, 5000 ) );
    QApplication::clipboard()->setText( QStringLiteral( "keep clipboard" ) );
    SafeQSignalSpy finished{ view.get(), &AbstractLogView::matchSelectionFinished };
    REQUIRE( ( serializing ? view->deferCopyUntilMatchSelection()
                           : view->selectAllMatches( true, true ) ) );
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    REQUIRE( finished.count() == 0 );
    SECTION( "Hide cancels a dispatched operation" ) {
        view->hide();
        REQUIRE( finished.count() == 1 );
        REQUIRE_FALSE( finished.at( 0 ).at( 0 ).toBool() );
        gate.release.release();
        view->stopSearchAndWait();
    }
    SECTION( "Destruction joins work before queued publication" ) {
        gate.release.release();
        view.reset();
    }
    gate.pool->waitForDone();
    QCoreApplication::processEvents();
    REQUIRE( QApplication::clipboard()->text() == QStringLiteral( "keep clipboard" ) );
    REQUIRE_FALSE( data.snapshots.empty() );
    for ( const auto& snapshot : data.snapshots ) {
        REQUIRE( snapshot.expired() );
    }
}

TEST_CASE( "Copy of captured matches can be cancelled without source reads or clipboard changes",
           "[match-selection][match-executor]" )
{
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "match match" ) };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "match" ) );
    MatchSelectionView view{ &data, &pattern };
    selectMatches( view, true );
    const auto reads = data.reads.load();
    QApplication::clipboard()->setText( QStringLiteral( "keep clipboard" ) );
    SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
    REQUIRE( view.deferCopyUntilMatchSelection() );
    view.cancelMatchSelection();
    REQUIRE( finished.safeWait() );
    REQUIRE_FALSE( finished.at( 0 ).at( 0 ).toBool() );
    REQUIRE( QApplication::clipboard()->text() == QStringLiteral( "keep clipboard" ) );
    REQUIRE( data.reads.load() == reads );
    REQUIRE( view.getSelectedLinesText().size() == 2 );
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

TEST_CASE( "X11 immediate deferred and direct match copy share clipboard policy",
           "[.x11][match-selection-x11]" )
{
    REQUIRE( QGuiApplication::platformName() == QStringLiteral( "xcb" ) );
    auto* clipboard = QApplication::clipboard();
    REQUIRE( clipboard->supportsSelection() );
    MatchSelectionData data;
    data.lines = QStringList{ QStringLiteral( "[one] [two]" ) };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( R"(\[([^]]+)\])" ), false, true, false );
    MatchSelectionView view{ &data, &pattern };
    clipboard->setText( QStringLiteral( "previous clipboard" ), QClipboard::Clipboard );
    clipboard->setText( QStringLiteral( "previous primary" ), QClipboard::Selection );
    SECTION( "Copy Matches" ) {
        selectMatches( view, true, true );
    }
    SECTION( "Copy after selection is ready" ) {
        selectMatches( view, true );
        SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
        REQUIRE( QMetaObject::invokeMethod( &view, "copy", Qt::DirectConnection ) );
        REQUIRE( finished.safeWait() );
        REQUIRE( finished.at( 0 ).at( 0 ).toBool() );
    }
    SECTION( "Copy while extraction is pending" ) {
        SafeQSignalSpy finished{ &view, &AbstractLogView::matchSelectionFinished };
        REQUIRE( view.selectAllMatches( true ) );
        REQUIRE( view.deferCopyUntilMatchSelection() );
        REQUIRE( finished.safeWait() );
        REQUIRE( finished.at( 0 ).at( 0 ).toBool() );
    }
    REQUIRE( clipboard->text( QClipboard::Clipboard ) == QStringLiteral( "one\ntwo" ) );
    REQUIRE( clipboard->text( QClipboard::Selection ) == QStringLiteral( "one\ntwo" ) );
}

TEST_CASE( "Wrapped painting bounds pixels and text work while retaining exact layout",
           "[match-selection][match-paint-window]" )
{
    using Access = AbstractLogView::access_by<MatchPaintWindowAccess>;
    class DenseData final : public MatchSelectionData {
        LineLength doGetMaxLength() const override { return LineLength{ lines.front().size() }; }
    } data;
    data.lines = QStringList{ QStringLiteral( "x " ).repeated( 8192 ) };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "x" ) );
    MatchSelectionView view{ &data, &pattern };
    view.resize( 720, 240 );
    view.show();
    selectMatches( view, true );
    view.textWrapSet( true );
    QCoreApplication::processEvents();
    REQUIRE( view.verticalScrollBar()->maximum() > 0 );
    for ( const bool bottom : { false, true } ) {
        view.verticalScrollBar()->setValue( bottom ? view.verticalScrollBar()->maximum() : 0 );
        REQUIRE_FALSE( view.renderFreshViewport().isNull() );
        CHECK( Access::cacheHeight( view ) <= std::max( 512, 2 * view.viewport()->height() ) + 1 );
        CHECK( Access::drawnUnits( view ) > 0 );
        CHECK( Access::drawnUnits( view ) <= Access::paintWindowUnits( view ) );
        const auto position = Access::hit( view, view.viewport()->height() / 2 );
        CHECK( position.line() == 0_lnum );
        CHECK( ( position.column().get() > 8192 ) == bottom );
        const auto layout = Access::layout( view );
        REQUIRE( layout );
        REQUIRE_FALSE( view.renderFreshViewport().isNull() );
        CHECK( Access::layout( view ) == layout );
        CHECK( view.getSelectedLinesText().size() == 8192 );
    }
    view.ensureLineMapFresh();
    REQUIRE( Access::hit( view, view.viewport()->height() / 2 ).column().get() > 8192 );
    view.verticalScrollBar()->setValue( 0 );
    view.ensureLineMapFresh();
    CHECK( Access::hit( view, view.viewport()->height() / 2 ).column().get() < 8192 );
    const auto formerLayout = Access::layout( view );
    data.lines[ 0 ] = QStringLiteral( "M " ).repeated( 8192 );
    ++data.incarnation;
    view.updateData();
    view.ensureLineMapFresh();
    CHECK( Access::layout( view ) != formerLayout );
    REQUIRE_FALSE( view.renderFreshViewport().isNull() );
    CHECK( Access::layout( view ) != formerLayout );
    const auto changedLayout = Access::layout( view );
    view.resize( 600, 240 );
    REQUIRE_FALSE( view.renderFreshViewport().isNull() );
    CHECK( Access::layout( view ) != changedLayout );
}

TEST_CASE( "Dense match painting measures warmed frames without changing fragments",
           "[.perf][match-paint-perf]" )
{
    class DenseData final : public MatchSelectionData {
        LineLength doGetMaxLength() const override
        {
            return LineLength{ lines.front().size() };
        }
    } data;
    data.lines = QStringList{ QStringLiteral( "x " ).repeated( 40000 ) };
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "x" ) );
    MatchSelectionView view{ &data, &pattern };
    view.resize( 720, 400 );
    view.show();
    selectMatches( view, true );
    REQUIRE( view.getSelectedLinesText().size() == 40000 );
    for ( const bool wrapped : { false, true } ) {
        view.textWrapSet( wrapped );
        QCoreApplication::processEvents();
        REQUIRE_FALSE( view.renderFreshViewport().isNull() );
        qint64 bestPaintNs = std::numeric_limits<qint64>::max();
        for ( int sample = 0; sample < 3; ++sample ) {
            QElapsedTimer elapsed;
            elapsed.start();
            const auto image = view.renderFreshViewport();
            bestPaintNs = std::min( bestPaintNs, elapsed.nsecsElapsed() );
            REQUIRE_FALSE( image.isNull() );
        }
        INFO( "Dense paint wrapped=" << wrapped << ": " << bestPaintNs << " ns" );
        REQUIRE( view.getSelectedLinesText().size() == 40000 );
    }
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
        const auto readsBeforeCopy = data.reads.load();
        SafeQSignalSpy copied{ &view, &AbstractLogView::matchSelectionFinished };
        REQUIRE( view.deferCopyUntilMatchSelection() );
        REQUIRE( copied.safeWait() );
        REQUIRE( copied.at( 0 ).at( 0 ).toBool() );
        bestSelectionMs = std::min( bestSelectionMs, elapsed.elapsed() );
        REQUIRE( data.reads.load() == readsBeforeCopy );
        REQUIRE( QApplication::clipboard()->text() == view.getSelectedText() );
        REQUIRE( fragments.size() == 40000 );
        REQUIRE( fragments.first() == QStringLiteral( "value" ) );
        REQUIRE( fragments.last() == QStringLiteral( "value" ) );
    }
    INFO( "Best selection and copy time: " << bestSelectionMs << " ms" );
    KLOGG_CHECK_PERF_BUDGET( bestSelectionMs
                             < 2000 ); // lint-allow: perf-budget -- Completeness and cancellation
                                       // have deterministic coverage above.
}
