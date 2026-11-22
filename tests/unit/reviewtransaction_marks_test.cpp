#include <catch2/catch.hpp>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWidget>

#include "configuration.h"
#include "foldercrawlerwidget.h"
#include "foldersearchengine.h"
#include "logfiltereddata.h"
#include "reviewtransaction.h"
#include "streaminglogdata.h"
#include "test_utils.h"

namespace {
using Status = ReviewTransaction::Status;
using MarkEdit = ReviewTransaction::MarkEdit;
using Kind = MarkEdit::Kind;

struct Fixture {
    QTemporaryDir root;
    std::shared_ptr<StreamingLogData> source;
    std::unique_ptr<LogFilteredData> marks;
    QWidget parent;
    ColorLabelsController labels{ &parent, {} };
    Fixture()
    {
        REQUIRE( root.isValid() );
        source = std::make_shared<StreamingLogData>( QUuid::createUuid().toString(), root.path() );
        source->appendUtf8( "a\nb\nc\n" );
        marks = source->getNewFilteredData();
    }
    klogg::LogRowId identity( LineNumber row ) const
    {
        return source->createReadSnapshot( klogg::SnapshotScope::PresentedRows )->row( row ).identity;
    }
    static ColorLabelsController::Edit label()
    {
        return { ColorLabelsController::Edit::Kind::Set, 0, { "evidence" }, {} };
    }
};

void waitPreparation( const ReviewTransaction& transaction )
{
    const auto future = transaction.preparationFuture();
    REQUIRE( future );
    REQUIRE( waitUiState( [&] { return future->isFinished(); } ) );
}
}

TEST_CASE( "Mark transactions read their staged source-backed edits without changing native owners",
           "[review-transaction][review-mark-writes]" )
{
    Fixture fixture;
    ReviewTransaction transaction;
    const auto first = fixture.identity( 0_lnum );
    const auto second = fixture.identity( 1_lnum );
    REQUIRE( transaction.stageMarks( *fixture.marks, { Kind::Add, { first, second } } ) == Status::Open );
    REQUIRE( transaction.marks( *fixture.marks )->groups.size() == 1 );
    CHECK( transaction.marks( *fixture.marks )->groups.front().rows == QList<LineNumber>{ 0_lnum, 1_lnum } );
    CHECK( fixture.marks->getNbMarks() == 0_lcount );
    REQUIRE( transaction.stageMarks( *fixture.marks, { Kind::Remove, { first } } ) == Status::Open );
    CHECK( transaction.marks( *fixture.marks )->groups.front().rows == QList<LineNumber>{ 1_lnum } );
    REQUIRE( transaction.stageMarks( *fixture.marks, { Kind::Replace, { first } } ) == Status::Open );
    CHECK( transaction.marks( *fixture.marks )->groups.front().rows == QList<LineNumber>{ 0_lnum } );
    REQUIRE( transaction.stageMarks( *fixture.marks, { Kind::Clear, {} } ) == Status::Open );
    CHECK( transaction.marks( *fixture.marks )->groups.empty() );
    REQUIRE( transaction.stageMarks( *fixture.marks, { Kind::Add, { second } } ) == Status::Open );
    REQUIRE( transaction.commit() == Status::Preparing );
    CHECK( fixture.marks->getNbMarks() == 0_lcount );
    waitPreparation( transaction );
    REQUIRE( transaction.commit() == Status::Committed );
    CHECK( fixture.marks->getMarks() == QList<LineNumber>{ 1_lnum } );
    CHECK( fixture.marks->reviewMarksRevision() == 1 );
    CHECK( transaction.commit() == Status::Committed );
    CHECK( fixture.marks->reviewMarksRevision() == 1 );
}

