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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>

#include <logger.h>

#include "configuration.h"
#include "loadingstatus.h"
#include "log.h"
#include "logdata.h"
#include "logfiltereddata.h"
#include "persistentinfo.h"
#include "regularexpressionpattern.h"
#include "streaminglogdata.h"

const bool PersistentInfo::ForcePortable = true;

namespace {

constexpr qint64 LoadTimeoutMs = 30 * 60 * 1000;
constexpr qint64 SearchTimeoutMs = 30 * 60 * 1000;
constexpr qsizetype WriteChunkBytes = 1 << 20;
constexpr const char* BenchmarkVersion = "v1";

struct SizeSpec {
    QString id;
    QString label;
    quint64 requestedBytes = 0;
};

struct ProfileSpec {
    QString id;
    QString label;
    QString description;
    QString pattern;
};

struct BenchmarkOptions {
    QString engine;
    QString scanMode;   // "auto", "per-line", "block"
    QString searchMode; // "full", "incremental", "streaming", "all"
    QString label;
    QString tmpfsDir;
    QString outputPath;
    QVector<SizeSpec> sizes;
    QVector<ProfileSpec> profiles;
    int iterations = 5;
    int warmup = 1;
    quint32 seed = 20260301u;
    bool keepFiles = false;
};

struct FilePrepResult {
    QString path;
    bool reusedInput = false;
    quint64 actualBytes = 0;
    qint64 generationMs = 0;
};

struct IndexedLogData {
    std::unique_ptr<LogData> logData;
    qint64 indexMs = 0;
    quint64 indexedBytes = 0;
};

struct Stats {
    double min = 0.0;
    double median = 0.0;
    double mean = 0.0;
    double max = 0.0;
    double stddev = 0.0;
};

struct CaseResult {
    QString sizeId;
    QString sizeLabel;
    QString searchMode = "full"; // "full" or "incremental"
    quint64 requestedBytes = 0;
    quint64 actualBytes = 0;
    quint64 searchedLineCount = 0;
    QString profileId;
    QString profileLabel;
    QString profileDescription;
    QString pattern;
    QString status = "ok";
    QString skipReason;
    bool reusedInput = false;
    qint64 generationMs = 0;
    qint64 indexMs = 0;
    quint64 matchCount = 0;
    double hitRate = 0.0;
    QVector<double> searchMsIterations;
    QVector<double> throughputMiBsIterations;
    Stats searchMs;
    Stats throughputMiBs;
};

class ConfigGuard {
  public:
    ConfigGuard()
        : config_( Configuration::getSynced() )
        , regexpEngine_( config_.regexpEngine() )
        , nativeFileWatchEnabled_( config_.nativeFileWatchEnabled() )
        , pollingEnabled_( config_.pollingEnabled() )
        , useSearchResultsCache_( config_.useSearchResultsCache() )
        , keepFileClosed_( config_.keepFileClosed() )
    {
        config_.setNativeFileWatchEnabled( false );
        config_.setPollingEnabled( false );
        config_.setUseSearchResultsCache( false );
        config_.setKeepFileClosed( false );
    }

    ~ConfigGuard()
    {
        config_.setRegexpEnging( regexpEngine_ );
        config_.setNativeFileWatchEnabled( nativeFileWatchEnabled_ );
        config_.setPollingEnabled( pollingEnabled_ );
        config_.setUseSearchResultsCache( useSearchResultsCache_ );
        config_.setKeepFileClosed( keepFileClosed_ );
    }

    void setEngine( const QString& engine )
    {
        if ( engine == QLatin1String( "qt" ) ) {
            config_.setRegexpEnging( RegexpEngine::QRegularExpression );
        }
        else if ( engine == QLatin1String( "vectorscan" ) ) {
            config_.setRegexpEnging( RegexpEngine::Vectorscan );
        }
        else {
            throw std::runtime_error( QString( "Unknown engine: %1" ).arg( engine ).toStdString() );
        }
    }

    void setScanMode( const QString& scanMode )
    {
        if ( scanMode == QLatin1String( "per-line" ) ) {
            config_.setUseBlockScan( false );
        }
        else if ( scanMode == QLatin1String( "block" ) || scanMode == QLatin1String( "auto" ) ) {
            config_.setUseBlockScan( true );
        }
        else {
            throw std::runtime_error(
                QString( "Unknown scan-mode: %1" ).arg( scanMode ).toStdString() );
        }
    }

