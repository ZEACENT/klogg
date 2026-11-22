#include <catch2/catch.hpp>

#include <QStringList>
#include <QTextCodec>
#include <QFile>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QCoreApplication>

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <vector>

#include "configuration.h"
#include "logdata.h"
#include "preparedfilesource.h"
#include "searchpreparation.h"
#include "test_utils.h"

namespace {
struct RestoreSearchPreferences {
    int chunk = Configuration::get().searchReadBufferSizeLines();
    int threads = Configuration::get().searchThreadPoolSize();
    bool parallel = Configuration::get().useParallelSearch();
    bool block = Configuration::get().useBlockScan();
    RegexpEngine backend = Configuration::get().regexpEngine();
    ~RestoreSearchPreferences()
    {
        auto& config = Configuration::get();
        config.setSearchReadBufferSizeLines( chunk );
        config.setSearchThreadPoolSize( threads );
        config.setUseParallelSearch( parallel );
        config.setUseBlockScan( block );
        config.setRegexpEnging( backend );
    }
};

struct ReadGate {
    void enter()
    {
        std::unique_lock<std::mutex> lock{ mutex };
        entered = true;
        changed.notify_all();
        changed.wait( lock, [this] { return released; } );
    }
    bool waitForEntry()
    {
        std::unique_lock<std::mutex> lock{ mutex };
        return changed.wait_for( lock, std::chrono::seconds{ 5 }, [this] { return entered; } );
    }
    void release()
    {
        std::lock_guard<std::mutex> lock{ mutex };
        released = true;
        changed.notify_all();
    }
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

struct ReleaseGate {
    std::shared_ptr<ReadGate> gate;
    ~ReleaseGate() { gate->release(); }
};

class PreparationRows final : public SearchableLogData::ReadSnapshot {
public:
    enum class Fault { None, Read, ShortRead, Resource };
    explicit PreparationRows( Fault fault = Fault::None, std::shared_ptr<ReadGate> gate = {},
                              QStringList rows = { QStringLiteral( "ERROR\tfirst" ),
                                  QStringLiteral( "plain" ), QStringLiteral( "ERROR third" ),
                                  QStringLiteral( "plain" ) } )
        : fault_( fault ), gate_( std::move( gate ) ), rows_( std::move( rows ) ) {}
    LinesCount lineCount() const override
    {
        return LinesCount{ static_cast<uint64_t>( rows_.size() ) };
    }
    qint64 byteSize() const override { return 0; }
    klogg::SnapshotRow row( LineNumber index ) const override
    {
        return { { klogg::LogRowId::Origin::Memory, 42, 0, index.get() }, index + 1_lcount,
                 true, false };
    }
    SearchableLogData::RawLines getLinesRaw( LineNumber first, LinesCount count ) const override
    {
        reads.push_back( count );
        if ( first >= 1_lnum ) {
            if ( gate_ ) gate_->enter();
            if ( fault_ == Fault::Read ) throw klogg::SnapshotError{ "Injected read failure" };
            if ( fault_ == Fault::Resource ) throw std::bad_alloc{};
            if ( fault_ == Fault::ShortRead ) return {};
        }
        SearchableLogData::RawLines result;
        result.startLine = first;
        result.textDecoder = TextCodecHolder{ QTextCodec::codecForName( "UTF-8" ) }.makeDecoder();
        for ( auto i = first.get(); i < first.get() + count.get(); ++i ) {
            const auto bytes = rows_.at( static_cast<klogg::ContainerIndex>( i ) ).toUtf8() + '\n';
            result.buffer.insert( result.buffer.end(), bytes.cbegin(), bytes.cend() );
            result.endOfLines.push_back( static_cast<qint64>( result.buffer.size() ) );
        }
        return result;
    }
    // Read only after the preparation future has joined the producer.
    mutable std::vector<LinesCount> reads;
private:
    Fault fault_;
    std::shared_ptr<ReadGate> gate_;
    QStringList rows_;
};

FullSearchPreparationRequest requestFor( SearchableLogData::ReadSnapshot::Ptr source,
                                        uint32_t threads = 1 )
{
    return { std::move( source ), RegularExpressionPattern{ QStringLiteral( "ERROR" ) },
             { 1, threads, true, RegexpEngine::QRegularExpression }, 0_lnum, std::nullopt };
}

SearchPreparation runPreparation( FullSearchPreparationRequest request, AtomicFlag& cancelled )
{
    return std::async( std::launch::async,
        [request = std::move( request ), &cancelled]() mutable {
            return prepareFullSearch( std::move( request ), cancelled );
        } ).get();
}
} // namespace

TEST_CASE( "Prepared search captures pattern backend and execution policy before worker dispatch",
           "[search-preparation]" )
{
    RestoreSearchPreferences restore;
    auto& config = Configuration::get();
    config.setSearchReadBufferSizeLines( 1 );
    config.setSearchThreadPoolSize( 2 );
    config.setUseParallelSearch( true );
    config.setUseBlockScan( false );
    config.setRegexpEnging( RegexpEngine::QRegularExpression );
    const auto snapshot = std::make_shared<PreparationRows>();
    auto request = requestFor( snapshot );
    RegularExpressionPattern pattern{ QStringLiteral( "ERROR" ) };
    bool inverse = false;
    SECTION( "single pattern" ) {}
    SECTION( "inverse pattern" ) { pattern.isExclude = true; inverse = true; }
    SECTION( "boolean pattern" ) {
        pattern.pattern = QStringLiteral( "\"ERROR\" | \"WARN\"" );
        pattern.isBoolean = true;
    }
    request.pattern = pattern;
    request.settings = SearchSettings::capture();
    CHECK( request.settings.matchingThreads == 2 );
    CHECK_FALSE( request.settings.useBlockScan );
    pattern.pattern = QStringLiteral( "never" );
    config.setSearchReadBufferSizeLines( 99 );
    config.setSearchThreadPoolSize( 1 );
    config.setUseParallelSearch( false );
    config.setUseBlockScan( true );
    config.setRegexpEnging( RegexpEngine::Vectorscan );
    AtomicFlag cancelled;
    const auto outcome = runPreparation( std::move( request ), cancelled );
    REQUIRE( outcome.status == SearchPreparation::Status::Ready );
    REQUIRE( outcome.result );
    REQUIRE( outcome.result->compiled );
    const auto matcher = outcome.result->compiled->createMatcher();
    CHECK( matcher->hasMatch( "ERROR retained query" ) == !inverse );
    CHECK( matcher->hasMatch( "plain" ) == inverse );
    CHECK( outcome.result->matches.cardinality() == 2 );
    CHECK( outcome.result->matches.contains( uint64_t{ 0 } ) == !inverse );
    CHECK( outcome.result->matches.contains( uint64_t{ 1 } ) == inverse );
    REQUIRE( snapshot->reads.size() == 4 );
    for ( const auto count : snapshot->reads ) CHECK( count == 1_lcount );
}

TEST_CASE( "Prepared result adoption revokes blocked native work without waiting or notifying",
           "[search-preparation][prepared-search-adoption]" )
{
    RestoreSearchPreferences restore;
    Configuration::get().setRegexpEnging( RegexpEngine::QRegularExpression );
    Configuration::get().setSearchReadBufferSizeLines( 1 );
    LogData source;
    LogFilteredDataWorker worker{ source };
    const auto gate = std::make_shared<ReadGate>();
    ReleaseGate release{ gate };
    const auto blocked = std::make_shared<PreparationRows>( PreparationRows::Fault::None, gate );
    worker.search( RegularExpressionPattern{ QStringLiteral( "plain" ) }, 0_lnum, 4_lnum, blocked );
    REQUIRE( gate->waitForEntry() );
    AtomicFlag cancelled;
    const auto prepared = runPreparation( requestFor( std::make_shared<PreparationRows>() ), cancelled );
    REQUIRE( prepared.status == SearchPreparation::Status::Ready );
    LogFilteredDataWorker::PreparedResult payload{ *prepared.result };
    static_assert( !std::is_copy_constructible<LogFilteredDataWorker::PreparedResult>::value,
                   "Mutable publication stamps must have only one owner" );
    const auto oldGeneration = worker.currentGeneration();
    QSignalSpy progress{ &worker, &LogFilteredDataWorker::searchProgressed };
    REQUIRE( worker.adoptPreparedResult( std::move( payload ) ) );
    const auto installed = worker.completedSearchResult();
    REQUIRE( installed );
    CHECK( installed->generation > oldGeneration );
    CHECK( installed->compiled == prepared.result->compiled );
    CHECK( installed->matches.contains( uint64_t{ 0 } ) );
    CHECK( progress.empty() );
    CHECK( prepared.result->generation == 0 );
    CHECK_FALSE( worker.adoptPreparedResult( std::move( payload ) ) );
    gate->release();
    worker.waitForDone();
    QCoreApplication::processEvents();
    CHECK( worker.completedSearchResult() == installed );
    CHECK( progress.empty() );
}

TEST_CASE( "Prepared result adoption rejects unavailable or already owned payloads",
           "[search-preparation][prepared-search-adoption]" )
{
    LogData source;
    LogFilteredDataWorker worker{ source };
    AtomicFlag cancelled;
    const auto prepared = runPreparation( requestFor( std::make_shared<PreparationRows>() ), cancelled );
    REQUIRE( prepared.status == SearchPreparation::Status::Ready );
    auto result = *prepared.result;
    SECTION( "missing source" ) { result.source.reset(); }
    SECTION( "missing matcher" ) { result.compiled.reset(); }
    SECTION( "published payload" ) { result.generation = 1; }
    SECTION( "retired owner" ) { worker.retire(); }
    CHECK_FALSE( worker.adoptPreparedResult( LogFilteredDataWorker::PreparedResult{ result } ) );
    CHECK_FALSE( worker.completedSearchResult() );
}

TEST_CASE( "Restored native search results retain their own query for subsequent updates",
           "[search-preparation][restored-search-query]" )
{
    RestoreSearchPreferences restore;
    Configuration::get().setRegexpEnging( RegexpEngine::QRegularExpression );
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    const auto path = root.filePath( "input.log" );
    QFile file{ path };
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    REQUIRE( file.write( "ERROR first\nplain\nERROR third\nplain\n" ) == 36 );
    file.close();
    AtomicFlag cancelled;
    const auto settings = IndexingSettings::capture();
    auto prepared = std::async( std::launch::async, [&] {
        return prepareFileSource( path, settings, cancelled );
    } ).get();
    REQUIRE( prepared.status == FileSourcePreparation::Status::Ready );
    auto source = LogData::fromPrepared( prepared.source );
    REQUIRE( source );
    LogFilteredDataWorker worker{ *source };
    const RegularExpressionPattern original{ QStringLiteral( "ERROR" ) };
    worker.search( original, 0_lnum, 4_lnum );
    REQUIRE( waitUiState( [&] { return bool( worker.completedSearchResult() ); } ) );
    const auto first = worker.completedSearchResult();
    worker.search( RegularExpressionPattern{ QStringLiteral( "plain" ) }, 0_lnum, 4_lnum );
    REQUIRE( waitUiState( [&] { return bool( worker.completedSearchResult() ); } ) );
    worker.bumpGeneration();
    worker.restoreCompletedResult( *first );
    REQUIRE( worker.updateSearch( original, 0_lnum, 4_lnum, 4_lnum ) );
    REQUIRE( waitUiState( [&] { return bool( worker.completedSearchResult() ); } ) );
    const auto result = worker.completedSearchResult();
    CHECK( result->matches.contains( uint64_t{ 0 } ) );
    CHECK_FALSE( result->matches.contains( uint64_t{ 1 } ) );
    CHECK( result->matches.contains( uint64_t{ 2 } ) );
    CHECK_FALSE( result->matches.contains( uint64_t{ 3 } ) );
}

TEST_CASE( "Native search requests capture execution settings before waiting for their predecessor",
           "[search-preparation]" )
{
    RestoreSearchPreferences restore;
    auto& config = Configuration::get();
    config.setSearchReadBufferSizeLines( 1 );
    config.setUseParallelSearch( false );
    config.setRegexpEnging( RegexpEngine::QRegularExpression );
    LogData source;
    LogFilteredDataWorker worker{ source };
    const auto gate = std::make_shared<ReadGate>();
    ReleaseGate release{ gate };
    const auto blocked = std::make_shared<PreparationRows>( PreparationRows::Fault::None, gate );
    const RegularExpressionPattern pattern{ QStringLiteral( "ERROR" ) };
    worker.search( pattern, 0_lnum, 4_lnum, blocked );
    REQUIRE( gate->waitForEntry() );
    const auto replacement = std::make_shared<PreparationRows>();
    worker.search( pattern, 0_lnum, 4_lnum, replacement );
    config.setSearchReadBufferSizeLines( 99 );
    config.setRegexpEnging( RegexpEngine::Vectorscan );
    gate->release();
    REQUIRE( waitUiState( [&] { return bool( worker.completedSearchResult() ); } ) );
    worker.waitForDone();
    const auto completed = worker.completedSearchResult();
    REQUIRE( completed );
    CHECK( completed->source == replacement );
    CHECK( completed->matches.cardinality() == 2 );
    REQUIRE( replacement->reads.size() == 4 );
    for ( const auto count : replacement->reads ) CHECK( count == 1_lcount );
}

TEST_CASE( "Full search preparation owns snapshot results without a mutable source",
           "[search-preparation]" )
{
    const auto threads = GENERATE( uint32_t{ 1 }, uint32_t{ 2 } );
    auto snapshot = std::make_shared<PreparationRows>();
    std::weak_ptr<const SearchableLogData::ReadSnapshot> retained = snapshot;
    auto request = requestFor( snapshot, threads );
    snapshot.reset();
    AtomicFlag cancelled;
    const auto outcome = runPreparation( std::move( request ), cancelled );
    REQUIRE( outcome.status == SearchPreparation::Status::Ready );
    REQUIRE( outcome.result );
    static_assert( std::is_const_v<std::remove_reference_t<decltype( *outcome.result )>> );
    CHECK( outcome.result->matches.cardinality() == 2 );
    CHECK( outcome.result->matches.contains( uint64_t{ 0 } ) );
    CHECK( outcome.result->matches.contains( uint64_t{ 2 } ) );
    CHECK( outcome.result->processedLines == 4_lcount );
    CHECK( outcome.result->maxLength == 13_length );
    CHECK( outcome.result->source == retained.lock() );
    CHECK( outcome.result->source->lineString( 0_lnum ) == QStringLiteral( "ERROR\tfirst" ) );
    CHECK( outcome.result->generation == 0 );
    CHECK( outcome.result->operationId == 0 );
    CHECK( outcome.result->requestTicket == 0 );
}

TEST_CASE( "Full search preparation accepts empty and bounded native ranges",
           "[search-preparation]" )
{
    auto snapshot = std::make_shared<PreparationRows>();
    auto request = requestFor( snapshot );
    uint64_t expectedMatches = 1;
    LinesCount expectedProcessed = 3_lcount;
    SECTION( "bounded range excludes earlier and later rows" ) {
        request.startLine = 1_lnum;
        request.endLine = 3_lnum;
    }
    SECTION( "empty snapshot succeeds without a read" ) {
        snapshot = std::make_shared<PreparationRows>( PreparationRows::Fault::None, nullptr,
                                                     QStringList{} );
        request.source = snapshot;
        expectedMatches = 0;
        expectedProcessed = 0_lcount;
    }
    SECTION( "empty range retains its native cutoff" ) {
        request.startLine = 2_lnum;
        request.endLine = 2_lnum;
        expectedMatches = 0;
        expectedProcessed = 2_lcount;
    }
    AtomicFlag cancelled;
    const auto outcome = runPreparation( std::move( request ), cancelled );
    REQUIRE( outcome.status == SearchPreparation::Status::Ready );
    REQUIRE( outcome.result );
    CHECK( outcome.result->matches.cardinality() == expectedMatches );
    CHECK( outcome.result->processedLines == expectedProcessed );
    if ( expectedMatches ) CHECK( outcome.result->matches.contains( uint64_t{ 2 } ) );
    else CHECK( snapshot->reads.empty() );
}

TEST_CASE( "Full search preparation distinguishes invalid inputs and patterns",
           "[search-preparation]" )
{
    auto request = requestFor( std::make_shared<PreparationRows>() );
    auto expected = SearchPreparation::Status::InvalidInput;
    SECTION( "missing snapshot" ) { request.source.reset(); }
    SECTION( "zero chunk" ) { request.settings.readBufferSizeLines = 0; }
    SECTION( "negative chunk" ) { request.settings.readBufferSizeLines = -1; }
    SECTION( "zero matching threads" ) { request.settings.matchingThreads = 0; }
    SECTION( "reversed range" ) { request.startLine = 3_lnum; request.endLine = 1_lnum; }
    SECTION( "invalid regex" ) {
        request.pattern = RegularExpressionPattern{ QStringLiteral( "[" ) };
        expected = SearchPreparation::Status::CompileFailure;
    }
    SECTION( "invalid boolean expression" ) {
        request.pattern = RegularExpressionPattern{ QStringLiteral( "AND" ) };
        request.pattern.isBoolean = true;
        expected = SearchPreparation::Status::CompileFailure;
    }
    AtomicFlag cancelled;
    const auto outcome = runPreparation( std::move( request ), cancelled );
    CHECK( outcome.status == expected );
    CHECK_FALSE( outcome.result );
}

TEST_CASE( "Full search preparation honors cancellation before any snapshot reads",
           "[search-preparation]" )
{
    const auto snapshot = std::make_shared<PreparationRows>();
    AtomicFlag cancelled;
    cancelled.set();
    const auto outcome = runPreparation( requestFor( snapshot ), cancelled );
    CHECK( outcome.status == SearchPreparation::Status::Cancelled );
    CHECK_FALSE( outcome.result );
    CHECK( snapshot->reads.empty() );
}

TEST_CASE( "Cancelling in-flight full search preparation never returns a partial prefix",
           "[search-preparation]" )
{
    const auto threads = GENERATE( uint32_t{ 1 }, uint32_t{ 2 } );
    const auto gate = std::make_shared<ReadGate>();
    const auto snapshot = std::make_shared<PreparationRows>( PreparationRows::Fault::None, gate );
    AtomicFlag cancelled;
    auto work = std::async( std::launch::async, [request = requestFor( snapshot, threads ), &cancelled] {
        return prepareFullSearch( request, cancelled );
    } );
    ReleaseGate release{ gate }; // Unblock before the future joins on assertion failure.
    REQUIRE( gate->waitForEntry() );
    cancelled.set();
    gate->release();
    const auto outcome = work.get();
    CHECK( outcome.status == SearchPreparation::Status::Cancelled );
    CHECK_FALSE( outcome.result );
    CHECK( snapshot->reads.size() >= 2 );
}

TEST_CASE( "Full search preparation rejects read and resource failures after a searched prefix",
           "[search-preparation]" )
{
    const auto threads = GENERATE( uint32_t{ 1 }, uint32_t{ 2 } );
    const auto fault = GENERATE( PreparationRows::Fault::Read, PreparationRows::Fault::ShortRead,
                                 PreparationRows::Fault::Resource );
    const auto snapshot = std::make_shared<PreparationRows>( fault );
    AtomicFlag cancelled;
    const auto outcome = runPreparation( requestFor( snapshot, threads ), cancelled );
    CHECK( outcome.status == SearchPreparation::Status::Failed );
    CHECK_FALSE( outcome.result );
    CHECK( snapshot->reads.size() == 2 );
}