TEST_CASE( "A private document can read its marks during adoption without leaking visible marks",
           "[review-transaction][review-mark-writes]" )
{
    Fixture hidden;
    Fixture visible;
    const bool adopted = GENERATE( false, true );
    QSignalSpy hiddenNotifications( hidden.marks.get(), &LogFilteredData::marksChanged );
    QSignalSpy visibleNotifications( visible.marks.get(), &LogFilteredData::marksChanged );
    ReviewTransaction transaction;
    REQUIRE( transaction.stageParticipant( {
        [] { return Status::Open; }, [&] {
            // GUI insertion can synchronously read the newly exposed native model.
            const auto rows = hidden.marks->getMarks();
            CHECK( rows == QList<LineNumber>{ 1_lnum } );
            // A fresh native guard is admissible only after the private one was released.
            const auto noop = LogFilteredData::prepareMarksUpdate( hidden.marks->captureMarksUpdate( rows ) );
            CHECK( hidden.marks->lockMarksUpdate( noop ) );
            CHECK( hiddenNotifications.isEmpty() );
            return adopted;
        }, [] {}, [] {} } ) == Status::Open );
    REQUIRE( transaction.stageMarks( *hidden.marks, { Kind::Add, { hidden.identity( 1_lnum ) } }, true ) == Status::Open );
    REQUIRE( transaction.stageMarks( *visible.marks, { Kind::Add, { visible.identity( 2_lnum ) } } ) == Status::Open );
    REQUIRE( transaction.commit() == Status::Preparing );
    waitPreparation( transaction );
    CHECK( transaction.commit() == ( adopted ? Status::Committed : Status::Conflict ) );
    CHECK( hiddenNotifications.count() == ( adopted ? 1 : 0 ) );
    CHECK( visibleNotifications.count() == ( adopted ? 1 : 0 ) );
    CHECK( visible.marks->getNbMarks() == ( adopted ? 1_lcount : 0_lcount ) );
}

TEST_CASE( "File folder labels and global highlighters install before the first mixed observer",
           "[review-transaction][review-mark-writes]" )
{
    const bool conflict = GENERATE( false, true );
    Fixture fixture;
    ScopedRegexpEngine engine{ RegexpEngine::QRegularExpression };
    auto& global = HighlighterSetCollection::getSynced();
    const auto saved = global;
    const auto globalRevision = global.revision();
    struct Restore {
        HighlighterSetCollection saved;
        ~Restore() { HighlighterSetCollection::get() = saved; }
    } restore{ saved };
    FolderCrawlerWidget folder;
    const auto path = fixture.root.filePath( "folder.log" );
    QFile file{ path };
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    REQUIRE( file.write( "hit\nother\n" ) == 10 );
    file.close();
    folder.setFolder( fixture.root.path(), { path } );
    RegularExpression expression{ RegularExpressionPattern{ QStringLiteral( "hit" ) } };
    auto matcher = expression.createMatcher();
    folder.folderResults()->setResults( { FolderSearchEngine::scanFile( path, *matcher ) } );
    const auto folderSource = folder.folderResults()->createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( folderSource );
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( fixture.labels, { Fixture::label() } ) == Status::Open );
    REQUIRE( transaction.stageMarks( *fixture.marks, { Kind::Add, { fixture.identity( 1_lnum ) } } ) == Status::Open );
    REQUIRE( transaction.stageMarks( folder, { Kind::Add, { folderSource->row( 0_lnum ).identity } } ) == Status::Open );
    const auto set = HighlighterSet::fromDescription( "mixed-review", "Mixed review",
        { Highlighter{ "hit", false, true, QColor( Qt::black ), QColor( Qt::yellow ) } } );
    REQUIRE( transaction.stageHighlighters( { set }, { set.id() } ) == Status::Open );
    bool allInstalled = false;
    QObject::connect( &fixture.labels, &ColorLabelsController::labelsChanged, &fixture.parent, [&] {
        allInstalled = fixture.marks->getNbMarks() == 1_lcount
            && folder.reviewMarksSnapshot()->groups.size() == 1
            && global.activeSetIds() == QStringList{ set.id() }
            && transaction.status() == Status::Committed;
    } );
    QSignalSpy marksChanged( fixture.marks.get(), &LogFilteredData::marksChanged );
    REQUIRE( transaction.commit() == Status::Preparing );
    CHECK( fixture.labels.snapshot().labels.front().isEmpty() );
    if ( conflict ) folder.folderResults()->collapseAll();
    waitPreparation( transaction );
    if ( conflict ) {
        REQUIRE( transaction.commit() == Status::Conflict );
        CHECK( marksChanged.count() == 0 );
        CHECK( fixture.marks->getNbMarks() == 0_lcount );
        CHECK( folder.reviewMarksSnapshot()->groups.empty() );
        CHECK( fixture.labels.snapshot().labels.front().isEmpty() );
        CHECK( global.revision() == globalRevision );
        // A later-owner acquisition failure must release the earlier file guard.
        ReviewTransaction retry;
        REQUIRE( retry.stageMarks( *fixture.marks, { Kind::Add, { fixture.identity( 1_lnum ) } } ) == Status::Open );
        REQUIRE( retry.commit() == Status::Preparing );
        waitPreparation( retry );
        CHECK( retry.commit() == Status::Committed );
    }
    else {
        REQUIRE( transaction.commit() == Status::Committed );
        CHECK( allInstalled );
        CHECK( marksChanged.count() == 1 );
        CHECK( folder.reviewMarksRevision() == 1 );
    }
}