  private:
    Configuration& config_;
    RegexpEngine regexpEngine_;
    bool nativeFileWatchEnabled_;
    bool pollingEnabled_;
    bool useSearchResultsCache_;
    bool keepFileClosed_;
};

QString defaultTmpfsDir()
{
#if defined( Q_OS_LINUX )
    return QLatin1String( "/dev/shm/klogg-bench" );
#elif defined( Q_OS_MAC )
    return QLatin1String( "/Volumes/RAMDisk/klogg-bench" );
#elif defined( Q_OS_WIN )
    return QDir( QDir::tempPath() ).filePath( QLatin1String( "klogg-bench" ) );
#else
    return QDir( QDir::tempPath() ).filePath( QLatin1String( "klogg-bench" ) );
#endif
}

QVector<SizeSpec> availableSizes()
{
    return {
        { QLatin1String( "50MB" ), QLatin1String( "50MB" ), 50ull * 1024ull * 1024ull },
        { QLatin1String( "500MB" ), QLatin1String( "500MB" ), 500ull * 1024ull * 1024ull },
        { QLatin1String( "5GB" ), QLatin1String( "5GB" ), 5ull * 1024ull * 1024ull * 1024ull },
    };
}

QVector<ProfileSpec> availableProfiles()
{
    return {
        { QLatin1String( "simple" ),
          QLatin1String( "Simple" ),
          QLatin1String( "Single-token severity match" ),
          QLatin1String( "ERROR" ) },
        { QLatin1String( "normal" ),
          QLatin1String( "Normal" ),
          QLatin1String(
              "Structured warning or error match with field alternation and bounded numeric suffixes" ),
          QLatin1String(
              "level=(ERROR|WARN).*component=(auth|scheduler|replication|storage|parser|crawler).*msg=\\\"(timeout|failed|exception).*\\\".*path=/api/v1/(task|job)/[0-9]{1,4}.*shard=[0-9]{1,2} retry=false" ) },
        { QLatin1String( "complex" ),
          QLatin1String( "Complex" ),
          QLatin1String(
              "Structured log-line match with nested alternation, optional groups, and bounded repetitions" ),
          QLatin1String(
              "level=(INFO|WARN) component=(auth|scheduler|replication|storage|parser|crawler).*req=[A-F0-9]{16}( session=([A-F0-9]{8}|[A-F0-9]{16}))?.*msg=\\\"(peer (reset|disconnect(ed)?) during remote link handover|(timeout|failed|exception) while processing customer ledger batch)\\\".*path=/api/v1/(node|task)/[0-9]{1,4}.*tenant=prod region=us-east-1 shard=[0-9]{1,2} (window=batch-e2e|retry=false)" ) },
    };
}

template <typename T>
QVector<T> selectByIds( const QVector<T>& all, const QString& csv, const char* kind )
{
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
    const auto wantedIds = csv.split( QLatin1Char( ',' ), Qt::SkipEmptyParts );
#else
    const auto wantedIds = csv.split( QLatin1Char( ',' ), QString::SkipEmptyParts );
#endif
    QVector<T> selected;
    selected.reserve( wantedIds.size() );

    for ( QString wantedId : wantedIds ) {
        wantedId = wantedId.trimmed();
        const auto it = std::find_if( all.cbegin(), all.cend(), [ &wantedId ]( const auto& item ) {
            return item.id.compare( wantedId, Qt::CaseInsensitive ) == 0;
        } );

        if ( it == all.cend() ) {
            throw std::runtime_error(
                QString( "Unknown %1: %2" ).arg( QLatin1String( kind ), wantedId ).toStdString() );
        }

        selected.push_back( *it );
    }

    return selected;
}

BenchmarkOptions parseOptions( QCoreApplication& app )
{
    QCommandLineParser parser;
    parser.setApplicationDescription( QLatin1String(
        "Benchmark klogg end-to-end regex search performance across file sizes and pattern profiles." ) );
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption engineOption(
        QStringList{ QLatin1String( "engine" ) },
        QLatin1String( "Regex engine to exercise: qt or vectorscan." ),
        QLatin1String( "engine" ),
        QLatin1String( "qt" ) );
    const QCommandLineOption labelOption(
        QStringList{ QLatin1String( "label" ) },
        QLatin1String( "Display label written into JSON output." ),
        QLatin1String( "label" ),
        QLatin1String() );
    const QCommandLineOption tmpfsDirOption(
        QStringList{ QLatin1String( "tmpfs-dir" ) },
        QLatin1String( "Directory on a memory-backed filesystem used for generated log files." ),
        QLatin1String( "path" ),
        defaultTmpfsDir() );
    const QCommandLineOption sizesOption(
        QStringList{ QLatin1String( "sizes" ) },
        QLatin1String( "Comma-separated size buckets to run." ),
        QLatin1String( "sizes" ),
        QLatin1String( "50MB,500MB,5GB" ) );
    const QCommandLineOption profilesOption(
        QStringList{ QLatin1String( "profiles" ) },
        QLatin1String( "Comma-separated profile ids to run." ),
        QLatin1String( "profiles" ),
        QLatin1String( "simple,normal,complex" ) );
    const QCommandLineOption iterationsOption(
        QStringList{ QLatin1String( "iterations" ) },
        QLatin1String( "Measured iterations per case." ),
        QLatin1String( "count" ),
        QLatin1String( "5" ) );
    const QCommandLineOption warmupOption(
        QStringList{ QLatin1String( "warmup" ) },
        QLatin1String( "Warmup iterations per case." ),
        QLatin1String( "count" ),
        QLatin1String( "1" ) );
    const QCommandLineOption outputOption(
        QStringList{ QLatin1String( "output" ) },
        QLatin1String( "Optional JSON output path." ),
        QLatin1String( "path" ),
        QLatin1String() );
    const QCommandLineOption seedOption(
        QStringList{ QLatin1String( "seed" ) },
        QLatin1String( "Deterministic seed for generated corpora." ),
        QLatin1String( "seed" ),
        QLatin1String( "20260301" ) );
    const QCommandLineOption scanModeOption(
        QStringList{ QLatin1String( "scan-mode" ) },
        QLatin1String( "Scan mode: auto (default), per-line, or block." ),
        QLatin1String( "mode" ),
        QLatin1String( "auto" ) );
    const QCommandLineOption searchModeOption(
        QStringList{ QLatin1String( "search-mode" ) },
        QLatin1String( "Search mode: full (default), incremental, streaming, or all." ),
        QLatin1String( "mode" ),
        QLatin1String( "full" ) );
    const QCommandLineOption keepFilesOption(
        QStringList{ QLatin1String( "keep-files" ) },
        QLatin1String( "Keep generated log files after the benchmark finishes." ) );

    parser.addOption( engineOption );
    parser.addOption( labelOption );
    parser.addOption( scanModeOption );
    parser.addOption( searchModeOption );
    parser.addOption( tmpfsDirOption );
    parser.addOption( sizesOption );
    parser.addOption( profilesOption );
    parser.addOption( iterationsOption );
    parser.addOption( warmupOption );
    parser.addOption( outputOption );
    parser.addOption( seedOption );
    parser.addOption( keepFilesOption );

    parser.process( app );

    BenchmarkOptions options;
    options.engine = parser.value( engineOption ).trimmed().toLower();
    options.scanMode = parser.value( scanModeOption ).trimmed().toLower();
    options.searchMode = parser.value( searchModeOption ).trimmed().toLower();
    options.label = parser.value( labelOption ).trimmed();
    if ( options.label.isEmpty() ) {
        options.label = options.engine;
        if ( options.scanMode != QLatin1String( "auto" ) ) {
            options.label += QLatin1String( "-" ) + options.scanMode;
        }
    }
    options.tmpfsDir = QDir::cleanPath( parser.value( tmpfsDirOption ).trimmed() );
    options.outputPath = parser.value( outputOption ).trimmed();
    options.iterations = parser.value( iterationsOption ).toInt();
    options.warmup = parser.value( warmupOption ).toInt();
    options.seed = parser.value( seedOption ).toUInt();
    options.keepFiles = parser.isSet( keepFilesOption );
    options.sizes = selectByIds( availableSizes(), parser.value( sizesOption ), "size bucket" );
    options.profiles = selectByIds( availableProfiles(), parser.value( profilesOption ), "profile" );

    if ( options.iterations <= 0 ) {
        throw std::runtime_error( "iterations must be greater than zero" );
    }
    if ( options.warmup < 0 ) {
        throw std::runtime_error( "warmup must be zero or greater" );
    }

    return options;
}

QString makeLogFileName( const SizeSpec& size, quint32 seed )
{
    return QString( "regex-bench-%1-%2-seed-%3.log" )
        .arg( QLatin1String( BenchmarkVersion ) )
        .arg( size.id )
        .arg( seed );
}

Stats computeStats( QVector<double> values )
{
    Stats stats;
    if ( values.isEmpty() ) {
        return stats;
    }

    std::sort( values.begin(), values.end() );
    stats.min = values.front();
    stats.max = values.back();
    stats.mean = std::accumulate( values.cbegin(), values.cend(), 0.0 )
               / static_cast<double>( values.size() );

    const auto mid = values.size() / 2;
    if ( values.size() % 2 == 0 ) {
        stats.median = ( values[ mid - 1 ] + values[ mid ] ) / 2.0;
    }
    else {
        stats.median = values[ mid ];
    }

    double variance = 0.0;
    for ( const double value : values ) {
        const auto delta = value - stats.mean;
        variance += delta * delta;
    }
    variance /= static_cast<double>( values.size() );
    stats.stddev = std::sqrt( variance );

    return stats;
}

QJsonObject statsToJson( const Stats& stats )
{
    return QJsonObject{
        { QLatin1String( "min" ), stats.min },
        { QLatin1String( "median" ), stats.median },
        { QLatin1String( "mean" ), stats.mean },
        { QLatin1String( "max" ), stats.max },
        { QLatin1String( "stddev" ), stats.stddev },
    };
}

QJsonArray doublesToJson( const QVector<double>& values )
{
    QJsonArray json;
    for ( const auto value : values ) {
        json.append( value );
    }
    return json;
}

QString formatDuration( double milliseconds )
{
    return QString::number( milliseconds, 'f', 2 );
}

QString formatThroughput( double mibPerSecond )
{
    return QString::number( mibPerSecond, 'f', 2 );
}

QString formatHitRate( double hitRate )
{
    return QString::number( hitRate * 100.0, 'f', 2 ) + QLatin1Char( '%' );
}

bool startAndWaitForLoad( LogData& logData, LoadingStatus& status, int timeoutMs,
                          const std::function<void()>& startFn )
{
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot( true );

    bool finished = false;
    QObject::connect( &timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit );
    QObject::connect( &logData, &LogData::loadingFinished, &loop,
                      [ & ]( LoadingStatus loadStatus ) {
                          status = loadStatus;
                          finished = true;
                          loop.quit();
                      } );

    timeoutTimer.start( timeoutMs );
    startFn();
    if ( finished ) {
        return true;
    }
    loop.exec();
    return finished;
}

bool startAndWaitForSearch( LogFilteredData& filteredData, int timeoutMs,
                            const std::function<void()>& startFn )
{
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot( true );

    bool finished = false;
    QObject::connect( &timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit );
    QObject::connect( &filteredData, &LogFilteredData::searchProgressed, &loop,
                      [ & ]( LinesCount, int progress, LineNumber ) {
                          if ( progress >= 100 ) {
                              finished = true;
                              loop.quit();
                          }
                      } );

    timeoutTimer.start( timeoutMs );
    startFn();
    if ( finished ) {
        return true;
    }
    loop.exec();
    return finished;
}

int formatLogLine( quint64 lineNumber, quint32 seed, char* buffer, std::size_t bufferSize )
{
    static constexpr std::array<const char*, 6> Components = {
        "auth", "scheduler", "replication", "storage", "parser", "crawler"
    };
    static constexpr std::array<const char*, 4> WarmStates = {
        "steady-state request processing",
        "background checkpoint completed",
        "normal replication heartbeat",
        "reader window advanced cleanly"
    };

    const auto component = Components[ static_cast<std::size_t>( lineNumber % Components.size() ) ];
    const auto second = static_cast<int>( lineNumber % 60 );
    const auto millis = static_cast<int>( lineNumber % 1000 );
    const auto threadId = static_cast<int>( lineNumber % 32 );
    const auto worker = static_cast<unsigned>( ( seed + static_cast<quint32>( lineNumber ) ) % 128u );
    const auto shard = static_cast<unsigned>( lineNumber % 64u );
    const auto requestId = static_cast<unsigned long long>(
        0x9e3779b97f4a7c15ull ^ ( static_cast<unsigned long long>( seed ) << 16 )
        ^ lineNumber * 0x100000001b3ull );

    if ( lineNumber % 100 == 0 ) {
        const auto session = ( lineNumber % 200 == 0 ) ? "DEADBEEFCAFEBABE" : "AB12CD34";
        const auto event = ( lineNumber % 300 == 0 ) ? "disconnect"
                                                     : ( lineNumber % 200 == 0 ) ? "reset"
                                                                                 : "disconnected";
        return std::snprintf(
            buffer,
            bufferSize,
            "2026-03-01T12:34:%02d.%03dZ host=bench01 level=INFO component=%s thread=%02d worker=%03u "
            "req=%016llX session=%s msg=\"peer %s during remote link handover\" path=/api/v1/node/%u "
            "tenant=prod region=us-east-1 shard=%u window=batch-e2e\n",
            second,
            millis,
            component,
            threadId,
            worker,
            requestId,
            session,
            event,
            static_cast<unsigned>( lineNumber % 4096u ),
            shard );
    }

    if ( lineNumber % 33 == 0 ) {
        const auto event = ( lineNumber % 99 == 0 ) ? "timeout"
                                                    : ( lineNumber % 66 == 0 ) ? "failed"
                                                                               : "exception";
        return std::snprintf(
            buffer,
            bufferSize,
            "2026-03-01T12:34:%02d.%03dZ host=bench01 level=WARN component=%s thread=%02d worker=%03u "
            "req=%016llX msg=\"%s while processing customer ledger batch\" path=/api/v1/task/%u "
            "tenant=prod region=us-east-1 shard=%u retry=false\n",
            second,
            millis,
            component,
            threadId,
            worker,
            requestId,
            event,
            static_cast<unsigned>( lineNumber % 2048u ),
            shard );
    }

    if ( lineNumber % 10 == 0 ) {
        return std::snprintf(
            buffer,
            bufferSize,
            "2026-03-01T12:34:%02d.%03dZ host=bench01 level=ERROR component=%s thread=%02d worker=%03u "
            "req=%016llX msg=\"simple marker only for benchmark coverage\" path=/api/v1/job/%u "
            "tenant=prod region=us-east-1 shard=%u action=read\n",
            second,
            millis,
            component,
            threadId,
            worker,
            requestId,
            static_cast<unsigned>( lineNumber % 1024u ),
            shard );
    }

    return std::snprintf(
        buffer,
        bufferSize,
        "2026-03-01T12:34:%02d.%03dZ host=bench01 level=INFO component=%s thread=%02d worker=%03u "
        "req=%016llX msg=\"%s\" path=/api/v1/steady/%u tenant=prod region=us-east-1 shard=%u "
        "status=ok bytes=%u\n",
        second,
        millis,
        component,
        threadId,
        worker,
        requestId,
        WarmStates[ static_cast<std::size_t>( lineNumber % WarmStates.size() ) ],
        static_cast<unsigned>( lineNumber % 8192u ),
        shard,
        static_cast<unsigned>( 512u + ( lineNumber % 4096u ) ) );
}

FilePrepResult ensureLogFile( const BenchmarkOptions& options, const SizeSpec& size, QTextStream& err )
{
    QDir tmpfsDir{ options.tmpfsDir };
    if ( !tmpfsDir.exists() && !QDir{}.mkpath( tmpfsDir.path() ) ) {
        throw std::runtime_error(
            QString( "Failed to create tmpfs directory: %1" ).arg( options.tmpfsDir ).toStdString() );
    }

    const auto filePath = tmpfsDir.filePath( makeLogFileName( size, options.seed ) );
    QFileInfo existingFileInfo{ filePath };
    if ( existingFileInfo.exists() && existingFileInfo.size() > 0 ) {
        return FilePrepResult{ filePath, true, static_cast<quint64>( existingFileInfo.size() ), 0 };
    }

    const auto storageInfo = QStorageInfo( tmpfsDir.absolutePath() );
    const auto bytesAvailable
        = static_cast<quint64>( qMax<qint64>( 0ll, storageInfo.bytesAvailable() ) );
    const auto headroomBytes = 64ull * 1024ull * 1024ull;
    if ( !storageInfo.isValid() || !storageInfo.isReady() ) {
        throw std::runtime_error(
            QString( "tmpfs path is not ready: %1" ).arg( tmpfsDir.absolutePath() ).toStdString() );
    }
    if ( bytesAvailable < size.requestedBytes + headroomBytes ) {
        throw std::runtime_error(
            QString( "Insufficient tmpfs capacity at %1: need at least %2 bytes, have %3 bytes" )
                .arg( tmpfsDir.absolutePath() )
                .arg( size.requestedBytes + headroomBytes )
                .arg( bytesAvailable )
                .toStdString() );
    }

    err << "Generating " << size.label << " corpus at " << filePath << QLatin1Char( '\n' );

    QFile file{ filePath };
    if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        throw std::runtime_error(
            QString( "Failed to open benchmark corpus for writing: %1" ).arg( filePath ).toStdString() );
    }

