#include <catch2/catch.hpp>

#include <future>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUuid>

#include "logdata.h"
#include "logfiltereddata.h"
#include "preparedfilesource.h"
#include "streaminglogdata.h"
#include "test_utils.h"

class PreparedSearchProjectionAccess;
template <>
struct LogFilteredData::access_by<PreparedSearchProjectionAccess> {
    static bool hasCachedContext( const LogFilteredData& model ) { return model.contextLinesListValid_; }
};

namespace {
struct SearchPublicationFixture {
    QTemporaryDir root;
    std::unique_ptr<SearchableLogData> source;
    std::unique_ptr<LogFilteredData> filtered;
    AtomicFlag cancelled;

    explicit SearchPublicationFixture( bool live = true )
    {
        REQUIRE( root.isValid() );
        const QByteArray text{ "ERROR first\nplain\nERROR third\nlast\n" };
        if ( live ) {
            auto stream = std::make_unique<StreamingLogData>(
                QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
            stream->appendUtf8( text );
            source = std::move( stream );
        } else {
            const auto path = root.filePath( "source.log" );
            QFile file{ path };
            REQUIRE( file.open( QIODevice::WriteOnly ) );
            REQUIRE( file.write( text ) == text.size() );
            file.close();
            const auto settings = IndexingSettings::capture();
            auto prepared = std::async( std::launch::async, [&] {
                return prepareFileSource( path, settings, cancelled );
            } ).get();
            REQUIRE( prepared.status == FileSourcePreparation::Status::Ready );
            source = LogData::fromPrepared( prepared.source );
        }
        REQUIRE( source );
        filtered = source->getNewFilteredData();
        REQUIRE( filtered );
    }
    LogFilteredData::SearchUpdatePreparation prepare( const QString& pattern = "ERROR" )
    {
        const auto captured = filtered->captureSearchUpdate( RegularExpressionPattern{ pattern } );
        REQUIRE( captured );
        return std::async( std::launch::async, [&] {
            return LogFilteredData::prepareSearchUpdate( captured, cancelled );
        } ).get();
    }
};
}

TEST_CASE( "Prepared native search installs complete projections before notifying",
           "[prepared-search-publication]" )
{
    SearchPublicationFixture fixture{ GENERATE( false, true ) };
    auto& model = *fixture.filtered;
    model.addMark( 1_lnum );
    model.setContextLines( 1, 1 );
    const auto marksRevision = model.reviewMarksRevision();
    QSignalSpy results{ &model, &LogFilteredData::searchResultsChanged };
    QSignalSpy progress{ &model, &LogFilteredData::searchProgressed };
    const auto prepared = fixture.prepare();
    REQUIRE( prepared.status == SearchPreparation::Status::Ready );
    REQUIRE( prepared.update );
    CHECK( model.getNbMatches() == 0_lcount );
    CHECK( results.empty() );
    auto publication = model.lockSearchUpdate( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    CHECK_FALSE( publication->install() );
    CHECK( model.isSearchComplete() );
    CHECK( model.getNbMatches() == 2_lcount );
    CHECK( model.getNbLine() == 4_lcount );
    CHECK( model.getMatchingLineNumber( 3_lnum ) == 3_lnum );
    CHECK( model.getLineString( 2_lnum ) == "ERROR third" );
    CHECK( model.getMarks() == QList<LineNumber>{ 1_lnum } );
    CHECK( model.reviewMarksRevision() == marksRevision );
    CHECK( results.empty() );
    CHECK( progress.empty() );
    publication->notify();
    CHECK( results.empty() ); // Release every owner's guard before observers run.
    publication->release();
    publication->notify();
    publication->notify();
    CHECK( results.size() == 1 );
    CHECK( progress.size() == 1 );
    CHECK_FALSE( model.lockSearchUpdate( prepared.update ) );
}

TEST_CASE( "Prepared empty-query reset preserves marks without manufacturing completed matches",
           "[prepared-search-publication][prepared-search-reset]" )
{
    SearchPublicationFixture fixture{ GENERATE( false, true ) };
    const bool showAll = GENERATE( false, true );
    auto& model = *fixture.filtered;
    model.addMark( 1_lnum );
    const auto initial = fixture.prepare();
    auto initialPublication = model.lockSearchUpdate( initial.update );
    REQUIRE( initialPublication );
    REQUIRE( initialPublication->install() );
    initialPublication->release();
    initialPublication->notify();
    REQUIRE( model.isSearchComplete() );
    const auto generation = model.currentSearchGeneration();
    const auto starts = model.searchPerformanceCounters().operationStarts;
    using Flags = LogFilteredData::VisibilityFlags;
    const auto captured = model.captureSearchReset( showAll, Flags::Matches | Flags::Marks );
    REQUIRE( captured );
    const auto prepared = std::async( std::launch::async, [&] {
        return LogFilteredData::prepareSearchUpdate( captured, fixture.cancelled );
    } ).get();
    REQUIRE( prepared.status == SearchPreparation::Status::Ready );
    CHECK( model.getNbMatches() == 2_lcount );
    QSignalSpy results{ &model, &LogFilteredData::searchResultsChanged };
    QSignalSpy progress{ &model, &LogFilteredData::searchProgressed };
    auto publication = model.lockSearchUpdate( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    CHECK( publication->isCurrent() );
    CHECK( model.currentSearchGeneration() > generation );
    CHECK( model.allLinesVisible() == showAll );
    CHECK( model.getNbMatches() == 0_lcount );
    CHECK( model.getMarks() == QList<LineNumber>{ 1_lnum } );
    CHECK( model.getNbLine() == ( showAll ? 4_lcount : 1_lcount ) );
    CHECK_FALSE( model.isSearchComplete() );
    CHECK_FALSE( model.createReadSnapshot( klogg::SnapshotScope::Matches ) );
    CHECK( model.createReadSnapshot( klogg::SnapshotScope::PresentedRows )->lineCount()
           == ( showAll ? 4_lcount : 1_lcount ) );
    CHECK( model.searchPerformanceCounters().operationStarts == starts );
    CHECK( results.empty() );
    publication->release();
    publication->notify();
    REQUIRE( results.size() == 1 );
    CHECK_FALSE( results.front().at( 2 ).toBool() ); // A reset is not a completed search.
    CHECK( progress.empty() );
}

TEST_CASE( "An explicit empty regex remains a search rather than an empty-query reset",
           "[prepared-search-publication][prepared-search-reset]" )
{
    SearchPublicationFixture fixture;
    // The low-level regex API and the UI's empty draft have distinct semantics.
    const auto prepared = fixture.prepare( QString{} );
    REQUIRE( prepared.status == SearchPreparation::Status::Ready );
    auto publication = fixture.filtered->lockSearchUpdate( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    CHECK( fixture.filtered->isSearchComplete() );
    CHECK( fixture.filtered->getNbMatches() == 4_lcount );
    CHECK_FALSE( fixture.filtered->allLinesVisible() );
}

TEST_CASE( "Prepared reset cancellation and stale reset notifications preserve newer state",
           "[prepared-search-publication][prepared-search-reset]" )
{
    SearchPublicationFixture fixture;
    const auto captured = fixture.filtered->captureSearchReset(
        true, LogFilteredData::VisibilityFlags::Matches );
    REQUIRE( captured );
    bool cancel = false;
    bool retire = false;
    SECTION( "cancel before preparation" ) { fixture.cancelled.set(); cancel = true; }
    SECTION( "a later native clear revokes notification" ) {}
    SECTION( "retirement revokes notification without a completed result" ) { retire = true; }
    const auto prepared = std::async( std::launch::async, [&] {
        return LogFilteredData::prepareSearchUpdate( captured, fixture.cancelled );
    } ).get();
    if ( cancel ) {
        CHECK( prepared.status == SearchPreparation::Status::Cancelled );
        CHECK_FALSE( prepared.update );
    } else {
        auto publication = fixture.filtered->lockSearchUpdate( prepared.update );
        REQUIRE( publication );
        REQUIRE( publication->install() );
        publication->release();
        if ( retire ) fixture.filtered->retire();
        else fixture.filtered->clearSearch();
        CHECK_FALSE( publication->isCurrent() );
        QSignalSpy results{ fixture.filtered.get(), &LogFilteredData::searchResultsChanged };
        publication->notify();
        CHECK( results.empty() );
    }
}

TEST_CASE( "Prepared native queries install their requested visibility with matching rows",
           "[prepared-search-publication][prepared-search-visibility]" )
{
    SearchPublicationFixture fixture;
    auto& model = *fixture.filtered;
    using Flags = LogFilteredData::VisibilityFlags;
    model.setVisibility( Flags::Marks );
    model.addMark( 1_lnum );
    const bool context = GENERATE( false, true );
    if ( context ) model.setContextLines( 1, 1 );
    const auto requested = Flags::Matches | Flags::Marks;
    const auto captured = model.captureSearchUpdate(
        RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, requested );
    REQUIRE( captured );
    const auto prepared = std::async( std::launch::async, [&] {
        return LogFilteredData::prepareSearchUpdate( captured, fixture.cancelled );
    } ).get();
    REQUIRE( prepared.status == SearchPreparation::Status::Ready );
    CHECK( model.visibility() == LogFilteredData::Visibility{ Flags::Marks } );
    CHECK( model.getNbLine() == ( context ? 3_lcount : 1_lcount ) );
    SECTION( "install with the complete result" ) {
        auto publication = model.lockSearchUpdate( prepared.update );
        REQUIRE( publication );
        REQUIRE( publication->install() );
        CHECK( model.visibility() == requested );
        REQUIRE( model.getNbLine() == ( context ? 4_lcount : 3_lcount ) );
        CHECK( model.getMatchingLineNumber( 0_lnum ) == 0_lnum );
        CHECK( model.getMatchingLineNumber( 2_lnum ) == 2_lnum );
    }
    SECTION( "a human visibility edit remains a conflict" ) {
        model.setVisibility( Flags::Matches );
        CHECK_FALSE( model.lockSearchUpdate( prepared.update ) );
    }
}

TEST_CASE( "Prepared native search rejects relevant concurrent state changes",
           "[prepared-search-publication]" )
{
    SearchPublicationFixture fixture;
    const auto prepared = fixture.prepare();
    REQUIRE( prepared.status == SearchPreparation::Status::Ready );
    auto& model = *fixture.filtered;
    SECTION( "marks ABA" ) { model.addMark( 1_lnum ); model.deleteMark( 1_lnum ); }
    SECTION( "query generation" ) { model.clearSearch(); }
    SECTION( "context" ) { model.setContextLines( 1, 1 ); }
    SECTION( "visibility" ) { model.setVisibility( LogFilteredData::VisibilityFlags::Marks ); }
    SECTION( "source append changes the search cutoff" ) {
        dynamic_cast<StreamingLogData*>( fixture.source.get() )->appendUtf8( "ERROR new\n" );
    }
    SECTION( "retirement" ) { model.retire(); }
    SECTION( "wrong owner" ) {
        auto other = fixture.source->getNewFilteredData();
        CHECK_FALSE( other->lockSearchUpdate( prepared.update ) );
        return;
    }
    CHECK_FALSE( model.lockSearchUpdate( prepared.update ) );
    CHECK( model.getNbMatches() == 0_lcount );
}

TEST_CASE( "Prepared native search never publishes cancellation or invalid queries",
           "[prepared-search-publication]" )
{
    SearchPublicationFixture fixture;
    QString query{ "ERROR" };
    auto expected = SearchPreparation::Status::Cancelled;
    SECTION( "cancelled" ) { fixture.cancelled.set(); }
    SECTION( "invalid regex" ) { query = "["; expected = SearchPreparation::Status::CompileFailure; }
    const auto prepared = fixture.prepare( query );
    CHECK( prepared.status == expected );
    CHECK_FALSE( prepared.update );
    CHECK( fixture.filtered->getNbMatches() == 0_lcount );
}

TEST_CASE( "Unchanged context reconciliation retains the privately built search projection",
           "[prepared-search-publication][prepared-context-retention]" )
{
    using Access = LogFilteredData::access_by<PreparedSearchProjectionAccess>;
    SearchPublicationFixture fixture;
    fixture.filtered->setContextLines( 1, 1 );
    const auto prepared = fixture.prepare();
    auto publication = fixture.filtered->lockSearchUpdate( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    publication->release();
    REQUIRE( Access::hasCachedContext( *fixture.filtered ) );
    fixture.filtered->setContextLines( 1, 1 );
    CHECK( Access::hasCachedContext( *fixture.filtered ) );
    fixture.filtered->setContextLines( -3, 1 );
    CHECK_FALSE( Access::hasCachedContext( *fixture.filtered ) );
    CHECK( fixture.filtered->getNbLine() == 4_lcount );
    REQUIRE( Access::hasCachedContext( *fixture.filtered ) );
    fixture.filtered->setContextLines( -7, 1 );
    CHECK( Access::hasCachedContext( *fixture.filtered ) );
}

TEST_CASE( "Prepared native searches install every owner before the first observer",
           "[prepared-search-publication]" )
{
    SearchPublicationFixture first{ false };
    SearchPublicationFixture second;
    const auto firstPrepared = first.prepare();
    const auto secondPrepared = second.prepare();
    auto firstPublication = first.filtered->lockSearchUpdate( firstPrepared.update );
    auto secondPublication = second.filtered->lockSearchUpdate( secondPrepared.update );
    REQUIRE( firstPublication );
    REQUIRE( secondPublication );
    bool observed = false;
    QObject::connect( first.filtered.get(), &LogFilteredData::searchResultsChanged,
        first.source.get(), [&] {
            observed = true;
            CHECK( second.filtered->isSearchComplete() );
            CHECK( second.filtered->getNbMatches() == 2_lcount );
            // Observers may start another edit only after every guard is released.
            const auto marks = LogFilteredData::prepareMarksUpdate(
                second.filtered->captureMarksUpdate( {} ) );
            CHECK( second.filtered->lockMarksUpdate( marks ) );
        } );
    REQUIRE( firstPublication->install() );
    REQUIRE( secondPublication->install() );
    firstPublication->release();
    secondPublication->release();
    firstPublication->notify();
    secondPublication->notify();
    CHECK( observed );
}

TEST_CASE( "Published private searches continue through the native live update pipeline",
           "[prepared-search-publication]" )
{
    SearchPublicationFixture fixture;
    const auto prepared = fixture.prepare();
    auto publication = fixture.filtered->lockSearchUpdate( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    publication->release();
    publication->notify();
    const auto retained = fixture.filtered->completedSearchResult();
    const auto generation = fixture.filtered->currentSearchGeneration();
    auto* stream = dynamic_cast<StreamingLogData*>( fixture.source.get() );
    stream->appendUtf8( "ERROR appended\nplain appended\n" );
    fixture.filtered->updateSearch( 0_lnum, LineNumber{ stream->getNbLine().get() } );
    REQUIRE( waitUiState( [&] { return fixture.filtered->isSearchComplete(); } ) );
    CHECK( fixture.filtered->getNbMatches() == 3_lcount );
    CHECK( fixture.filtered->currentSearchGeneration() == generation );
    CHECK( retained->matches.cardinality() == 2 );
    CHECK( fixture.filtered->completedSearchResult()->compiled == retained->compiled );
}

TEST_CASE( "Abandoned native search guards do not install and projection preparation is bounded",
           "[prepared-search-publication]" )
{
    SearchPublicationFixture fixture;
    SECTION( "release without installation" ) {
        const auto prepared = fixture.prepare();
        auto publication = fixture.filtered->lockSearchUpdate( prepared.update );
        REQUIRE( publication );
        publication->release();
        CHECK_FALSE( publication->install() );
        publication->notify();
        CHECK( fixture.filtered->lockSearchUpdate( prepared.update ) );
    }
    SECTION( "newly visible matches are included in the context budget" ) {
        fixture.filtered->setVisibility( LogFilteredData::VisibilityFlags::Marks );
        fixture.filtered->setContextLines( static_cast<int>( LogFilteredData::MaxPreparedContextRows ), 0 );
        const auto captured = fixture.filtered->captureSearchUpdate(
            RegularExpressionPattern{ QStringLiteral( "ERROR" ) },
            LogFilteredData::VisibilityFlags::Matches );
        const auto prepared = std::async( std::launch::async, [&] {
            return LogFilteredData::prepareSearchUpdate( captured, fixture.cancelled );
        } ).get();
        CHECK( prepared.status == SearchPreparation::Status::Failed );
        CHECK_FALSE( prepared.update );
    }
    SECTION( "excessive context" ) {
        fixture.filtered->setContextLines( static_cast<int>( LogFilteredData::MaxPreparedContextRows ), 0 );
        const auto prepared = fixture.prepare();
        CHECK( prepared.status == SearchPreparation::Status::Failed );
        CHECK_FALSE( prepared.update );
    }
    CHECK( fixture.filtered->getNbMatches() == 0_lcount );
}

TEST_CASE( "Prepared terminal notifications cannot overtake a reentrant native update",
           "[prepared-search-publication][prepared-search-reentry]" )
{
    SearchPublicationFixture fixture;
    const auto prepared = fixture.prepare();
    auto publication = fixture.filtered->lockSearchUpdate( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    publication->release();
    const auto generation = fixture.filtered->currentSearchGeneration();
    QSignalSpy progress{ fixture.filtered.get(), &LogFilteredData::searchProgressed };
    QObject::connect( fixture.filtered.get(), &LogFilteredData::searchResultsChanged,
        fixture.source.get(), [&] {
            dynamic_cast<StreamingLogData*>( fixture.source.get() )->appendUtf8( "ERROR later\n" );
            fixture.filtered->updateSearch( 0_lnum, LineNumber{ fixture.source->getNbLine().get() } );
        } );
    publication->notify();
    CHECK( fixture.filtered->currentSearchGeneration() == generation );
    CHECK_FALSE( fixture.filtered->isSearchComplete() );
    CHECK( progress.empty() );
}

TEST_CASE( "Earlier owner observers may supersede a later prepared notification",
           "[prepared-search-publication][prepared-search-reentry]" )
{
    SearchPublicationFixture first;
    SearchPublicationFixture second;
    const auto firstPrepared = first.prepare();
    const auto secondPrepared = second.prepare();
    auto firstPublication = first.filtered->lockSearchUpdate( firstPrepared.update );
    auto secondPublication = second.filtered->lockSearchUpdate( secondPrepared.update );
    REQUIRE( firstPublication );
    REQUIRE( secondPublication );
    REQUIRE( firstPublication->install() );
    REQUIRE( secondPublication->install() );
    firstPublication->release();
    secondPublication->release();
    QSignalSpy results{ second.filtered.get(), &LogFilteredData::searchResultsChanged };
    QSignalSpy progress{ second.filtered.get(), &LogFilteredData::searchProgressed };
    QObject::connect( first.filtered.get(), &LogFilteredData::searchResultsChanged,
        first.source.get(), [&] {
            dynamic_cast<StreamingLogData*>( second.source.get() )->appendUtf8( "ERROR later\n" );
            second.filtered->updateSearch( 0_lnum, LineNumber{ second.source->getNbLine().get() } );
        } );
    firstPublication->notify();
    secondPublication->notify();
    CHECK_FALSE( second.filtered->isSearchComplete() );
    CHECK( results.empty() );
    CHECK( progress.empty() );
}

TEST_CASE( "Native search publication observers may destroy the model and publication",
           "[prepared-search-publication]" )
{
    SearchPublicationFixture fixture;
    const auto prepared = fixture.prepare();
    auto publication = fixture.filtered->lockSearchUpdate( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    publication->release();
    bool notified = false;
    QObject::connect( fixture.filtered.get(), &LogFilteredData::searchResultsChanged,
        fixture.source.get(), [&] {
            notified = true;
            publication.reset();
            fixture.filtered.reset();
        } );
    publication->notify();
    CHECK( notified );
    CHECK_FALSE( publication );
    CHECK_FALSE( fixture.filtered );
}