TEST_CASE( "Changes during private mark preparation reject the entire staged transaction",
           "[review-transaction][review-mark-writes]" )
{
    Fixture fixture;
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( fixture.labels, { Fixture::label() } ) == Status::Open );
    REQUIRE( transaction.stageMarks( *fixture.marks, { Kind::Add, { fixture.identity( 1_lnum ) } } ) == Status::Open );
    REQUIRE( transaction.commit() == Status::Preparing );
    auto expected = Status::Conflict;
    SECTION( "human mark ABA" ) { fixture.marks->addMark( 0_lnum ); fixture.marks->deleteMark( 0_lnum ); }
    SECTION( "human layout change" ) { fixture.marks->setContextLines( 1, 1 ); }
    SECTION( "source incarnation replacement" ) { fixture.source->clearCapture(); fixture.source->appendUtf8( "replacement\n" ); }
    SECTION( "owner closes" ) { fixture.marks.reset(); expected = Status::TargetClosed; }
    SECTION( "explicit abort" ) {
        const auto pending = transaction.preparationFuture();
        REQUIRE( pending );
        REQUIRE( transaction.abort() == Status::Aborted );
        REQUIRE( waitUiState( [&] { return pending->isFinished(); } ) );
        expected = Status::Aborted;
    }
    if ( expected != Status::Aborted ) waitPreparation( transaction );
    CHECK( transaction.commit() == expected );
    CHECK( fixture.labels.snapshot().labels.front().isEmpty() );
    if ( fixture.marks ) CHECK( fixture.marks->getNbMarks() == 0_lcount );
}

TEST_CASE( "Mark source proofs cannot silently rebase surviving rolling identities",
           "[review-transaction][review-mark-writes]" )
{
    Fixture fixture;
    CaptureStore::Limits limits;
    limits.segmentTargetBytes = 2;
    limits.maxTotalLines = 2;
    fixture.source->clearCapture();
    fixture.source->setCaptureLimits( limits );
    fixture.source->appendUtf8( "a\nb\n" );
    const auto identity = fixture.identity( 1_lnum );
    ReviewTransaction transaction;
    REQUIRE( transaction.stageMarks( *fixture.marks, { Kind::Add, { identity } } ) == Status::Open );
    fixture.source->appendUtf8( "c\n" );
    REQUIRE( fixture.marks->resolveReviewMark( identity ) );
    CHECK( fixture.marks->resolveReviewMark( identity )->row == 0_lnum );
    CHECK( transaction.commit() == Status::Conflict );
    CHECK( fixture.marks->getNbMarks() == 0_lcount );
}