    QElapsedTimer timer;
    timer.start();

    QByteArray batch;
    batch.reserve( WriteChunkBytes );
    std::array<char, 512> lineBuffer{};

    quint64 bytesWritten = 0;
    quint64 lineNumber = 0;
    while ( bytesWritten < size.requestedBytes ) {
        const auto length = formatLogLine( lineNumber++, options.seed, lineBuffer.data(), lineBuffer.size() );
        if ( length <= 0 || static_cast<std::size_t>( length ) >= lineBuffer.size() ) {
            throw std::runtime_error( "Failed to format benchmark log line" );
        }

        batch.append( lineBuffer.data(), length );
        if ( batch.size() >= WriteChunkBytes ) {
            const auto written = file.write( batch );
            if ( written != batch.size() ) {
                throw std::runtime_error(
                    QString( "Failed to write benchmark corpus to %1" ).arg( filePath ).toStdString() );
            }
            bytesWritten += static_cast<quint64>( written );
            batch.clear();
        }
    }

    if ( !batch.isEmpty() ) {
        const auto written = file.write( batch );
        if ( written != batch.size() ) {
            throw std::runtime_error(
                QString( "Failed to finalize benchmark corpus at %1" ).arg( filePath ).toStdString() );
        }
        bytesWritten += static_cast<quint64>( written );
    }

