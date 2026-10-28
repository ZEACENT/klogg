/*
 * Copyright (C) 2026
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

#include <catch2/catch.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEvent>
#include <QSignalSpy>
#include <QTextCodec>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>

#ifndef Q_MOC_RUN
#include <roaring.hh>
#include <roaring64map.hh>
#include <tbb/task_group.h>
#include <tbb/global_control.h>
#endif

#include "abstractlogdata.h"
#include "atomicflag.h"
#include "configuration.h"
#include "containers.h"
#include "test_utils.h"
#include "linetypes.h"
#include "logfiltereddata.h"
#include "logfiltereddataworker.h"
#include "matchercache.h"
#include "regularexpression.h"
#include "searchablelogdata.h"
#include "synchronization.h"

namespace {
class TestSearchableLogData : public SearchableLogData {
public:
    void interruptLoading() override {}
    std::unique_ptr<LogFilteredData> getNewFilteredData() const override
    {
        return nullptr;
    }
    qint64 getFileSize() const override
    {
        return 0;
    }
    QDateTime getLastModifiedDate() const override
    {
        return {};
    }
    void reload( QTextCodec* = nullptr ) override {}
    QTextCodec* getDetectedEncoding() const override
    {
        return QTextCodec::codecForName( "UTF-8" );
    }
    void setPrefilter( const QString& ) override {}
    void setAnsiProcessingMode( AnsiProcessingMode ) override {}
    RawLines getLinesRaw( LineNumber, LinesCount ) const override
    {
        return {};
    }
    bool isLiveSource() const override
    {
        return true;
    }

protected:
    QString doGetLineString( LineNumber ) const override
    {
        return {};
    }
    QString doGetExpandedLineString( LineNumber ) const override
    {
        return {};
    }
    klogg::vector<QString> doGetLines( LineNumber, LinesCount ) const override
    {
        return {};
    }
    klogg::vector<QString> doGetExpandedLines( LineNumber, LinesCount ) const override
    {
        return {};
    }
    LineNumber doGetLineNumber( LineNumber index ) const override
    {
        return index;
    }
    LinesCount doGetNbLine() const override
    {
        return 0_lcount;
    }
    LineLength doGetMaxLength() const override
    {
        return 0_length;
    }
    LineLength doGetLineLength( LineNumber ) const override
    {
        return 0_length;
    }
    void doSetDisplayEncoding( const char* ) override {}
    QTextCodec* doGetDisplayEncoding() const override
    {
        return QTextCodec::codecForName( "UTF-8" );
    }
    void doAttachReader() const override {}
    void doDetachReader() const override {}
};

struct SnapshotReadGate {
    explicit SnapshotReadGate( LineNumber line ) : blockedLine( line ) {}
    void beforeRead( LineNumber first )
    {
        if ( first != blockedLine ) return;
        std::unique_lock<std::mutex> lock( mutex );
        entered = true;
        changed.notify_all();
        changed.wait( lock, [ this ] { return released; } );
    }
    bool waitForEntry()
    {
        std::unique_lock<std::mutex> lock( mutex );
        return changed.wait_for( lock, std::chrono::seconds{ 5 }, [ this ] { return entered; } );
    }
    void release()
    {
        std::lock_guard<std::mutex> lock( mutex );
        released = true;
        changed.notify_all();
    }
    LineNumber blockedLine;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

struct ReleaseSnapshotGate {
    std::shared_ptr<SnapshotReadGate> gate;
    ~ReleaseSnapshotGate() { gate->release(); }
};

struct SnapshotSearchConfiguration {
    SnapshotSearchConfiguration()
    {
        Configuration::get().setSearchReadBufferSizeLines( 1 );
        Configuration::get().setUseParallelSearch( false );
        Configuration::get().setUseSearchResultsCache( false );
    }
    ~SnapshotSearchConfiguration()
    {
        Configuration::get().setSearchReadBufferSizeLines( chunkSize );
        Configuration::get().setUseParallelSearch( parallel );
        Configuration::get().setSearchThreadPoolSize( threads );
        Configuration::get().setUseSearchResultsCache( cached );
        Configuration::get().setUseBlockScan( blockScan );
        Configuration::get().setRegexpEnging( engine );
    }
    bool blockScan = Configuration::get().useBlockScan();
    RegexpEngine engine = Configuration::get().regexpEngine();
    int chunkSize = Configuration::get().searchReadBufferSizeLines();
    int threads = Configuration::get().searchThreadPoolSize();
    bool parallel = Configuration::get().useParallelSearch();
    bool cached = Configuration::get().useSearchResultsCache();
};

class FrozenSearchRows : public SearchableLogData::ReadSnapshot {
public:
    explicit FrozenSearchRows( QStringList lines, std::shared_ptr<SnapshotReadGate> gate = {},
                                bool failRead = false, uint64_t tailVersion = 0 )
        : lines_( std::move( lines ) ), gate_( std::move( gate ) ), failRead_( failRead ),
          tailVersion_( tailVersion ) {}
    std::optional<CacheKey> cacheKey() const override
    {
        return CacheKey{ klogg::LogRowId::Origin::Memory, 1, cacheVersion_,
                         QByteArrayLiteral( "UTF-8" ), {}, AnsiProcessingMode::Plain };
    }
    bool isAppendOf( const ReadSnapshot& previous ) const override
    {
        const auto* old = dynamic_cast<const FrozenSearchRows*>( &previous );
        if ( !old || lines_.size() < old->lines_.size() ) return false;
        for ( decltype( lines_.size() ) i = 0; i + 1 < old->lines_.size(); ++i ) {
            if ( lines_.at( i ) != old->lines_.at( i ) ) return false;
        }
        return true;
    }
    LinesCount lineCount() const override { return LinesCount{ static_cast<uint64_t>( lines_.size() ) }; }
    klogg::SnapshotRow row( LineNumber index ) const override
    {
        const auto member = index.get() + 1 == lineCount().get() ? tailVersion_ : uint64_t{ 0 };
        return { { klogg::LogRowId::Origin::Memory, 1, member, index.get() },
                 LineNumber{ index.get() + 1 }, true, false };
    }
    qint64 byteSize() const override { return 0; }
    void failReads() { failRead_ = true; }
    SearchableLogData::RawLines getLinesRaw( LineNumber first, LinesCount count ) const override
    {
        if ( gate_ ) gate_->beforeRead( first );
        if ( failRead_ ) throw klogg::SnapshotError{ "Injected immutable range read failure" };
        SearchableLogData::RawLines result;
        result.startLine = first;
        result.textDecoder = TextCodecHolder{ QTextCodec::codecForName( "UTF-8" ) }.makeDecoder();
        for ( auto i = first.get(); i < first.get() + count.get(); ++i ) {
            const auto bytes = lines_.at( static_cast<decltype( lines_.size() )>( i ) ).toUtf8() + '\n';
            result.buffer.insert( result.buffer.end(), bytes.cbegin(), bytes.cend() );
            result.endOfLines.push_back( static_cast<qint64>( result.buffer.size() ) );
        }
        return result;
    }
private:
    QStringList lines_;
    std::shared_ptr<SnapshotReadGate> gate_;
    bool failRead_ = false;
    uint64_t tailVersion_ = 0;
    static uint64_t nextCacheVersion()
    {
        static std::atomic<uint64_t> next{ 0 };
        return ++next;
    }
    uint64_t cacheVersion_ = nextCacheVersion();
};

class FrozenRawSearchRows : public SearchableLogData::ReadSnapshot {
public:
    FrozenRawSearchRows( QByteArray bytes, AnsiProcessingMode ansiMode,
                         QRegularExpression prefilter,
                         QTextCodec* codec = QTextCodec::codecForName( "UTF-8" ) )
        : bytes_( std::move( bytes ) ), ansiMode_( ansiMode ), prefilter_( std::move( prefilter ) ),
          codec_( codec )
    {
        for ( klogg::ContainerIndex i = 0; i < bytes_.size(); ++i ) {
            if ( bytes_.at( i ) == '\n' ) ends_.push_back( static_cast<qint64>( i ) + 1 );
        }
        if ( !bytes_.isEmpty() && bytes_.back() != '\n' ) {
            throw klogg::SnapshotError{ "Raw search fixture requires committed rows" };
        }
    }
    LinesCount lineCount() const override { return LinesCount{ ends_.size() }; }
    qint64 byteSize() const override { return static_cast<qint64>( bytes_.size() ); }
    klogg::SnapshotRow row( LineNumber index ) const override
    {
        return { { klogg::LogRowId::Origin::Memory, 2, 0, index.get() },
                 index + 1_lcount, true, false };
    }
    SearchableLogData::RawLines getLinesRaw( LineNumber first, LinesCount count ) const override
    {
        SearchableLogData::RawLines result;
        result.startLine = first;
        result.textDecoder = TextCodecHolder{ codec_ }.makeDecoder();
        result.ansiProcessingMode = ansiMode_;
        result.prefilterPattern = prefilter_;
        if ( count == 0_lcount ) return result;
        const auto start = first == 0_lnum ? qint64{ 0 } : ends_.at( static_cast<size_t>( first.get() - 1 ) );
        const auto end = ends_.at( static_cast<size_t>( first.get() + count.get() - 1 ) );
        result.buffer.assign( bytes_.constData() + start, bytes_.constData() + end );
        for ( auto i = first.get(); i < first.get() + count.get(); ++i ) {
            result.endOfLines.push_back( ends_.at( static_cast<size_t>( i ) ) - start );
        }
        return result;
    }
private:
    QByteArray bytes_;
    AnsiProcessingMode ansiMode_;
    QRegularExpression prefilter_;
    QTextCodec* codec_;
    klogg::vector<qint64> ends_;
};

// US-ASCII is optional in Qt's platform codec inventory. This fixture keeps
// the MIB-3 framing/interpretation distinction under test on every Qt build.
class AsciiSearchCodec final : public QTextCodec {
public:
    QByteArray name() const override { return QByteArrayLiteral( "Klogg-test-ASCII" ); }
    int mibEnum() const override { return 3; }
protected:
    QString convertToUnicode( const char* bytes, int length, ConverterState* ) const override
    {
        QString result;
        result.reserve( length );
        for ( int i = 0; i < length; ++i ) {
            const auto byte = static_cast<unsigned char>( bytes[ i ] );
            result.append( byte < 128 ? QChar::fromLatin1( bytes[ i ] )
                                      : QChar{ QChar::ReplacementCharacter } );
        }
        return result;
    }
    QByteArray convertFromUnicode( const QChar* characters, int length, ConverterState* ) const override
    {
        QByteArray result( length, '?' );
        for ( int i = 0; i < length; ++i ) {
            if ( characters[ i ].unicode() < 128 ) {
                result[ i ] = static_cast<char>( characters[ i ].unicode() );
            }
        }
        return result;
    }
};

class ThrowingSearchCodec final : public QTextCodec {
public:
    explicit ThrowingSearchCodec( std::shared_ptr<SnapshotReadGate> gate ) : gate_( std::move( gate ) ) {}
    QByteArray name() const override { return QByteArrayLiteral( "Klogg-test-search-fault" ); }
    int mibEnum() const override { return 29999; }
protected:
    QString convertToUnicode( const char*, int, ConverterState* ) const override
    {
        gate_->beforeRead( 0_lnum );
        throw klogg::SnapshotError{ "Injected matcher normalization failure" };
    }
    QByteArray convertFromUnicode( const QChar* characters, int length, ConverterState* ) const override
    {
        return QString{ characters, length }.toUtf8();
    }
private:
    std::shared_ptr<SnapshotReadGate> gate_;
};

class ParallelFaultRows final : public FrozenSearchRows {
public:
    enum class Fault { Producer, Matcher };
    explicit ParallelFaultRows( Fault fault )
        : FrozenSearchRows( makeRows() ), fault_( fault ),
          gate( std::make_shared<SnapshotReadGate>( 0_lnum ) ),
          codec_( std::make_unique<ThrowingSearchCodec>( gate ) ) {}
    SearchableLogData::RawLines getLinesRaw( LineNumber first, LinesCount count ) const override
    {
        if ( first == 1_lnum ) {
            if ( fault_ == Fault::Producer ) {
                faultReached.store( true );
                throw klogg::SnapshotError{ "Injected read failure after the first queued block" };
            }
            const bool entered = gate->waitForEntry();
            faultReached.store( entered );
            gate->release();
            if ( !entered ) throw klogg::SnapshotError{ "Matcher fault gate was not entered" };
        }
        auto result = FrozenSearchRows::getLinesRaw( first, count );
        if ( fault_ == Fault::Matcher && first == 0_lnum ) {
            result.textDecoder = TextCodecHolder{ codec_.get() }.makeDecoder();
        }
        return result;
    }
private:
    static QStringList makeRows()
    {
        QStringList rows;
        for ( int i = 0; i < 64; ++i ) rows.push_back( QStringLiteral( "ERROR value" ) );
        return rows;
    }
    Fault fault_;
public:
    std::shared_ptr<SnapshotReadGate> gate;
    mutable std::atomic<bool> faultReached{ false };
private:
    std::unique_ptr<ThrowingSearchCodec> codec_;
};

class SnapshotSearchSource : public TestSearchableLogData {
public:
    ReadSnapshot::Ptr version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "before" ), QStringLiteral( "ERROR old" ),
                     QStringLiteral( "after" ), QStringLiteral( "marked" ) } );
    bool live = false;
    bool failAcquisition = false;
    void changeInterpretation() { Q_EMIT interpretationChanged(); }
    ReadSnapshot::Ptr searchableSnapshot() const override
    {
        if ( failAcquisition ) throw klogg::SnapshotError{ "Injected snapshot acquisition failure" };
        return version;
    }
    RawLines getLinesRaw( LineNumber first, LinesCount count ) const override
    {
        return version->getLinesRaw( first, count );
    }
    bool isLiveSource() const override { return live; }
    OptionalLineNumber rowForIdentity( const klogg::LogRowId& identity ) const override
    {
        if ( identity.row < version->lineCount().get()
             && version->row( LineNumber{ identity.row } ).identity == identity ) {
            return LineNumber{ identity.row };
        }
        return {};
    }
protected:
    LinesCount doGetNbLine() const override { return version->lineCount(); }
    QString doGetLineString( LineNumber line ) const override { return version->lineString( line ); }
};

void finishFilteredSearch( LogFilteredData& filtered )
{
    QElapsedTimer deadline;
    deadline.start();
    while ( !filtered.isSearchComplete() && deadline.elapsed() < 5000 ) {
        QCoreApplication::processEvents();
        std::this_thread::yield();
    }
    REQUIRE( filtered.isSearchComplete() );
}

void checkParallelFaultTerminal( ParallelFaultRows::Fault fault, size_t concurrency )
{
    SnapshotSearchConfiguration configuration;
    Configuration::get().setUseParallelSearch( true );
    Configuration::get().setSearchThreadPoolSize( 2 );
    tbb::global_control workers{ tbb::global_control::max_allowed_parallelism, concurrency };
    SnapshotSearchSource source;
    const auto rows = std::make_shared<ParallelFaultRows>( fault );
    source.version = rows;
    LogFilteredDataWorker worker{ source };
    struct Cleanup {
        LogFilteredDataWorker& worker;
        std::shared_ptr<SnapshotReadGate> gate;
        ~Cleanup()
        {
            gate->release();
            worker.interrupt();
            worker.shutdownAndWait();
        }
    } cleanup{ worker, rows->gate };
    QSignalSpy progressed{ &worker, &LogFilteredDataWorker::searchProgressed };
    worker.search( RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, 0_lnum, 64_lnum );
    QElapsedTimer deadline;
    deadline.start();
    bool terminalBeforeUserStop = false;
    while ( !terminalBeforeUserStop && deadline.elapsed() < 5000 ) {
        QCoreApplication::processEvents();
        for ( const auto& arguments : progressed ) {
            terminalBeforeUserStop |= arguments.at( 1 ).toInt() == 100;
        }
        std::this_thread::yield();
    }
    // Always release/join before an assertion can unwind the fixture. An error
    // must terminate itself; the cleanup Stop is not evidence of success.
    rows->gate->release();
    worker.interrupt();
    worker.shutdownAndWait();
    REQUIRE( rows->faultReached.load() );
    REQUIRE( terminalBeforeUserStop );
    CHECK_FALSE( worker.completedSearchResult() );
}

// Unlike stringbuf, this sink exposes bytes only after the writer explicitly syncs.
class SearchTraceBuffer : public std::streambuf {
public:
    enum class Failure : std::uint8_t {
        None,
        ShortWrite,
        RejectWrite,
        FailedSync,
        ThrowWrite,
        ThrowSync
    };

    explicit SearchTraceBuffer( Failure failure = Failure::None )
        : failure_( failure )
    {
    }

    std::string committed;
    std::string pending;
    int writes = 0;
    int syncs = 0;

protected:
    std::streamsize xsputn( const char* bytes, std::streamsize count ) override
    {
        ++writes;
        if ( failure_ == Failure::ThrowWrite ) {
            throw std::runtime_error( "trace write failed" );
        }
        if ( failure_ == Failure::RejectWrite ) {
            return 0;
        }
        const auto accepted = failure_ == Failure::ShortWrite ? count - 1 : count;
        pending.append( bytes, static_cast<std::size_t>( accepted ) );
        return accepted;
    }

    int sync() override
    {
        ++syncs;
        if ( failure_ == Failure::ThrowSync ) {
            throw std::runtime_error( "trace sync failed" );
        }
        if ( failure_ == Failure::FailedSync ) {
            return -1;
        }
        committed += pending;
        pending.clear();
        return 0;
    }

private:
    Failure failure_;
};

class StderrCapture {
public:
    explicit StderrCapture( std::streambuf* buffer )
        : oldState_( std::cerr.rdstate() )
        , oldExceptions_( std::cerr.exceptions() )
        , oldBuffer_( std::cerr.rdbuf() )
    {
        std::cerr.exceptions( std::ios::goodbit );
        std::cerr.rdbuf( buffer );
    }

    ~StderrCapture()
    {
        std::cerr.exceptions( std::ios::goodbit );
        std::cerr.rdbuf( oldBuffer_ );
        std::cerr.clear( oldState_ );
        std::cerr.exceptions( oldExceptions_ );
    }

private:
    std::ios::iostate oldState_;
    std::ios::iostate oldExceptions_;
    std::streambuf* oldBuffer_ = nullptr;
};

class GroupedNumbers : public std::numpunct<char> {
protected:
    char do_thousands_sep() const override
    {
        return '_';
    }
    std::string do_grouping() const override
    {
        return "\3";
    }
};

class StderrFormatGuard {
public:
    StderrFormatGuard()
        : flags_( std::cerr.flags() )
        , precision_( std::cerr.precision() )
        , width_( std::cerr.width() )
        , fill_( std::cerr.fill() )
        , locale_( std::cerr.getloc() )
        , globalLocale_( std::locale() )
        , exceptions_( std::cerr.exceptions() )
        , tie_( std::cerr.tie() )
    {
    }

    ~StderrFormatGuard()
    {
        std::cerr.exceptions( std::ios::goodbit );
        std::cerr.flags( flags_ );
        std::cerr.precision( precision_ );
        std::cerr.width( width_ );
        std::cerr.fill( fill_ );
        std::cerr.imbue( locale_ );
        std::locale::global( globalLocale_ );
        std::cerr.tie( tie_ );
        std::cerr.exceptions( exceptions_ );
    }

private:
    std::ios::fmtflags flags_;
    std::streamsize precision_;
    std::streamsize width_;
    char fill_;
    std::locale locale_;
    std::locale globalLocale_;
    std::ios::iostate exceptions_;
    std::ostream* tie_;
};

constexpr auto MaxTraceRecord = "Search start compile.begin generation 18446744073709551615 "
                                "operation 18446744073709551615 type -1\n"
                                "Search start compile.end generation 18446744073709551615 "
                                "operation 18446744073709551615 type -1\n"
                                "Search start enqueue.request generation 18446744073709551615 "
                                "operation 18446744073709551615 type -1\n";

} // namespace

struct LogFilteredDataSnapshotPrivate {};

template <>
struct LogFilteredData::access_by<LogFilteredDataSnapshotPrivate> {
    static void waitForDone( LogFilteredData& data ) { data.workerThread_.waitForDone(); }
};

struct LogFilteredDataWorkerPrivate {};

template <>
struct LogFilteredDataWorker::access_by<LogFilteredDataWorkerPrivate> {
    static void enqueueOrDeferLiveRequest( LogFilteredDataWorker* worker, quint64 operationId,
                                           LineNumber endLine )
    {
        worker->enqueueOrDeferLiveRequest( makeLiveRequest( operationId, endLine ) );
    }

    static void enqueueImmediateLiveRequest( LogFilteredDataWorker* worker, quint64 operationId,
                                             LineNumber endLine )
    {
        worker->enqueueRequest( makeLiveRequest( operationId, endLine ), false );
    }

    static bool hasDeferredLiveRequest( LogFilteredDataWorker* worker )
    {
        std::lock_guard<std::mutex> lock( worker->requestMutex_ );
        return worker->deferredLiveRequest_.has_value();
    }

    static LineNumber deferredLiveEndLine( LogFilteredDataWorker* worker )
    {
        std::lock_guard<std::mutex> lock( worker->requestMutex_ );
        return worker->deferredLiveRequest_->endLine;
    }

    static quint64 deferredLiveOperationId( LogFilteredDataWorker* worker )
    {
        std::lock_guard<std::mutex> lock( worker->requestMutex_ );
        return worker->deferredLiveRequest_->operationId;
    }

    static void setActiveOperation( LogFilteredDataWorker* worker, quint64 generation,
                                    quint64 operationId )
    {
        worker->operationGeneration_.store( generation );
        worker->operationId_.store( operationId );
    }

    static bool hasExpectedFullRequest( LogFilteredDataWorker* worker, quint64 identity )
    {
        std::lock_guard<std::mutex> lock( worker->requestMutex_ );
        const auto& request = worker->pendingRequest_;
        return request && request->type == SearchRequest::Type::Full
               && request->generation == identity && request->operationId == identity
               && request->startLine == 0_lnum && request->endLine == 10_lnum
               && request->regExp.pattern == QStringLiteral( "ERROR" ) && request->compiled
               && request->compiled == worker->compiledExpression_;
    }

    static void queueTerminalProgress( LogFilteredDataWorker* worker, quint64 generation,
                                       quint64 operationId )
    {
        worker->emitSearchProgressedOnOwnerThread( 2_lcount, 100, 1_lnum, generation, operationId );
    }

private:
    static SearchRequest makeLiveRequest( quint64 operationId, LineNumber endLine )
    {
        return SearchRequest{ SearchRequest::Type::LiveUpdate,
                              RegularExpressionPattern{ QStringLiteral( "ERROR" ) },
                              0_lnum,
                              endLine,
                              0_lnum,
                              0,
                              operationId,
                              nullptr };
    }
};

TEST_CASE( "Filtered presentation contains retained source read failures without weakening snapshots",
           "[filtered-snapshot][filtered-presentation-failure]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    const auto retained = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "ERROR retained" ) } );
    source.version = retained;
    LogFilteredData filtered{ &source };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    LogFilteredData::access_by<LogFilteredDataSnapshotPrivate>::waitForDone( filtered );
    const auto matches = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( matches );
    REQUIRE( filtered.getLineString( 0_lnum ) == QStringLiteral( "ERROR retained" ) );

    // Retained storage can fail after successful search (for example, a truncated spill).
    retained->failReads();
    CHECK( filtered.getLineString( 0_lnum ).isEmpty() );
    CHECK( filtered.getExpandedLineString( 0_lnum ).isEmpty() );
    CHECK( filtered.getLineLength( 0_lnum ) == 0_length );
    CHECK( filtered.getLineAnsiColors( 0_lnum ).empty() );
    CHECK( filtered.getLines( 0_lnum, 1_lcount ) == klogg::vector<QString>{ QString{} } );
    CHECK( filtered.getExpandedLines( 0_lnum, 1_lcount ) == klogg::vector<QString>{ QString{} } );
    CHECK_THROWS_AS( matches->lineString( 0_lnum ), klogg::SnapshotError );
}

TEST_CASE( "Completed filtered snapshots retain source text and canonical matches",
           "[filtered-snapshot]" )
{
    SnapshotSearchSource source;
    LogFilteredData filtered{ &source };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    const auto matches = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( matches );
    REQUIRE( matches->lineCount() == 1_lcount );
    CHECK( matches->lineString( 0_lnum ) == QStringLiteral( "ERROR old" ) );
    CHECK( matches->row( 0_lnum ).sourceLineNumber == 2_lnum );
    CHECK( matches->row( 0_lnum ).match );

    filtered.addMark( 3_lnum );
    filtered.setContextLines( 1, 1 );
    const auto presented = filtered.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( presented );
    REQUIRE( presented->lineCount() == 4_lcount );
    CHECK_FALSE( presented->row( 0_lnum ).match );
    CHECK( presented->row( 0_lnum ).copyable );
    CHECK( presented->row( 3_lnum ).copyable );
    CHECK( filtered.rowForIdentity( matches->row( 0_lnum ).identity ) == 1_lnum );

    filtered.setContextLines( 0, 0 );
    filtered.setVisibility( LogFilteredData::VisibilityFlags::Marks );
    CHECK_FALSE( filtered.rowForIdentity( matches->row( 0_lnum ).identity ).has_value() );
    const auto hiddenMatches = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( hiddenMatches );
    CHECK( hiddenMatches->lineCount() == 1_lcount );
    CHECK( presented->lineCount() == 4_lcount );
    CHECK( presented->rowForIdentity( matches->row( 0_lnum ).identity ) == 1_lnum );
    CHECK( matches->rowForIdentity( matches->row( 0_lnum ).identity ) == 0_lnum );
    source.version = std::make_shared<FrozenSearchRows>( QStringList{ QStringLiteral( "replacement" ) } );
    CHECK( matches->lineString( 0_lnum ) == QStringLiteral( "ERROR old" ) );
    CHECK( presented->lineString( 3_lnum ) == QStringLiteral( "marked" ) );
}

TEST_CASE( "Same-generation filtered update revokes completed snapshot readiness",
           "[filtered-snapshot]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    LogFilteredData filtered{ &source };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    const auto generation = filtered.currentSearchGeneration();
    const auto gate = std::make_shared<SnapshotReadGate>( 3_lnum );
    ReleaseSnapshotGate release{ gate };
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "before" ), QStringLiteral( "ERROR old" ),
                     QStringLiteral( "after" ), QStringLiteral( "marked" ),
                     QStringLiteral( "ERROR new" ) }, gate );
    filtered.updateSearch( 0_lnum, 5_lnum );
    REQUIRE( gate->waitForEntry() );
    CHECK( filtered.currentSearchGeneration() == generation );
    CHECK_FALSE( filtered.isSearchComplete() );
    CHECK_FALSE( filtered.createReadSnapshot( klogg::SnapshotScope::Matches ) );
    filtered.interruptSearch();
    gate->release();
    LogFilteredData::access_by<LogFilteredDataSnapshotPrivate>::waitForDone( filtered );
    QCoreApplication::processEvents();
    CHECK_FALSE( filtered.isSearchComplete() );
    CHECK_FALSE( filtered.createReadSnapshot( klogg::SnapshotScope::Matches ) );
}

TEST_CASE( "Parallel producer failure drains queued blocks before destroying graph state",
           "[filtered-worker-failure][filtered-producer-failure]" )
{
    // No helper can consume the first accepted block before read #2 throws.
    // The graph must drain that queued task while all node/capture state lives.
    checkParallelFaultTerminal( ParallelFaultRows::Fault::Producer, 1 );
}

TEST_CASE( "Parallel matcher failure escapes producer backpressure without a user Stop",
           "[filtered-worker-failure][filtered-matcher-failure]" )
{
    // One helper is held inside the first matcher until the second source read.
    // It throws before any later block can return a limiter credit.
    checkParallelFaultTerminal( ParallelFaultRows::Fault::Matcher, 2 );
}

TEST_CASE( "Clearing a search cannot relabel its undrained partial results as current",
           "[filtered-snapshot][filtered-cancellation]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    const auto gate = std::make_shared<SnapshotReadGate>( 1_lnum );
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "ERROR first" ), QStringLiteral( "last" ) }, gate );
    LogFilteredData filtered{ &source };
    ReleaseSnapshotGate release{ gate };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    REQUIRE( gate->waitForEntry() );
    // The first block is internally published, but owner-thread progress has
    // deliberately not been delivered before retiring its generation.
    filtered.clearSearch( true );
    gate->release();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::MetaCall );
    CHECK( filtered.getNbMatches() == 0_lcount );
    CHECK_FALSE( filtered.isSearchComplete() );
}

TEST_CASE( "Interrupting an already completed or cached search preserves immutable readiness",
           "[filtered-snapshot][filtered-cancellation]" )
{
    SnapshotSearchConfiguration configuration;
    Configuration::get().setUseSearchResultsCache( true );
    SnapshotSearchSource source;
    LogFilteredData filtered{ &source };
    const RegularExpressionPattern pattern{ QStringLiteral( "ERROR" ) };
    filtered.runSearch( pattern );
    finishFilteredSearch( filtered );
    SECTION( "Completed worker execution" ) {}
    SECTION( "Completed cache hit" ) {
        const auto starts = filtered.searchPerformanceCounters().operationStarts;
        filtered.runSearch( pattern );
        REQUIRE( filtered.isSearchComplete() );
        REQUIRE( filtered.searchPerformanceCounters().operationStarts == starts );
    }
    const auto previous = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( previous );
    filtered.interruptSearch();
    CHECK( filtered.isSearchComplete() );
    const auto current = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( current );
    CHECK( current->lineString( 0_lnum ) == previous->lineString( 0_lnum ) );
    QCoreApplication::processEvents();
    CHECK( filtered.isSearchComplete() );
}

TEST_CASE( "Partial and stopped filtered presentations remain snapshot-capable",
           "[filtered-snapshot][filtered-presented-readiness]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    LogFilteredData filtered{ &source };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    const auto previous = filtered.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( previous );
    const auto gate = std::make_shared<SnapshotReadGate>( 3_lnum );
    ReleaseSnapshotGate release{ gate };
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "before" ), QStringLiteral( "ERROR old" ),
                     QStringLiteral( "after" ), QStringLiteral( "marked" ),
                     QStringLiteral( "ERROR appended" ) }, gate );
    filtered.updateSearch( 0_lnum, 5_lnum );
    REQUIRE( gate->waitForEntry() );
    CHECK_FALSE( filtered.createReadSnapshot( klogg::SnapshotScope::Matches ) );
    const auto partial = filtered.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( partial );
    CHECK( partial->lineString( 0_lnum ) == QStringLiteral( "ERROR old" ) );
    CHECK( partial->preservesPresentationOf( *previous ) );
    filtered.interruptSearch();
    gate->release();
    QCoreApplication::processEvents();
    CHECK_FALSE( filtered.isSearchComplete() );
    CHECK_FALSE( filtered.createReadSnapshot( klogg::SnapshotScope::Matches ) );
    const auto stopped = filtered.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( stopped );
    CHECK( stopped->lineString( 0_lnum ) == QStringLiteral( "ERROR old" ) );
}

TEST_CASE( "Appended marked rows use current presented text without replacing accepted canonical matches",
           "[filtered-snapshot][filtered-presented-readiness]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    LogFilteredData filtered{ &source };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    const auto canonical = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( canonical );
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "before" ), QStringLiteral( "ERROR old" ),
                     QStringLiteral( "after" ), QStringLiteral( "marked" ),
                     QStringLiteral( "new marked row" ) } );
    // No updateSearch: automatic search refresh is off.
    filtered.addMark( 4_lnum );
    CHECK( filtered.getLineString( 1_lnum ) == QStringLiteral( "new marked row" ) );
    const auto presented = filtered.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( presented );
    REQUIRE( presented->lineCount() == 2_lcount );
    CHECK( presented->lineString( 1_lnum ) == QStringLiteral( "new marked row" ) );
    CHECK( filtered.isSearchComplete() );
    const auto accepted = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( accepted );
    CHECK( accepted->lineCount() == canonical->lineCount() );
    CHECK( accepted->lineString( 0_lnum ) == canonical->lineString( 0_lnum ) );
}

TEST_CASE( "Readable snapshots prove sealed prefix preservation but reject mutable tail identities",
           "[filtered-snapshot][match-layout-work]" )
{
    FrozenSearchRows sealed{ QStringList{ QStringLiteral( "first" ), QStringLiteral( "last" ) } };
    FrozenSearchRows appended{ QStringList{ QStringLiteral( "first" ), QStringLiteral( "last" ),
                                            QStringLiteral( "next" ) } };
    CHECK( appended.preservesPresentationOf( sealed ) );
    CHECK( sealed.preservesPresentationOf( sealed ) );
    CHECK_FALSE( sealed.preservesPresentationOf( appended ) );
    FrozenSearchRows tail{ QStringList{ QStringLiteral( "first" ), QStringLiteral( "last" ) },
                           {}, false, 1 };
    REQUIRE( appended.isAppendOf( tail ) );
    CHECK_FALSE( appended.preservesPresentationOf( tail ) );
    FrozenSearchRows changedTail{ QStringList{ QStringLiteral( "first" ), QStringLiteral( "changed" ) },
                                  {}, false, 2 };
    REQUIRE( changedTail.isAppendOf( tail ) );
    CHECK_FALSE( changedTail.preservesPresentationOf( tail ) );
    FrozenSearchRows replacement{ QStringList{ QStringLiteral( "replacement" ),
                                               QStringLiteral( "last" ), QStringLiteral( "next" ) } };
    CHECK_FALSE( replacement.preservesPresentationOf( sealed ) );
}

TEST_CASE( "Filtered append proof rejects newly visible rows anywhere below the old source cutoff",
           "[filtered-snapshot][match-layout-work]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    LogFilteredData filtered{ &source };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    const auto previous = filtered.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( previous );
    REQUIRE( previous->lineCount() == 1_lcount );
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "before" ), QStringLiteral( "ERROR old" ),
                     QStringLiteral( "after" ), QStringLiteral( "marked" ),
                     QStringLiteral( "ERROR appended" ) } );
    filtered.updateSearch( 0_lnum, 5_lnum );
    finishFilteredSearch( filtered );
    const auto appended = filtered.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( appended );
    CHECK( appended->preservesPresentationOf( *previous ) );
    // This row was AFTER the old last visible match, but still inside the old
    // source cutoff. A last-visible-row-only proof would incorrectly accept it.
    filtered.addMark( 3_lnum );
    const auto revealed = filtered.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( revealed );
    CHECK_FALSE( revealed->preservesPresentationOf( *previous ) );
    filtered.setVisibility( LogFilteredData::VisibilityFlags::Marks );
    const auto hidden = filtered.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( hidden );
    CHECK_FALSE( hidden->preservesPresentationOf( *previous ) );
}

TEST_CASE( "Filtered prefix proof rejects an edited mutable tail even when membership is unchanged",
           "[filtered-snapshot][match-layout-work]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "before" ), QStringLiteral( "ERROR tail" ) },
        std::shared_ptr<SnapshotReadGate>{}, false, 1 );
    LogFilteredData filtered{ &source };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    const auto previous = filtered.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( previous );
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "before" ), QStringLiteral( "ERROR changed" ) },
        std::shared_ptr<SnapshotReadGate>{}, false, 2 );
    filtered.updateSearch( 0_lnum, 2_lnum );
    finishFilteredSearch( filtered );
    const auto current = filtered.createReadSnapshot( klogg::SnapshotScope::PresentedRows );
    REQUIRE( current );
    REQUIRE( current->lineCount() == previous->lineCount() );
    CHECK_FALSE( current->preservesPresentationOf( *previous ) );
}

TEST_CASE( "Hidden filtered rows still exist independently of presentation visibility",
           "[filtered-snapshot][filtered-interpretation]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    LogFilteredData filtered{ &source };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    const auto matches = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( matches );
    const auto identity = matches->row( 0_lnum ).identity;
    filtered.addMark( 3_lnum );
    filtered.setVisibility( LogFilteredData::VisibilityFlags::Marks );
    CHECK_FALSE( filtered.rowForIdentity( identity ) );
    CHECK( filtered.containsRowIdentity( identity ) );
    CHECK( matches->lineString( 0_lnum ) == QStringLiteral( "ERROR old" ) );
    source.version = std::make_shared<FrozenSearchRows>( QStringList{ QStringLiteral( "replacement" ) } );
    CHECK_FALSE( filtered.containsRowIdentity( identity ) );
}

TEST_CASE( "Source interpretation changes revoke readiness and notify the filtered owner",
           "[filtered-snapshot][filtered-interpretation]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    LogFilteredData filtered{ &source };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    const auto retained = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( retained );
    QSignalSpy changed{ &filtered, &AbstractLogData::interpretationChanged };
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "before" ), QStringLiteral( "normalized differently" ),
                     QStringLiteral( "after" ), QStringLiteral( "marked" ) } );
    source.changeInterpretation();
    CHECK( changed.count() == 1 );
    CHECK_FALSE( filtered.isSearchComplete() );
    CHECK_FALSE( filtered.createReadSnapshot( klogg::SnapshotScope::Matches ) );
    QCoreApplication::processEvents();
    CHECK_FALSE( filtered.isSearchComplete() );
    CHECK( retained->lineString( 0_lnum ) == QStringLiteral( "ERROR old" ) );
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    CHECK( filtered.getNbMatches() == 0_lcount );
}

TEST_CASE( "Completed incremental membership removes a changed final-row match",
           "[filtered-snapshot]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "ERROR retained" ), QStringLiteral( "ERROR tail" ) } );
    LogFilteredData filtered{ &source };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    const auto previous = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    const auto generation = filtered.currentSearchGeneration();
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "ERROR retained" ), QStringLiteral( "changed tail" ),
                     QStringLiteral( "ERROR appended" ) } );
    filtered.updateSearch( 0_lnum, 3_lnum );
    CHECK_FALSE( filtered.isSearchComplete() );
    finishFilteredSearch( filtered );
    const auto current = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( current );
    REQUIRE( current->lineCount() == 2_lcount );
    CHECK( filtered.getNbMatches() == 2_lcount );
    CHECK( current->row( 1_lnum ).sourceLineNumber == 3_lnum );
    CHECK( current->lineString( 1_lnum ) == QStringLiteral( "ERROR appended" ) );
    CHECK( previous->lineString( 1_lnum ) == QStringLiteral( "ERROR tail" ) );
    CHECK( filtered.currentSearchGeneration() == generation );
}

TEST_CASE( "Live fixed-prefix progress remains observable while a newer snapshot is pending",
           "[filtered-snapshot]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    source.live = true;
    LogFilteredData filtered{ &source };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    const auto generation = filtered.currentSearchGeneration();
    const auto gate = std::make_shared<SnapshotReadGate>( 4_lnum );
    ReleaseSnapshotGate release{ gate }; // Unblock before filtered/worker destruction on assertion failure.
    const auto prefix = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "before" ), QStringLiteral( "ERROR old" ),
                     QStringLiteral( "after" ), QStringLiteral( "ERROR prefix" ),
                     QStringLiteral( "ERROR middle" ), QStringLiteral( "plain" ) }, gate );
    source.version = prefix;
    QSignalSpy progressed{ &filtered, &LogFilteredData::searchProgressed };
    filtered.updateSearch( 0_lnum, 6_lnum );
    REQUIRE( gate->waitForEntry() );
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "before" ), QStringLiteral( "ERROR old" ),
                     QStringLiteral( "after" ), QStringLiteral( "ERROR prefix" ),
                     QStringLiteral( "changed" ), QStringLiteral( "plain" ),
                     QStringLiteral( "ERROR newest" ) } );
    filtered.updateSearch( 0_lnum, 7_lnum );
    QElapsedTimer deadline;
    deadline.start();
    while ( progressed.isEmpty() && deadline.elapsed() < 5000 ) {
        QCoreApplication::processEvents();
        std::this_thread::yield();
    }
    REQUIRE_FALSE( progressed.isEmpty() );
    for ( const auto& arguments : progressed ) CHECK( arguments.at( 1 ).toInt() < 100 );
    CHECK( filtered.getNbMatches() >= 2_lcount );
    CHECK_FALSE( filtered.isSearchComplete() );
    CHECK( filtered.currentSearchGeneration() == generation );
    CHECK( prefix->lineCount() == 6_lcount );
    gate->release();
    finishFilteredSearch( filtered );
    const auto matches = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( matches );
    REQUIRE( matches->lineCount() == 3_lcount );
    CHECK( matches->row( 2_lnum ).sourceLineNumber == 7_lnum );
    CHECK( matches->lineString( 2_lnum ) == QStringLiteral( "ERROR newest" ) );
    CHECK( prefix->lineString( 4_lnum ) == QStringLiteral( "ERROR middle" ) );
    CHECK( filtered.currentSearchGeneration() == generation );
}

TEST_CASE( "A dispatched filtered search cannot read a newer mutable source version",
           "[filtered-snapshot]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    const auto gate = std::make_shared<SnapshotReadGate>( 0_lnum );
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "first" ), QStringLiteral( "ERROR captured" ) }, gate );
    LogFilteredData filtered{ &source };
    ReleaseSnapshotGate release{ gate };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    REQUIRE( gate->waitForEntry() );
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "replacement" ), QStringLiteral( "not a match" ),
                     QStringLiteral( "ERROR later arrival" ) } );
    gate->release();
    finishFilteredSearch( filtered );
    const auto completed = filtered.completedSearchResult();
    REQUIRE( completed );
    CHECK( completed->processedLines == 2_lcount );
    CHECK( completed->source->lineCount() == 2_lcount );
    CHECK( filtered.getLineString( 0_lnum ) == QStringLiteral( "ERROR captured" ) );
    const auto matches = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( matches );
    CHECK( matches->lineCount() == 1_lcount );
    CHECK( matches->lineString( 0_lnum ) == QStringLiteral( "ERROR captured" ) );
}

TEST_CASE( "Snapshot acquisition and read errors terminate without publishing ready partial results",
           "[filtered-snapshot]" )
{
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    LogFilteredData filtered{ &source };
    filtered.runSearch( RegularExpressionPattern{ QStringLiteral( "ERROR" ) } );
    finishFilteredSearch( filtered );
    QSignalSpy progressed{ &filtered, &LogFilteredData::searchProgressed };
    SECTION( "Acquisition failure" ) { source.failAcquisition = true; }
    SECTION( "Immutable range read failure" ) {
        source.version = std::make_shared<FrozenSearchRows>(
            QStringList{ QStringLiteral( "before" ), QStringLiteral( "ERROR old" ),
                         QStringLiteral( "after" ), QStringLiteral( "ERROR broken" ) },
            std::shared_ptr<SnapshotReadGate>{}, true );
    }
    filtered.updateSearch( 0_lnum, 4_lnum );
    QElapsedTimer deadline;
    deadline.start();
    bool terminal = false;
    while ( !terminal && deadline.elapsed() < 5000 ) {
        QCoreApplication::processEvents();
        for ( const auto& arguments : progressed ) terminal |= arguments.at( 1 ).toInt() == 100;
        std::this_thread::yield();
    }
    REQUIRE( terminal );
    CHECK_FALSE( filtered.isSearchComplete() );
    CHECK_FALSE( filtered.createReadSnapshot( klogg::SnapshotScope::Matches ) );
}

TEST_CASE( "Stale same-generation terminal cannot make a newer stopped execution ready",
           "[filtered-snapshot]" )
{
    using WorkerVisitor = LogFilteredDataWorker::access_by<LogFilteredDataWorkerPrivate>;
    SnapshotSearchConfiguration configuration;
    SnapshotSearchSource source;
    LogFilteredDataWorker worker{ source };
    QSignalSpy progressed{ &worker, &LogFilteredDataWorker::searchProgressed };
    const RegularExpressionPattern pattern{ QStringLiteral( "ERROR" ) };
    worker.search( pattern, 0_lnum, 4_lnum );
    QElapsedTimer deadline;
    deadline.start();
    while ( !worker.completedSearchResult() && deadline.elapsed() < 5000 ) {
        QCoreApplication::processEvents();
        std::this_thread::yield();
    }
    const auto initial = worker.completedSearchResult();
    REQUIRE( initial );
    progressed.clear();
    const auto gate = std::make_shared<SnapshotReadGate>( 3_lnum );
    ReleaseSnapshotGate release{ gate };
    source.version = std::make_shared<FrozenSearchRows>(
        QStringList{ QStringLiteral( "before" ), QStringLiteral( "ERROR old" ),
                     QStringLiteral( "after" ), QStringLiteral( "marked" ),
                     QStringLiteral( "ERROR new" ) }, gate );
    worker.updateSearch( pattern, 0_lnum, 5_lnum, 4_lnum );
    REQUIRE( gate->waitForEntry() );
    // Completion publication can precede the prior operation's final unwind;
    // a safe full-scan fallback may publish rows 0-2 before this gate. Drain
    // that legitimate progress before isolating the injected stale terminal.
    QCoreApplication::sendPostedEvents( &worker, QEvent::MetaCall );
    for ( const auto& arguments : progressed ) CHECK( arguments.at( 1 ).toInt() < 100 );
    progressed.clear();
    WorkerVisitor::queueTerminalProgress( &worker, initial->generation, initial->operationId );
    QCoreApplication::sendPostedEvents( &worker, QEvent::MetaCall );
    CHECK( progressed.isEmpty() );
    CHECK_FALSE( worker.completedSearchResult() );
    CHECK( worker.currentGeneration() == initial->generation );
    worker.interrupt();
    gate->release();
    worker.waitForDone();
    QCoreApplication::sendPostedEvents( &worker, QEvent::MetaCall );
    CHECK_FALSE( worker.completedSearchResult() );
}

TEST_CASE( "Block capability keeps common local patterns and refuses unsafe record semantics",
           "[filtered-snapshot][filtered-backend-parity]" )
{
    SnapshotSearchConfiguration configuration;
    auto& config = Configuration::get();
    configureProductLikeRegexpEngine( config );
    if ( config.regexpEngine() != RegexpEngine::Vectorscan ) return;
    const auto expression = GENERATE( "value$", "^ERROR: [A-Z0-9]+$", "a\\d+b", "a[^\\n]+b",
                                      "\\Avalue\\z", "a\\s+b", "a[^x]+b", "(?s)a.*b" );
    const auto pattern = QString::fromLatin1( expression );
    const bool expected = pattern == QStringLiteral( "value$" )
        || pattern == QStringLiteral( "^ERROR: [A-Z0-9]+$" )
        || pattern == QStringLiteral( "a\\d+b" ) || pattern == QStringLiteral( "a[^\\n]+b" );
    RegularExpression compiled{ RegularExpressionPattern{ pattern } };
    INFO( "pattern=" << expression );
    CHECK( compiled.createMatcher()->hasBufferScan() == expected );
}

TEST_CASE( "Shared UTF8 borrow eligibility preserves literal interior controls and headers",
           "[filtered-snapshot][filtered-backend-parity]" )
{
    auto* utf8Codec = QTextCodec::codecForName( "UTF-8" );
    REQUIRE( utf8Codec );
    AsciiSearchCodec asciiCodec;
    const EncodingParameters utf8{ utf8Codec };
    const EncodingParameters ascii{ &asciiCodec };
    using RawLines = SearchableLogData::RawLines;
    CHECK( RawLines::canBorrowNormalizedUtf8( "value\n", utf8 ) );
    CHECK( RawLines::canBorrowNormalizedUtf8( "value\n", ascii ) );
    CHECK( RawLines::canBorrowNormalizedUtf8( "value", utf8 ) );
    CHECK( RawLines::canBorrowNormalizedUtf8( "\xc3\xa9value\n", utf8 ) );
    CHECK_FALSE( RawLines::canBorrowNormalizedUtf8( "\xc3\xa9value\n", ascii ) );
    CHECK_FALSE( RawLines::canBorrowNormalizedUtf8( "\xff" "value\n", utf8 ) );
    CHECK_FALSE( RawLines::canBorrowNormalizedUtf8( "value\r\n", utf8 ) );
    CHECK_FALSE( RawLines::canBorrowNormalizedUtf8( "value\r", utf8 ) );
    CHECK( RawLines::canBorrowNormalizedUtf8( "left\rright\n", utf8 ) );
    CHECK_FALSE( RawLines::canBorrowNormalizedUtf8( "\xef\xbb\xbfvalue\n", utf8 ) );
    CHECK_FALSE( RawLines::canBorrowNormalizedUtf8( "first\n\xef\xbb\xbfvalue\n", utf8 ) );
    CHECK( RawLines::canBorrowNormalizedUtf8( "left\xef\xbb\xbfright\n", utf8 ) );
}

TEST_CASE( "Block scanner preserves consuming and zero-width row ownership at LF and EOF",
           "[filtered-snapshot][filtered-backend-parity]" )
{
    SnapshotSearchConfiguration configuration;
    configureProductLikeRegexpEngine( Configuration::get() );
    const auto expression = GENERATE( "value$", "^$", "^", "tail$", "\\b" );
    const bool terminated = GENERATE( false, true );
    RegularExpression compiled{ RegularExpressionPattern{ QString::fromLatin1( expression ) } };
    auto matcher = compiled.createMatcher();
    const klogg::vector<std::string_view> rows{ "", "value", "", "tail" };
    klogg::vector<uint64_t> expected;
    for ( size_t i = 0; i < rows.size(); ++i ) {
        if ( matcher->hasMatch( rows[ i ] ) ) expected.push_back( static_cast<uint64_t>( i ) );
    }
    if ( !matcher->hasBufferScan() ) return; // SOM-incompatible patterns retain per-row matching.
    const QByteArray bytes = terminated ? QByteArrayLiteral( "\nvalue\n\ntail\n" )
                                        : QByteArrayLiteral( "\nvalue\n\ntail" );
    const klogg::vector<qint64> ends{ 1, 7, 8, static_cast<qint64>( bytes.size() ) };
    klogg::vector<uint64_t> actual;
    REQUIRE( matcher->scanBuffer( bytes.constData(), static_cast<unsigned int>( bytes.size() ),
                                  ends, actual ) );
    INFO( "pattern=" << expression << " terminated=" << terminated );
    CHECK( actual == expected );
}

TEST_CASE( "Batch and single-row decoding independently replace incomplete UTF8 records",
           "[filtered-snapshot][filtered-backend-parity]" )
{
    auto* codec = QTextCodec::codecForName( "UTF-8" );
    REQUIRE( codec );
    FrozenRawSearchRows source{ QByteArrayLiteral( "\xe2\n\x82\xac\n" ),
                                AnsiProcessingMode::Plain, {}, codec };
    const auto first = codec->toUnicode( QByteArrayLiteral( "\xe2" ) );
    const auto second = codec->toUnicode( QByteArrayLiteral( "\x82\xac" ) );
    REQUIRE_FALSE( first.isEmpty() );
    REQUIRE_FALSE( second.isEmpty() );
    const auto raw = source.getLinesRaw( 0_lnum, 2_lcount );
    const auto batch = raw.decodeLinesStrict();
    REQUIRE( batch.size() == 2 );
    CHECK( batch[ 0 ] == first );
    CHECK( batch[ 1 ] == second );
    CHECK( source.lineString( 0_lnum ) == first );
    CHECK( source.lineString( 1_lnum ) == second );
    const auto block = raw.buildUtf8Block();
    REQUIRE( block.lines.size() == 2 );
    CHECK( QString::fromUtf8( block.lines[ 0 ].data(), static_cast<int>( block.lines[ 0 ].size() ) ) == first );
    CHECK( QString::fromUtf8( block.lines[ 1 ].data(), static_cast<int>( block.lines[ 1 ].size() ) ) == second );
}

TEST_CASE( "Normalized UTF8 block borrows plain records and frames transformed records",
           "[filtered-snapshot][filtered-backend-parity]" )
{
    FrozenRawSearchRows plain{ QByteArrayLiteral( "value\n\n" ), AnsiProcessingMode::Plain, {} };
    const auto raw = plain.getLinesRaw( 0_lnum, 2_lcount );
    const auto block = raw.buildUtf8Block();
    CHECK( block.buffer.data() == raw.buffer.data() );
    REQUIRE( block.lines.size() == 2 );
    CHECK( block.lines[ 0 ] == "value" );
    CHECK( block.lines[ 1 ].empty() );
    CHECK( block.endOfLines == raw.endOfLines );

    FrozenRawSearchRows crlf{ QByteArrayLiteral( "value\r\n\r\n" ), AnsiProcessingMode::Plain, {} };
    const auto windowsRaw = crlf.getLinesRaw( 0_lnum, 2_lcount );
    const auto normalized = windowsRaw.buildUtf8Block();
    CHECK( normalized.buffer == "value\n\n" );
    REQUIRE( normalized.endOfLines.size() == 2 );
    CHECK( normalized.endOfLines[ 0 ] == 6 );
    CHECK( normalized.endOfLines[ 1 ] == 7 );
    REQUIRE( normalized.lines.size() == 2 );
    CHECK( normalized.lines[ 0 ] == "value" );
    CHECK( normalized.lines[ 1 ].empty() );
}

TEST_CASE( "Worker backends search the same normalized snapshot records",
           "[filtered-snapshot][filtered-backend-parity]" )
{
    SnapshotSearchConfiguration configuration;
    auto& config = Configuration::get();
    configureProductLikeRegexpEngine( config );
    config.setSearchReadBufferSizeLines( 32 );
    const bool blockScan = GENERATE( false, true );
    config.setUseBlockScan( blockScan );
    CAPTURE( blockScan );

    QByteArray bytes;
    QString expression;
    AnsiProcessingMode ansiMode = AnsiProcessingMode::Plain;
    QRegularExpression prefilter;
    AsciiSearchCodec asciiCodec;
    QTextCodec* codec = QTextCodec::codecForName( "UTF-8" );
    REQUIRE( codec );
    klogg::vector<LineNumber> expectedRows;
    QStringList expectedText;
    bool requiresBlockCapability = false;
    SECTION( "CRLF end anchors use displayed text" ) {
        bytes = QByteArrayLiteral( "value\r\nvalue extra\r\nvalue\r\n" );
        expression = QStringLiteral( "value$" );
        expectedRows = { 1_lnum, 3_lnum };
        expectedText = QStringList{ QStringLiteral( "value" ), QStringLiteral( "value" ) };
        requiresBlockCapability = true;
    }
    SECTION( "Empty committed rows retain their source ordinals" ) {
        bytes = QByteArrayLiteral( "\nvalue\n\n" );
        expression = QStringLiteral( "^$" );
        expectedRows = { 1_lnum, 3_lnum };
        expectedText = QStringList{ QString{}, QString{} };
    }
    SECTION( "ANSI and anchored prefilters apply independently to every record" ) {
        bytes = QByteArrayLiteral( "\x1b[31mprefix value\x1b[0m\r\n"
                                   "\x1b[32mprefix value\x1b[0m\r\n"
                                   "prefix other\r\n" );
        ansiMode = AnsiProcessingMode::Strip;
        prefilter = QRegularExpression{ QStringLiteral( "^prefix " ) };
        expression = QStringLiteral( "^value$" );
        expectedRows = { 1_lnum, 2_lnum };
        expectedText = QStringList{ QStringLiteral( "value" ), QStringLiteral( "value" ) };
    }
    SECTION( "Malformed UTF8 uses the configured Qt replacement text" ) {
        bytes = QByteArrayLiteral( "\xff" "value\n\xff" "other\n" );
        const auto replacement = QString{ QChar{ 0xfffd } };
        expression = QStringLiteral( "^" ) + replacement + QStringLiteral( "value$" );
        expectedRows = { 1_lnum };
        expectedText = QStringList{ replacement + QStringLiteral( "value" ) };
    }
    SECTION( "Absolute start and strict end anchors apply to every record" ) {
        bytes = QByteArrayLiteral( "value\nvalue\n" );
        expression = QStringLiteral( "\\Avalue\\z" );
        expectedRows = { 1_lnum, 2_lnum };
        expectedText = QStringList{ QStringLiteral( "value" ), QStringLiteral( "value" ) };
    }
    SECTION( "Absolute end-before-newline anchors apply to every record" ) {
        bytes = QByteArrayLiteral( "value\nvalue\n" );
        expression = QStringLiteral( "\\Avalue\\Z" );
        expectedRows = { 1_lnum, 2_lnum };
        expectedText = QStringList{ QStringLiteral( "value" ), QStringLiteral( "value" ) };
    }
    SECTION( "Whitespace patterns cannot consume separators between physical rows" ) {
        bytes = QByteArrayLiteral( "a\nb\n" );
        expression = QStringLiteral( "a\\s+b" );
    }
    SECTION( "An earlier cross-record start cannot hide a valid same-end local alternative" ) {
        bytes = QByteArrayLiteral( "a\nb\nab\n" );
        expression = QStringLiteral( "a\\s+b|b" );
        expectedRows = { 2_lnum, 3_lnum };
        expectedText = QStringList{ QStringLiteral( "b" ), QStringLiteral( "ab" ) };
    }
    SECTION( "An ASCII display codec does not interpret non-ASCII UTF8 as Unicode" ) {
        codec = &asciiCodec;
        const auto record = QByteArrayLiteral( "\xc3\xa9value" );
        bytes = record + '\n';
        const auto text = codec->toUnicode( record );
        REQUIRE( text == QString( 2, QChar{ QChar::ReplacementCharacter } ) + QStringLiteral( "value" ) );
        REQUIRE( text != QString::fromUtf8( record ) );
        expression = QStringLiteral( "^" ) + QRegularExpression::escape( text ) + QStringLiteral( "$" );
        expectedRows = { 1_lnum };
        expectedText = QStringList{ text };
    }
    SECTION( "UTF8 headers follow the same per-record Qt decoding policy" ) {
        bytes = QByteArrayLiteral( "\xef\xbb\xbfvalue\n\xef\xbb\xbfvalue\n" );
        expression = QStringLiteral( "^value$" );
        expectedRows = { 1_lnum, 2_lnum };
        expectedText = QStringList{ QStringLiteral( "value" ), QStringLiteral( "value" ) };
    }
    SECTION( "Only the CR paired with LF is removed from source text" ) {
        bytes = QByteArrayLiteral( "value\r\r\n" );
        expression = QStringLiteral( "value\\r$" );
        expectedRows = { 1_lnum };
        expectedText = QStringList{ QStringLiteral( "value\r" ) };
    }
    SECTION( "Incomplete UTF8 records are replaced rather than combined across LF" ) {
        bytes = QByteArrayLiteral( "\xe2\n\x82\xac\n" );
        const auto text = codec->toUnicode( QByteArrayLiteral( "\xe2" ) );
        REQUIRE_FALSE( text.isEmpty() );
        expression = QStringLiteral( "^" ) + QRegularExpression::escape( text ) + QStringLiteral( "$" );
        expectedRows = { 1_lnum };
        expectedText = QStringList{ text };
    }
    INFO( "blockScan=" << blockScan << " pattern=" << expression.toStdString()
          << " bytes=" << bytes.toHex().toStdString() );
    const RegularExpressionPattern pattern{ expression };
    if ( requiresBlockCapability ) {
        RegularExpression compiled{ pattern };
        const bool available = compiled.createMatcher()->hasBufferScan();
        CHECK( available == ( config.regexpEngine() == RegexpEngine::Vectorscan ) );
    }
    SnapshotSearchSource source;
    source.version = std::make_shared<FrozenRawSearchRows>( bytes, ansiMode, prefilter, codec );
    LogFilteredData filtered{ &source };
    filtered.runSearch( pattern );
    finishFilteredSearch( filtered );
    const auto matches = filtered.createReadSnapshot( klogg::SnapshotScope::Matches );
    REQUIRE( matches );
    CHECK( matches->lineCount().get() == expectedRows.size() );
    for ( size_t i = 0; i < expectedRows.size(); ++i ) {
        // Check the normalization oracle even when backend membership is wrong.
        CHECK( source.version->lineString( expectedRows[ i ] - 1_lcount )
               == expectedText.at( static_cast<klogg::ContainerIndex>( i ) ) );
        if ( i < matches->lineCount().get() ) {
            CHECK( matches->row( LineNumber{ i } ).sourceLineNumber == expectedRows[ i ] );
            CHECK( matches->lineString( LineNumber{ i } )
                   == expectedText.at( static_cast<klogg::ContainerIndex>( i ) ) );
        }
    }
}

TEST_CASE( "Search start trace empty-mode child receives a present empty opt-in",
           "[.search-start-empty]" )
{
    REQUIRE( qEnvironmentVariableIsSet( "KLOGG_TEST_TRACE_SEARCH_STARTS" ) );
    REQUIRE( qgetenv( "KLOGG_TEST_TRACE_SEARCH_STARTS" ).isEmpty() );
}

TEST_CASE( "Search start trace emits and flushes the real maximum-identity request",
           "[.search-start-enabled]" )
{
    using WorkerVisitor = LogFilteredDataWorker::access_by<LogFilteredDataWorkerPrivate>;
    REQUIRE( qEnvironmentVariableIsSet( "KLOGG_TEST_TRACE_SEARCH_STARTS" ) );
    TestSearchableLogData sourceLogData;
    LogFilteredDataWorker worker( sourceLogData );
    worker.shutdownAndWait();
    const auto maximum = std::numeric_limits<quint64>::max();
    WorkerVisitor::setActiveOperation( &worker, maximum - 1, maximum - 1 );

    SearchTraceBuffer buffer;
    bool flushedBeforeReturn = false;
    {
        StderrCapture capture( &buffer );
        worker.search( RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, 0_lnum, 10_lnum );
        flushedBeforeReturn = buffer.pending.empty() && buffer.committed == MaxTraceRecord;
    }
    CHECK( flushedBeforeReturn );
    CHECK( buffer.committed == MaxTraceRecord );
    CHECK( buffer.writes == 3 );
    CHECK( buffer.syncs == 3 );
    CHECK( worker.currentGeneration() == maximum );
    CHECK( WorkerVisitor::hasExpectedFullRequest( &worker, maximum ) );
    CHECK( worker.performanceCounters().operationStarts == 0 );
}

TEST_CASE( "Search start trace ignores and preserves hostile stderr formatting",
           "[.search-start-enabled]" )
{
    using WorkerVisitor = LogFilteredDataWorker::access_by<LogFilteredDataWorkerPrivate>;
    REQUIRE( qEnvironmentVariableIsSet( "KLOGG_TEST_TRACE_SEARCH_STARTS" ) );
    TestSearchableLogData sourceLogData;
    LogFilteredDataWorker worker( sourceLogData );
    worker.shutdownAndWait();
    const auto maximum = std::numeric_limits<quint64>::max();
    WorkerVisitor::setActiveOperation( &worker, maximum - 1, maximum - 1 );

    SearchTraceBuffer buffer;
    SearchTraceBuffer tiedBuffer( SearchTraceBuffer::Failure::ThrowSync );
    std::ostream tiedStream( &tiedBuffer );
    tiedStream.exceptions( std::ios::badbit );
    bool flushedBeforeReturn = false;
    bool stateUnchanged = false;
    {
        StderrCapture capture( &buffer );
        StderrFormatGuard formatGuard;
        const std::locale hostileLocale( std::locale::classic(), new GroupedNumbers );
        std::locale::global( hostileLocale );
        std::cerr.imbue( hostileLocale );
        std::cerr.setf( std::ios::hex, std::ios::basefield );
        std::cerr.setf( std::ios::showbase | std::ios::showpos | std::ios::uppercase
                        | std::ios::unitbuf );
        std::cerr.precision( 2 );
        std::cerr.width( 120 );
        std::cerr.fill( '#' );
        std::cerr.exceptions( std::ios::badbit | std::ios::failbit );
        std::cerr.tie( &tiedStream );
        const auto hostileFlags = std::cerr.flags();
        worker.search( RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, 0_lnum, 10_lnum );
        flushedBeforeReturn = buffer.pending.empty() && buffer.committed == MaxTraceRecord;
        stateUnchanged = std::cerr.flags() == hostileFlags && std::cerr.precision() == 2
                         && std::cerr.width() == 120 && std::cerr.fill() == '#'
                         && std::cerr.getloc() == hostileLocale && std::locale() == hostileLocale
                         && std::cerr.exceptions() == ( std::ios::badbit | std::ios::failbit )
                         && std::cerr.tie() == &tiedStream && std::cerr.good();
    }
    CHECK( flushedBeforeReturn );
    CHECK( stateUnchanged );
    CHECK( tiedBuffer.syncs == 0 );
    CHECK( buffer.writes == 3 );
    CHECK( buffer.syncs == 3 );
    CHECK( WorkerVisitor::hasExpectedFullRequest( &worker, maximum ) );
}

TEST_CASE( "Search start trace failures cannot change the primary request or escape",
           "[.search-start-enabled]" )
{
    using WorkerVisitor = LogFilteredDataWorker::access_by<LogFilteredDataWorkerPrivate>;
    REQUIRE( qEnvironmentVariableIsSet( "KLOGG_TEST_TRACE_SEARCH_STARTS" ) );
    const auto failure
        = GENERATE( SearchTraceBuffer::Failure::ShortWrite, SearchTraceBuffer::Failure::RejectWrite,
                    SearchTraceBuffer::Failure::FailedSync, SearchTraceBuffer::Failure::ThrowWrite,
                    SearchTraceBuffer::Failure::ThrowSync );
    TestSearchableLogData sourceLogData;
    LogFilteredDataWorker worker( sourceLogData );
    worker.shutdownAndWait();
    const auto maximum = std::numeric_limits<quint64>::max();
    WorkerVisitor::setActiveOperation( &worker, maximum - 1, maximum - 1 );
    SearchTraceBuffer buffer( failure );
    bool escaped = false;
    {
        StderrCapture capture( &buffer );
        StderrFormatGuard formatGuard;
        std::cerr.exceptions( std::ios::badbit | std::ios::failbit );
        try {
            worker.search( RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, 0_lnum, 10_lnum );
        } catch ( ... ) {
            escaped = true;
        }
    }
    CHECK_FALSE( escaped );
    CHECK( worker.currentGeneration() == maximum );
    CHECK( WorkerVisitor::hasExpectedFullRequest( &worker, maximum ) );
    CHECK( worker.performanceCounters().operationStarts == 0 );
    CHECK( buffer.committed.empty() );
    CHECK( buffer.writes == 3 );
    const bool syncExpected = failure == SearchTraceBuffer::Failure::FailedSync
                              || failure == SearchTraceBuffer::Failure::ThrowSync;
    CHECK( buffer.syncs == ( syncExpected ? 3 : 0 ) );
}

TEST_CASE( "Search start trace tolerates a missing stderr buffer", "[.search-start-enabled]" )
{
    using WorkerVisitor = LogFilteredDataWorker::access_by<LogFilteredDataWorkerPrivate>;
    REQUIRE( qEnvironmentVariableIsSet( "KLOGG_TEST_TRACE_SEARCH_STARTS" ) );
    TestSearchableLogData sourceLogData;
    LogFilteredDataWorker worker( sourceLogData );
    worker.shutdownAndWait();
    bool escaped = false;
    {
        StderrCapture capture( nullptr );
        try {
            worker.search( RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, 0_lnum, 10_lnum );
        } catch ( ... ) {
            escaped = true;
        }
    }
    CHECK_FALSE( escaped );
    CHECK( WorkerVisitor::hasExpectedFullRequest( &worker, 1 ) );
}

TEST_CASE( "Search start trace is silent when the opt-in is absent", "[.search-start-absent]" )
{
    using WorkerVisitor = LogFilteredDataWorker::access_by<LogFilteredDataWorkerPrivate>;
    REQUIRE_FALSE( qEnvironmentVariableIsSet( "KLOGG_TEST_TRACE_SEARCH_STARTS" ) );
    TestSearchableLogData sourceLogData;
    LogFilteredDataWorker worker( sourceLogData );
    worker.shutdownAndWait();
    SearchTraceBuffer buffer;
    {
        StderrCapture capture( &buffer );
        worker.search( RegularExpressionPattern{ QStringLiteral( "ERROR" ) }, 0_lnum, 10_lnum );
    }
    CHECK( buffer.writes == 0 );
    CHECK( buffer.syncs == 0 );
    CHECK( buffer.committed.empty() );
    CHECK( buffer.pending.empty() );
    CHECK( WorkerVisitor::hasExpectedFullRequest( &worker, 1 ) );
}

TEST_CASE(
    "LogFilteredDataWorker clears deferred live update when immediate live dispatch supersedes it" )
{
    using WorkerVisitor = LogFilteredDataWorker::access_by<LogFilteredDataWorkerPrivate>;

    TestSearchableLogData sourceLogData;
    LogFilteredDataWorker worker( sourceLogData );

    // The constructor starts a dispatch thread whose dispatchLoop() consumes
    // deferredLiveRequest_ under requestMutex_.  Inspecting that same private
    // state from this test thread (hasDeferredLiveRequest also takes
    // requestMutex_) is reported by TSan as a double-lock/deadlock risk.
    // Stop the dispatch thread before the enqueue/inspect sequence so this
    // thread is the only one touching requestMutex_; enqueueRequest and
    // enqueueOrDeferLiveRequest manipulate the deferred/pending optionals
    // under the lock regardless of whether a consumer is running, so the
    // assertions below remain meaningful and become deterministic.  The
    // destructor performs the final shutdownAndWait() teardown.
    worker.shutdownAndWait();

    WorkerVisitor::enqueueOrDeferLiveRequest( &worker, 1, 10_lnum );
    REQUIRE( WorkerVisitor::hasDeferredLiveRequest( &worker ) );

    WorkerVisitor::enqueueImmediateLiveRequest( &worker, 2, 100_lnum );

    CHECK_FALSE( WorkerVisitor::hasDeferredLiveRequest( &worker ) );
}

TEST_CASE( "LogFilteredDataWorker deterministically coalesces deferred live endpoints" )
{
    using WorkerVisitor = LogFilteredDataWorker::access_by<LogFilteredDataWorkerPrivate>;

    TestSearchableLogData sourceLogData;
    LogFilteredDataWorker worker( sourceLogData );
    worker.shutdownAndWait();

    for ( quint64 operationId = 1; operationId <= 4; ++operationId ) {
        const auto endLine = LineNumber( 10000 + operationId * 10000 );
        WorkerVisitor::enqueueOrDeferLiveRequest( &worker, operationId, endLine );
    }

    REQUIRE( WorkerVisitor::hasDeferredLiveRequest( &worker ) );
    CHECK( WorkerVisitor::deferredLiveEndLine( &worker ) == 50000_lnum );
    CHECK( WorkerVisitor::deferredLiveOperationId( &worker ) == 4 );
    CHECK( worker.performanceCounters().coalescedLiveUpdates == 3 );
}

TEST_CASE( "LogFilteredDataWorker delivers typed terminal progress on its owner thread" )
{
    using WorkerVisitor = LogFilteredDataWorker::access_by<LogFilteredDataWorkerPrivate>;

    TestSearchableLogData sourceLogData;
    LogFilteredDataWorker worker( sourceLogData );
    worker.shutdownAndWait();
    WorkerVisitor::setActiveOperation( &worker, 2, 9 );

    QSignalSpy progress{ &worker, &LogFilteredDataWorker::searchProgressed };
    REQUIRE( progress.isValid() );

    WorkerVisitor::queueTerminalProgress( &worker, 2, 9 );
    REQUIRE( progress.isEmpty() );
    QCoreApplication::sendPostedEvents( &worker, QEvent::MetaCall );
    REQUIRE( progress.size() == 1 );
    const auto arguments = progress.takeFirst();
    CHECK( arguments.at( 0 ).value<LinesCount>() == 2_lcount );
    CHECK( arguments.at( 1 ).toInt() == 100 );
    CHECK( arguments.at( 2 ).value<LineNumber>() == 1_lnum );
    CHECK( arguments.at( 3 ).toULongLong() == 2 );

    WorkerVisitor::queueTerminalProgress( &worker, 2, 8 );
    QCoreApplication::sendPostedEvents( &worker, QEvent::MetaCall );
    CHECK( progress.isEmpty() );
}