TEST_CASE( "Completed private mark results retain process admission until publication or retirement",
           "[review-transaction][review-mark-writes]" )
{
    Fixture fixture;
    ReviewTransaction first;
    ReviewTransaction second;
    const auto one = fixture.identity( 1_lnum );
    const auto two = fixture.identity( 2_lnum );
    REQUIRE( first.stageMarks( *fixture.marks, { Kind::Add, { one } } ) == Status::Open );
    REQUIRE( second.stageMarks( *fixture.marks, { Kind::Add, { two } } ) == Status::Open );
    REQUIRE( first.commit() == Status::Preparing );
    REQUIRE( second.commit() == Status::Preparing );
    waitPreparation( first );
    waitPreparation( second );
    ReviewTransaction rejected;
    REQUIRE( rejected.stageMarks( *fixture.marks, { Kind::Add, { one } } ) == Status::Open );
    CHECK( rejected.commit() == Status::ResourceLimit );
    REQUIRE( first.commit() == Status::Committed );
    ReviewTransaction next;
    REQUIRE( next.stageMarks( *fixture.marks, { Kind::Add, { two } } ) == Status::Open );
    REQUIRE( next.commit() == Status::Preparing );
    CHECK( second.commit() == Status::Conflict );
    waitPreparation( next );
    CHECK( next.commit() == Status::Committed );
    CHECK( fixture.marks->getMarks() == QList<LineNumber>{ 1_lnum, 2_lnum } );
}

TEST_CASE( "Mark preparation uses one aggregate allowance across native owners",
           "[review-transaction][review-mark-writes]" )
{
    Fixture fixture;
    std::vector<std::unique_ptr<LogFilteredData>> owners;
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( fixture.labels, { Fixture::label() } ) == Status::Open );
    for ( int index = 0; index < 6; ++index ) {
        auto owner = fixture.source->getNewFilteredData();
        owner->setVisibility( LogFilteredData::VisibilityFlags::Marks );
        owner->setContextLines( 200000, 0 );
        REQUIRE( transaction.stageMarks( *owner, { Kind::Add, { fixture.identity( 1_lnum ) } } ) == Status::Open );
        owners.push_back( std::move( owner ) );
    }
    CHECK( transaction.commit() == Status::ResourceLimit );
    CHECK_FALSE( transaction.preparationFuture() );
    CHECK( fixture.labels.snapshot().labels.front().isEmpty() );
    for ( const auto& owner : owners ) CHECK( owner->getNbMarks() == 0_lcount );
}

TEST_CASE( "Invalid mark edits abort staged prefixes and net no-ops need no preparation",
           "[review-transaction][review-mark-writes]" )
{
    Fixture fixture;
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( fixture.labels, { Fixture::label() } ) == Status::Open );
    const auto identity = fixture.identity( 1_lnum );
    REQUIRE( transaction.stageMarks( *fixture.marks, { Kind::Add, { identity } } ) == Status::Open );
    SECTION( "invalid clear payload" ) {
        CHECK( transaction.stageMarks( *fixture.marks, { Kind::Clear, { identity } } ) == Status::InvalidArgument );
        CHECK( fixture.labels.snapshot().labels.front().isEmpty() );
    }
    SECTION( "unavailable source identity" ) {
        auto foreign = identity;
        ++foreign.incarnation;
        CHECK( transaction.stageMarks( *fixture.marks, { Kind::Add, { foreign } } ) == Status::Conflict );
        CHECK( fixture.labels.snapshot().labels.front().isEmpty() );
    }
    SECTION( "bounded input" ) {
        MarkEdit excessive{ Kind::Add, std::vector<klogg::LogRowId>( klogg::ReviewMarksOwner::MaxMarks + 1, identity ) };
        CHECK( transaction.stageMarks( *fixture.marks, excessive ) == Status::ResourceLimit );
        CHECK( fixture.labels.snapshot().labels.front().isEmpty() );
    }
    SECTION( "net no-op" ) {
        REQUIRE( transaction.stageMarks( *fixture.marks, { Kind::Remove, { identity } } ) == Status::Open );
        CHECK( transaction.commit() == Status::Committed );
        CHECK_FALSE( transaction.preparationFuture() );
        CHECK( fixture.marks->reviewMarksRevision() == 0 );
    }
    CHECK( fixture.marks->getNbMarks() == 0_lcount );
}