    file.flush();
    file.close();

    QFileInfo generatedFileInfo{ filePath };
    return FilePrepResult{
        filePath,
        false,
        static_cast<quint64>( generatedFileInfo.size() ),
        timer.elapsed(),
    };
}

IndexedLogData indexLogFile( const QString& filePath )
{
    auto logData = std::make_unique<LogData>();
    LoadingStatus status = LoadingStatus::Interrupted;

    QElapsedTimer timer;
    timer.start();
    if ( !startAndWaitForLoad( *logData, status, static_cast<int>( LoadTimeoutMs ),
                               [ &logData, &filePath ] { logData->attachFile( filePath ); } ) ) {
        throw std::runtime_error(
            QString( "Timed out while indexing %1" ).arg( filePath ).toStdString() );
    }
    if ( status != LoadingStatus::Successful ) {
        throw std::runtime_error(
            QString( "Indexing failed for %1" ).arg( filePath ).toStdString() );
    }

    return IndexedLogData{
        std::move( logData ),
        timer.elapsed(),
        static_cast<quint64>( QFileInfo{ filePath }.size() ),
    };
}

CaseResult benchmarkCase( const FilePrepResult& filePrepResult, const IndexedLogData& indexedLog,
                          const BenchmarkOptions& options, const SizeSpec& size,
                          const ProfileSpec& profile, QTextStream& err )
{
    CaseResult result;
    result.sizeId = size.id;
    result.sizeLabel = size.label;
    result.requestedBytes = size.requestedBytes;
    result.actualBytes = filePrepResult.actualBytes;
    result.searchedLineCount = indexedLog.logData->getNbLine().get();
    result.profileId = profile.id;
    result.profileLabel = profile.label;
    result.profileDescription = profile.description;
    result.pattern = profile.pattern;
    result.reusedInput = filePrepResult.reusedInput;
    result.generationMs = filePrepResult.generationMs;
    result.indexMs = indexedLog.indexMs;

    err << "Running " << options.label << " size=" << size.label << " profile=" << profile.id
        << " warmup=" << options.warmup << " iterations=" << options.iterations
        << QLatin1Char( '\n' );

    for ( int warmupIndex = 0; warmupIndex < options.warmup; ++warmupIndex ) {
        auto filteredData = indexedLog.logData->getNewFilteredData();
        const RegularExpressionPattern pattern{ profile.pattern };
        if ( !startAndWaitForSearch( *filteredData, static_cast<int>( SearchTimeoutMs ),
                                     [ &filteredData, &pattern ] {
                                         filteredData->runSearch( pattern );
                                     } ) ) {
            throw std::runtime_error(
                QString( "Warmup timed out for size=%1 profile=%2" ).arg( size.id, profile.id ).toStdString() );
        }
    }

    quint64 expectedMatchCount = 0;
    bool hasExpectedMatchCount = false;
    const auto mib = static_cast<double>( filePrepResult.actualBytes ) / ( 1024.0 * 1024.0 );

    for ( int iteration = 0; iteration < options.iterations; ++iteration ) {
        auto filteredData = indexedLog.logData->getNewFilteredData();
        const RegularExpressionPattern pattern{ profile.pattern };

        QElapsedTimer timer;
        timer.start();
        if ( !startAndWaitForSearch( *filteredData, static_cast<int>( SearchTimeoutMs ),
                                     [ &filteredData, &pattern ] {
                                         filteredData->runSearch( pattern );
                                     } ) ) {
            throw std::runtime_error(
                QString( "Search timed out for size=%1 profile=%2 iteration=%3" )
                    .arg( size.id, profile.id )
                    .arg( iteration )
                    .toStdString() );
        }

        const auto searchMs = static_cast<double>( timer.nsecsElapsed() ) / 1'000'000.0;
        const auto matchCount = filteredData->getNbMatches().get();
        if ( !hasExpectedMatchCount ) {
            expectedMatchCount = matchCount;
            hasExpectedMatchCount = true;
        }
        else if ( expectedMatchCount != matchCount ) {
            throw std::runtime_error(
                QString( "Match count changed between iterations for size=%1 profile=%2" )
                    .arg( size.id, profile.id )
                    .toStdString() );
        }

        result.searchMsIterations.push_back( searchMs );
        result.throughputMiBsIterations.push_back( mib / ( searchMs / 1000.0 ) );
        result.matchCount = matchCount;
    }

    if ( result.searchedLineCount > 0 ) {
        result.hitRate
            = static_cast<double>( result.matchCount ) / static_cast<double>( result.searchedLineCount );
    }

    result.searchMs = computeStats( result.searchMsIterations );
    result.throughputMiBs = computeStats( result.throughputMiBsIterations );
    return result;
}

// Incremental search benchmark: full search on first 90% of lines, then
// updateSearch on the remaining 10%.  Measures only the incremental portion.
CaseResult benchmarkCaseIncremental( const FilePrepResult& filePrepResult,
                                      const IndexedLogData& indexedLog,
                                      const BenchmarkOptions& options, const SizeSpec& size,
                                      const ProfileSpec& profile, QTextStream& err )
{
    CaseResult result;
    result.sizeId = size.id;
    result.sizeLabel = size.label;
    result.requestedBytes = size.requestedBytes;
    result.actualBytes = filePrepResult.actualBytes;
    result.profileId = profile.id;
    result.profileLabel = profile.label;
    result.profileDescription = profile.description;
    result.pattern = profile.pattern;
    result.reusedInput = filePrepResult.reusedInput;
    result.generationMs = filePrepResult.generationMs;
    result.indexMs = indexedLog.indexMs;

    const auto totalLines = indexedLog.logData->getNbLine();
    const auto splitLine = LineNumber( static_cast<LineNumber::UnderlyingType>( totalLines.get() * 9 / 10 ) );
    const auto endLine = LineNumber( totalLines.get() );
    result.searchedLineCount = static_cast<quint64>( totalLines.get() - splitLine.get() );

    err << "Running incremental " << options.label << " size=" << size.label
        << " profile=" << profile.id << " split=" << splitLine.get() << "/" << totalLines.get()
        << " warmup=" << options.warmup << " iterations=" << options.iterations
        << QLatin1Char( '\n' );

    const auto incrementalMib
        = static_cast<double>( filePrepResult.actualBytes ) / 10.0 / ( 1024.0 * 1024.0 );

    for ( int iteration = 0; iteration < options.iterations; ++iteration ) {
        auto filteredData = indexedLog.logData->getNewFilteredData();
        const RegularExpressionPattern pattern{ profile.pattern };

        // Phase 1: full search on first 90% (establishes watermark)
        if ( !startAndWaitForSearch( *filteredData, static_cast<int>( SearchTimeoutMs ),
                                     [ &filteredData, &pattern, &splitLine ] {
                                         filteredData->runSearch( pattern, 0_lnum, splitLine );
                                     } ) ) {
            throw std::runtime_error( "Incremental: initial search timed out" );
        }
        const auto matchesBefore = filteredData->getNbMatches().get();

        // Phase 2: measure only the incremental update (remaining 10%)
        QElapsedTimer timer;
        timer.start();
        if ( !startAndWaitForSearch( *filteredData, static_cast<int>( SearchTimeoutMs ),
                                     [ &filteredData, &endLine ] {
                                         filteredData->updateSearch( 0_lnum, endLine );
                                     } ) ) {
            throw std::runtime_error( "Incremental: update search timed out" );
        }

        const auto searchMs = static_cast<double>( timer.nsecsElapsed() ) / 1'000'000.0;
        const auto matchesAfter = filteredData->getNbMatches().get();
        result.matchCount = matchesAfter - matchesBefore;
        result.searchMsIterations.push_back( searchMs );
        result.throughputMiBsIterations.push_back( incrementalMib / ( searchMs / 1000.0 ) );
    }

    if ( result.searchedLineCount > 0 ) {
        result.hitRate = static_cast<double>( result.matchCount )
                       / static_cast<double>( totalLines.get() );
    }

    result.searchMs = computeStats( result.searchMsIterations );
    result.throughputMiBs = computeStats( result.throughputMiBsIterations );
    return result;
}

// Streaming benchmark: simulates ADB logcat live source.
// A writer thread appends data at a fixed rate while search runs concurrently.
// Measures: total search time to cover all lines, search lag (watermark behind data).
CaseResult benchmarkCaseStreaming( const BenchmarkOptions& options, const SizeSpec& size,
                                    const ProfileSpec& profile, QTextStream& err )
{
    CaseResult result;
    result.sizeId = size.id;
    result.sizeLabel = size.label;
    result.searchMode = QLatin1String( "streaming" );
    result.requestedBytes = size.requestedBytes;
    result.profileId = profile.id;
    result.profileLabel = profile.label;
    result.profileDescription = profile.description;
    result.pattern = profile.pattern;

    // Use a smaller target for streaming to keep wall-clock time reasonable
    const auto streamBytes = qMin( size.requestedBytes, static_cast<quint64>( 50 ) * 1024 * 1024 );
    const auto targetLines = static_cast<quint64>( streamBytes / 200 ); // ~200 bytes/line average

    err << "Running streaming " << options.label << " size=" << size.label
        << " profile=" << profile.id << " targetLines=" << targetLines
        << " iterations=" << options.iterations << QLatin1Char( '\n' );

    const auto streamMib = static_cast<double>( streamBytes ) / ( 1024.0 * 1024.0 );

    for ( int iteration = 0; iteration < options.iterations; ++iteration ) {
        auto captureId = QString( "bench-stream-%1" ).arg( iteration );
        auto streamingLogData = std::make_unique<StreamingLogData>( captureId );
        auto filteredData = streamingLogData->getNewFilteredData();

        // Pre-generate all lines to remove generation overhead from measurement
        std::vector<QByteArray> allLines;
        allLines.reserve( targetLines );
        std::array<char, 512> lineBuffer{};
        for ( quint64 i = 0; i < targetLines; ++i ) {
            const auto length
                = formatLogLine( i, options.seed, lineBuffer.data(), lineBuffer.size() );
            allLines.emplace_back( lineBuffer.data(), length );
        }

        // Start search with auto-refresh
        const RegularExpressionPattern pattern{ profile.pattern };
        std::atomic<bool> searchDone{ false };
        std::atomic<int> searchUpdates{ 0 };

        QObject::connect( filteredData.get(), &LogFilteredData::searchProgressed,
                          [ &searchDone, &searchUpdates ]( LinesCount, int progress, LineNumber ) {
                              searchUpdates.fetch_add( 1 );
                              if ( progress >= 100 ) {
                                  searchDone.store( true );
                              }
                          } );

        // Feed all data in large batches (10,000 lines -- matching the search
        // chunk size) with a few updateSearch triggers.  This models the real
        // application where scheduleLoadingFinished() coalesces many small
        // appendUtf8 calls into fewer search triggers.
        //
        // We measure:
        //   totalMs     = wall-clock from first append to search completion
        //   searchCount = number of updateSearch calls (thread create/join cycles)
        constexpr quint64 batchSize = 10000;
        quint64 linesWritten = 0;

        QElapsedTimer timer;
        timer.start();

        for ( quint64 i = 0; i < targetLines; i += batchSize ) {
            QByteArray batch;
            const auto end = qMin( i + batchSize, targetLines );
            for ( quint64 j = i; j < end; ++j ) {
                batch.append( allLines[ j ] );
            }
            streamingLogData->appendUtf8( batch );
            linesWritten = end;

            // Trigger search (one per batch -- models coalesced loadingFinished)
            if ( i == 0 ) {
                filteredData->runSearch( pattern, 0_lnum,
                                         LineNumber( streamingLogData->getNbLine().get() ) );
            }
            else {
                filteredData->updateSearch( 0_lnum,
                                             LineNumber( streamingLogData->getNbLine().get() ) );
            }

            // Process Qt events (signals, search progress)
            QCoreApplication::processEvents( QEventLoop::AllEvents, 1 );
        }

        // Final catch-up: wait for search to cover all ingested lines.
        streamingLogData->finishInput();
        const auto finalEnd = LineNumber( streamingLogData->getNbLine().get() );

        for ( int catchup = 0; catchup < 100; ++catchup ) {
            searchDone.store( false );
            if ( !startAndWaitForSearch( *filteredData, static_cast<int>( SearchTimeoutMs ),
                                         [ &filteredData, &finalEnd ] {
                                             filteredData->updateSearch( 0_lnum, finalEnd );
                                         } ) ) {
                break;
            }
            if ( searchDone.load() ) {
                break;
            }
        }

        const auto totalMs = static_cast<double>( timer.nsecsElapsed() ) / 1'000'000.0;
        result.matchCount = filteredData->getNbMatches().get();
        result.searchedLineCount = static_cast<quint64>( linesWritten );
        result.actualBytes = streamBytes;
        result.searchMsIterations.push_back( totalMs );
        result.throughputMiBsIterations.push_back( streamMib / ( totalMs / 1000.0 ) );

        // Cleanup capture files
        streamingLogData->deleteCaptureFiles();
    }

    if ( result.searchedLineCount > 0 ) {
        result.hitRate = static_cast<double>( result.matchCount )
                       / static_cast<double>( result.searchedLineCount );
    }

    result.searchMs = computeStats( result.searchMsIterations );
    result.throughputMiBs = computeStats( result.throughputMiBsIterations );
    return result;
}

QJsonObject caseResultToJson( const CaseResult& result )
{
    return QJsonObject{
        { QLatin1String( "size_id" ), result.sizeId },
        { QLatin1String( "size_label" ), result.sizeLabel },
        { QLatin1String( "search_mode" ), result.searchMode },
        { QLatin1String( "requested_bytes" ), static_cast<qint64>( result.requestedBytes ) },
        { QLatin1String( "actual_bytes" ), static_cast<qint64>( result.actualBytes ) },
        { QLatin1String( "searched_line_count" ),
          static_cast<qint64>( result.searchedLineCount ) },
        { QLatin1String( "profile_id" ), result.profileId },
        { QLatin1String( "profile_label" ), result.profileLabel },
        { QLatin1String( "profile_description" ), result.profileDescription },
        { QLatin1String( "pattern" ), result.pattern },
        { QLatin1String( "status" ), result.status },
        { QLatin1String( "skip_reason" ), result.skipReason },
        { QLatin1String( "reused_input" ), result.reusedInput },
        { QLatin1String( "generation_ms" ), result.generationMs },
        { QLatin1String( "index_ms" ), result.indexMs },
        { QLatin1String( "match_count" ), static_cast<qint64>( result.matchCount ) },
        { QLatin1String( "hit_rate" ), result.hitRate },
        { QLatin1String( "search_ms_iterations" ), doublesToJson( result.searchMsIterations ) },
        { QLatin1String( "throughput_mib_per_s_iterations" ),
          doublesToJson( result.throughputMiBsIterations ) },
        { QLatin1String( "search_ms" ), statsToJson( result.searchMs ) },
        { QLatin1String( "throughput_mib_per_s" ), statsToJson( result.throughputMiBs ) },
    };
}

QJsonObject buildJsonReport( const BenchmarkOptions& options, const QVector<CaseResult>& results )
{
    QJsonArray casesJson;
    for ( const auto& result : results ) {
        casesJson.append( caseResultToJson( result ) );
    }

    QJsonArray profilesJson;
    for ( const auto& profile : options.profiles ) {
        profilesJson.append( QJsonObject{
            { QLatin1String( "id" ), profile.id },
            { QLatin1String( "label" ), profile.label },
            { QLatin1String( "description" ), profile.description },
            { QLatin1String( "pattern" ), profile.pattern },
        } );
    }

    QJsonArray sizesJson;
    for ( const auto& size : options.sizes ) {
        sizesJson.append( QJsonObject{
            { QLatin1String( "id" ), size.id },
            { QLatin1String( "label" ), size.label },
            { QLatin1String( "requested_bytes" ), static_cast<qint64>( size.requestedBytes ) },
        } );
    }

    return QJsonObject{
        { QLatin1String( "benchmark" ), QLatin1String( "regex_search_benchmark" ) },
        { QLatin1String( "version" ), QLatin1String( BenchmarkVersion ) },
        { QLatin1String( "generated_at_utc" ),
          QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) },
        { QLatin1String( "engine" ), options.engine },
        { QLatin1String( "scan_mode" ), options.scanMode },
        { QLatin1String( "search_mode" ), options.searchMode },
        { QLatin1String( "label" ), options.label },
        { QLatin1String( "tmpfs_dir" ), options.tmpfsDir },
        { QLatin1String( "iterations" ), options.iterations },
        { QLatin1String( "warmup" ), options.warmup },
        { QLatin1String( "seed" ), static_cast<int>( options.seed ) },
        { QLatin1String( "keep_files" ), options.keepFiles },
        { QLatin1String( "host" ),
          QJsonObject{
              { QLatin1String( "product_name" ), QSysInfo::prettyProductName() },
              { QLatin1String( "kernel_type" ), QSysInfo::kernelType() },
              { QLatin1String( "kernel_version" ), QSysInfo::kernelVersion() },
              { QLatin1String( "cpu_architecture" ), QSysInfo::currentCpuArchitecture() },
              { QLatin1String( "qt_version" ), QLatin1String( QT_VERSION_STR ) },
          } },
        { QLatin1String( "sizes" ), sizesJson },
        { QLatin1String( "profiles" ), profilesJson },
        { QLatin1String( "cases" ), casesJson },
    };
}

void writeJsonReport( const QString& outputPath, const QJsonObject& report )
{
    if ( outputPath.isEmpty() ) {
        return;
    }

    const QFileInfo outputInfo{ outputPath };
    QDir{}.mkpath( outputInfo.absolutePath() );

    QSaveFile outputFile{ outputPath };
    if ( !outputFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        throw std::runtime_error(
            QString( "Failed to open JSON output file: %1" ).arg( outputPath ).toStdString() );
    }

    const auto data = QJsonDocument( report ).toJson( QJsonDocument::Indented );
    if ( outputFile.write( data ) != data.size() ) {
        throw std::runtime_error(
            QString( "Failed to write JSON output file: %1" ).arg( outputPath ).toStdString() );
    }
    if ( !outputFile.commit() ) {
        throw std::runtime_error(
            QString( "Failed to commit JSON output file: %1" ).arg( outputPath ).toStdString() );
    }
}

void printSummary( QTextStream& out, const BenchmarkOptions& options, const QVector<CaseResult>& results )
{
    out << "# Regex Search Benchmark: " << options.label << QLatin1Char( '\n' );
    out << QLatin1Char( '\n' );
    out << "## Regex Profiles" << QLatin1Char( '\n' );
    out << QLatin1Char( '\n' );
    out << "| Profile | Pattern |" << QLatin1Char( '\n' );
    out << "| --- | --- |" << QLatin1Char( '\n' );
    for ( const auto& profile : options.profiles ) {
        out << "| " << profile.id << " | `" << profile.pattern << "` |" << QLatin1Char( '\n' );
    }
    out << QLatin1Char( '\n' );
    out << "| Size | Profile | Searched lines | Matches | Hit rate | Median search (ms) | Mean search (ms) | Median throughput (MiB/s) | Status |"
        << QLatin1Char( '\n' );
    out << "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |"
        << QLatin1Char( '\n' );

    for ( const auto& result : results ) {
        if ( result.status != QLatin1String( "ok" ) ) {
            out << "| " << result.sizeLabel << " | " << result.profileId
                << " | - | - | - | - | - | - | " << result.status;
            if ( !result.skipReason.isEmpty() ) {
                out << " (" << result.skipReason << ")";
            }
            out << " |" << QLatin1Char( '\n' );
            continue;
        }

        out << "| " << result.sizeLabel << " | " << result.profileId << " | "
            << result.searchedLineCount << " | " << result.matchCount << " | "
            << formatHitRate( result.hitRate ) << " | " << formatDuration( result.searchMs.median )
            << " | " << formatDuration( result.searchMs.mean ) << " | "
            << formatThroughput( result.throughputMiBs.median ) << " | ok |"
            << QLatin1Char( '\n' );
    }
}

} // namespace

int main( int argc, char* argv[] )
{
    QCoreApplication app( argc, argv );
    QCoreApplication::setApplicationName( QLatin1String( "regex_search_benchmark" ) );
    QCoreApplication::setOrganizationName( QLatin1String( "klogg" ) );

    logging::enableLogging( true, logging::LogLevel::Error );

    QTextStream out( stdout );
    QTextStream err( stderr );

    try {
        const auto options = parseOptions( app );
        ConfigGuard configGuard;
        configGuard.setEngine( options.engine );
        configGuard.setScanMode( options.scanMode );

        QVector<CaseResult> results;
        QStringList touchedFiles;

        for ( const auto& size : options.sizes ) {
            bool skipSize = false;
            FilePrepResult filePrepResult;

            try {
                filePrepResult = ensureLogFile( options, size, err );
                touchedFiles.push_back( filePrepResult.path );
            } catch ( const std::exception& ex ) {
                skipSize = true;
                for ( const auto& profile : options.profiles ) {
                    CaseResult skipped;
                    skipped.sizeId = size.id;
                    skipped.sizeLabel = size.label;
                    skipped.requestedBytes = size.requestedBytes;
                    skipped.profileId = profile.id;
                    skipped.profileLabel = profile.label;
                    skipped.profileDescription = profile.description;
                    skipped.pattern = profile.pattern;
                    skipped.status = QLatin1String( "skipped" );
                    skipped.skipReason = QString::fromUtf8( ex.what() );
                    results.push_back( skipped );
                }
            }

            if ( skipSize ) {
                continue;
            }

            const auto indexedLog = indexLogFile( filePrepResult.path );

            const auto isAll = options.searchMode == QLatin1String( "all" );
            const bool runFull = isAll || options.searchMode == QLatin1String( "full" );
            const bool runIncremental = isAll || options.searchMode == QLatin1String( "incremental" );
            const bool runStreaming = isAll || options.searchMode == QLatin1String( "streaming" );

            for ( const auto& profile : options.profiles ) {
                if ( runFull ) {
                    results.push_back(
                        benchmarkCase( filePrepResult, indexedLog, options, size, profile, err ) );
                }
                if ( runIncremental ) {
                    auto incrResult = benchmarkCaseIncremental( filePrepResult, indexedLog, options,
                                                                size, profile, err );
                    incrResult.searchMode = QLatin1String( "incremental" );
                    results.push_back( std::move( incrResult ) );
                }
                if ( runStreaming ) {
                    results.push_back(
                        benchmarkCaseStreaming( options, size, profile, err ) );
                }
            }
        }

        const auto report = buildJsonReport( options, results );
        writeJsonReport( options.outputPath, report );
        printSummary( out, options, results );

        if ( !options.keepFiles ) {
            for ( const auto& touchedFile : touchedFiles ) {
                QFile::remove( touchedFile );
            }
        }

        return 0;
    } catch ( const std::exception& ex ) {
        err << "regex_search_benchmark failed: " << ex.what() << QLatin1Char( '\n' );
    }

    return 1;
}
