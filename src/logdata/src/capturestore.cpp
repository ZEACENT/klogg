#include "capturestore.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include "log.h"
#include "readablesize.h"

namespace {
QString makeSegmentFileName( int id )
{
    return QString( "segment_%1.log" ).arg( id, 6, 10, QLatin1Char( '0' ) );
}

QString decodeUtf8Line( const QByteArray& utf8Line, QTextCodec* codec,
                        const QRegularExpression& prefilterPattern )
{
    auto line = codec ? codec->toUnicode( utf8Line ) : QString::fromUtf8( utf8Line );
    if ( !prefilterPattern.pattern().isEmpty() ) {
        line.remove( prefilterPattern );
    }
    return line;
}

void reserveSegmentMemory( QByteArray& data, qint64 targetBytes, qint64 budgetBytes )
{
    const auto reserveTarget = std::min( targetBytes, budgetBytes );
    if ( reserveTarget <= 0 ) {
        return;
    }

    const auto cappedTarget = std::min<qint64>( reserveTarget, std::numeric_limits<int>::max() );
    data.reserve( type_safe::narrow_cast<int>( cappedTarget ) );
}

// Appended lines always live at the tail of the store. Their first line number
// must be derived from the CURRENT total (after commit + trim) so it stays
// correct when trimming shifts every absolute line number down. Using the
// pre-commit total leaves firstLine stale (too large by the trimmed count).
LineNumber tailFirstLine( qint64 totalLines, LinesCount lineCount )
{
    const auto count = static_cast<qint64>( lineCount.get() );
    return LineNumber( static_cast<LineNumber::UnderlyingType>(
        totalLines >= count ? totalLines - count : 0 ) );
}

// Clamp untrusted limit inputs (session-restore JSON, hand-edited .ini) so the
// rolling window math (rollingMaxFileSize * rollingBackupCount) and the backup
// cleanup loop stay within their integer ranges.
CaptureStore::Limits sanitizeLimits( CaptureStore::Limits limits )
{
    constexpr int kMaxRollingBackupCount = 100000;
    limits.rollingBackupCount
        = std::clamp( limits.rollingBackupCount, 0, kMaxRollingBackupCount );
    return limits;
}

QDateTime latestModificationTime( const QFileInfo& entry )
{
    auto latestModification = entry.lastModified().toUTC();
    if ( !entry.isDir() ) {
        return latestModification;
    }

    QDirIterator iterator( entry.absoluteFilePath(), QDir::AllEntries | QDir::NoDotAndDotDot,
                           QDirIterator::Subdirectories );
    while ( iterator.hasNext() ) {
        iterator.next();
        const auto modified = iterator.fileInfo().lastModified().toUTC();
        if ( !latestModification.isValid() || modified > latestModification ) {
            latestModification = modified;
        }
    }

    return latestModification;
}
} // namespace

QString CaptureStore::defaultRootPath()
{
    return QDir( QDir::tempPath() ).filePath( "klogg_live" );
}

void CaptureStore::cleanupUnusedCaptures( const QSet<QString>& retainCaptureIds,
                                          const QString& rootPath,
                                          const QDateTime& preserveModifiedAfter )
{
    QDir capturesRoot( rootPath.isEmpty() ? defaultRootPath() : rootPath );
    if ( !capturesRoot.exists() ) {
        return;
    }

    const auto cutoff = preserveModifiedAfter.toUTC();
    const auto entries = capturesRoot.entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot );
    for ( const auto& entry : entries ) {
        if ( retainCaptureIds.contains( entry.fileName() ) ) {
            continue;
        }

        if ( cutoff.isValid() && latestModificationTime( entry ) > cutoff ) {
            continue;
        }

        QDir orphanCaptureDir( entry.absoluteFilePath() );
        orphanCaptureDir.removeRecursively();
    }
}

void CaptureStore::cleanupUnusedCapturesAsync( const QSet<QString>& retainCaptureIds,
                                               const QString& rootPath )
{
    const auto cleanupScheduledAt = QDateTime::currentDateTimeUtc();
    std::thread( [ retainCaptureIds, rootPath, cleanupScheduledAt ] {
        CaptureStore::cleanupUnusedCaptures( retainCaptureIds, rootPath, cleanupScheduledAt );
    } ).detach();
}

CaptureStore::CaptureStore( QString captureId, QString rootPath )
    : CaptureStore( std::move( captureId ), std::move( rootPath ), Limits{} )
{
}

CaptureStore::CaptureStore( QString captureId, QString rootPath, Limits limits )
    : captureId_( std::move( captureId ) )
    , rootPath_( rootPath.isEmpty() ? defaultRootPath() : std::move( rootPath ) )
    , capturePath_( QDir( rootPath_ ).filePath( captureId_ ) )
    , limits_( sanitizeLimits( limits ) )
{
    ensureCaptureDir();
}

CaptureStore::~CaptureStore()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    if ( persistBufferedSegmentsOnDestroy_ ) {
        finishInput();
        persistBufferedSegments();
    }
    flush();
}

bool CaptureStore::loadFromDisk()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    ensureCaptureDir();

    segments_.clear();
    partialLine_.clear();
    fileSize_ = 0;
    memoryBytes_ = 0;
    totalLines_ = 0;
    maxLineLength_ = 0;
    nextSegmentId_ = 0;
    lastModified_ = QDateTime{};

    const auto segmentFiles
        = QDir( capturePath_ ).entryList( QStringList{ "segment_*.log" }, QDir::Files,
                                          QDir::Name | QDir::IgnoreCase );
    for ( const auto& fileName : segmentFiles ) {
        Segment segment;
        segment.filePath = QDir( capturePath_ ).filePath( fileName );

        const auto numericId = QFileInfo( fileName ).baseName().mid( QString( "segment_" ).size() );
        segment.id = numericId.toInt();
        scanSegment( segment );
        nextSegmentId_ = qMax( nextSegmentId_, segment.id + 1 );
        segments_.push_back( std::move( segment ) );
    }

    rebuildCumulativeLineCounts();
    enforceMemoryBudget();
    return true;
}

CaptureStore::AppendResult CaptureStore::appendUtf8( const QByteArray& data )
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    AppendResult appendResult;
    appendResult.firstLine = LineNumber( static_cast<LineNumber::UnderlyingType>( totalLines_ ) );
    if ( data.isEmpty() ) {
        return appendResult;
    }
    appendResult.rawUtf8Lines.reserve( partialLine_.size() + data.size() );
    appendResult.endOfLines.reserve( static_cast<size_t>( qMax<qsizetype>( 1, data.size() / 32 ) ) );

    const auto originalLineCount = totalLines_;

    const auto processBuffer = [ &appendResult ]( const QByteArray& buffer ) -> qsizetype {
        qsizetype lineStart = 0;
        const auto* const bufferData = buffer.constData();
        const auto bufferSize = buffer.size();
        klogg::vector<qint64> lineEnds;
        lineEnds.reserve( static_cast<size_t>( qMax<qsizetype>( 1, bufferSize / 32 ) ) );
        bool needsNormalization = false;

        while ( lineStart < bufferSize ) {
            const auto remaining = bufferSize - lineStart;
            const auto* newline = static_cast<const char*>(
                std::memchr( bufferData + lineStart, '\n', static_cast<size_t>( remaining ) ) );
            if ( newline == nullptr ) {
                break;
            }

            auto lineLength = static_cast<qsizetype>( newline - ( bufferData + lineStart ) );
            if ( lineLength > 0 && bufferData[ lineStart + lineLength - 1 ] == '\r' ) {
                needsNormalization = true;
            }
            lineEnds.push_back( static_cast<qint64>( newline - bufferData ) + 1 );
            lineStart = static_cast<qsizetype>( newline - bufferData ) + 1;
        }

        if ( lineEnds.empty() ) {
            return lineStart;
        }

        if ( !needsNormalization ) {
            const auto outputStart = appendResult.rawUtf8Lines.size();
            appendResult.rawUtf8Lines.append( bufferData, type_safe::narrow_cast<int>( lineStart ) );
            for ( const auto lineEnd : lineEnds ) {
                appendResult.endOfLines.push_back( outputStart + lineEnd );
            }
            return lineStart;
        }

        qsizetype normalizedLineStart = 0;
        for ( const auto lineEnd : lineEnds ) {
            auto lineLength = static_cast<qsizetype>( lineEnd ) - normalizedLineStart - 1;
            if ( lineLength > 0 && bufferData[ normalizedLineStart + lineLength - 1 ] == '\r' ) {
                --lineLength;
            }

            appendResult.rawUtf8Lines.append( bufferData + normalizedLineStart,
                                               type_safe::narrow_cast<int>( lineLength ) );
            appendResult.rawUtf8Lines.append( '\n' );
            appendResult.endOfLines.push_back( appendResult.rawUtf8Lines.size() );
            normalizedLineStart = static_cast<qsizetype>( lineEnd );
        }

        return lineStart;
    };

    if ( partialLine_.isEmpty() ) {
        const auto consumed = processBuffer( data );
        if ( consumed < data.size() ) {
            partialLine_ = data.mid( type_safe::narrow_cast<int>( consumed ) );
        }
        appendResult.lineCount
            = LinesCount( static_cast<LinesCount::UnderlyingType>( appendResult.endOfLines.size() ) );
        commitLines( appendResult );
        appendResult.firstLine = tailFirstLine( totalLines_, appendResult.lineCount );
        if ( totalLines_ != originalLineCount ) {
            lastModified_ = QDateTime::currentDateTime();
        }
        return appendResult;
    }

    partialLine_.append( data );
    const auto consumed = processBuffer( partialLine_ );
    if ( consumed == partialLine_.size() ) {
        partialLine_.clear();
    }
    else if ( consumed > 0 ) {
        partialLine_ = partialLine_.mid( type_safe::narrow_cast<int>( consumed ) );
    }
    appendResult.lineCount
        = LinesCount( static_cast<LinesCount::UnderlyingType>( appendResult.endOfLines.size() ) );
    commitLines( appendResult );
    appendResult.firstLine = tailFirstLine( totalLines_, appendResult.lineCount );
    if ( totalLines_ != originalLineCount ) {
        lastModified_ = QDateTime::currentDateTime();
    }
    return appendResult;
}

CaptureStore::AppendResult CaptureStore::finishInput()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    AppendResult appendResult;
    appendResult.firstLine = LineNumber( static_cast<LineNumber::UnderlyingType>( totalLines_ ) );
    if ( !partialLine_.isEmpty() ) {
        auto lineBytes = partialLine_;
        partialLine_.clear();
        if ( lineBytes.endsWith( '\r' ) ) {
            lineBytes.chop( 1 );
        }

        commitLine( lineBytes, false );
        appendResult.rawUtf8Lines.append( lineBytes );
        appendResult.rawUtf8Lines.append( '\n' );
        appendResult.endOfLines.push_back( appendResult.rawUtf8Lines.size() );
        appendResult.lineCount = 1_lcount;
        lastModified_ = QDateTime::currentDateTime();
    }
    appendResult.firstLine = tailFirstLine( totalLines_, appendResult.lineCount );

    // Flush any pending output data
    if ( rollingOutput_.isValid() && unflushedOutputBytes_ > 0 ) {
        rollingOutput_.flush();
        resetOutputFlushCounters();
    }
    return appendResult;
}

void CaptureStore::flush()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    if ( rollingOutput_.isValid() && unflushedOutputBytes_ > 0 ) {
        rollingOutput_.flush();
        resetOutputFlushCounters();
    }
}

void CaptureStore::clear()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    flush();

    partialLine_.clear();
    segments_.clear();
    fileSize_ = 0;
    memoryBytes_ = 0;
    totalLines_ = 0;
    maxLineLength_ = 0;
    nextSegmentId_ = 0;
    lastModified_ = QDateTime::currentDateTime();
    lastTrimResult_ = {};

    const auto files = QDir( capturePath_ ).entryList( QDir::Files | QDir::NoDotAndDotDot );
    for ( const auto& fileName : files ) {
        QFile::remove( QDir( capturePath_ ).filePath( fileName ) );
    }

    if ( !boundOutputFile_.isEmpty() ) {
        rollingOutput_.deleteAll();
        rollingOutput_ = RollingFileManager( boundOutputFile_, limits_.rollingMaxFileSize,
                                             limits_.rollingBackupCount );
        if ( !rollingOutput_.open( true ) ) {
            LOG_WARNING << "CaptureStore::clear: failed to reopen output file: "
                        << boundOutputFile_;
            boundOutputFile_.clear();
        }
    }
}

CaptureStore::TrimResult CaptureStore::trimToLimits()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    TrimResult result;

    if ( segments_.size() <= 1 ) {
        return result;
    }

    // Rolling window: rollingMaxFileSize * rollingBackupCount.
    // backupCount is the number of retained backups; the current file is
    // tracked separately via fileSize_.  Use checked multiplication to guard
    // against overflow from hand-edited or session-restored limits.
    qint64 windowBytes = 0;
    if ( limits_.rollingMaxFileSize > 0 && limits_.rollingBackupCount > 0 ) {
        const auto backupCount = static_cast<qint64>( limits_.rollingBackupCount );
        windowBytes
            = limits_.rollingMaxFileSize > std::numeric_limits<qint64>::max() / backupCount
                  ? std::numeric_limits<qint64>::max()
                  : limits_.rollingMaxFileSize * backupCount;
    }
    const auto exceedsBytes = windowBytes > 0 && fileSize_ > windowBytes;
    const auto exceedsLines = limits_.maxTotalLines > 0 && totalLines_ > limits_.maxTotalLines;

    if ( !exceedsBytes && !exceedsLines ) {
        return result;
    }

    LinesCount::UnderlyingType totalRemovedLines = 0;
    bool removedMaxLine = false;

    // Remove oldest segments (FIFO) until within limits.
    // Never remove the active (last) segment.
    while ( segments_.size() > 1 ) {
        const auto stillOverBytes = windowBytes > 0 && fileSize_ > windowBytes;
        const auto stillOverLines
            = limits_.maxTotalLines > 0 && totalLines_ > limits_.maxTotalLines;

        if ( !stillOverBytes && !stillOverLines ) {
            break;
        }

        auto& front = segments_.front();
        const auto segmentLines = klogg::ssize( front.lineOffsets );
        const auto segmentBytes = front.byteSize;

        // Track if we're removing the segment that held maxLineLength
        for ( const auto len : front.lineLengths ) {
            if ( len == maxLineLength_ ) {
                removedMaxLine = true;
            }
        }

        // Delete disk file if spilled
        if ( front.spilled && !front.filePath.isEmpty() ) {
            QFile::remove( front.filePath );
        }

        fileSize_ -= segmentBytes;
        if ( front.memoryData ) {
            memoryBytes_ -= front.memoryData->size();
        }
        totalLines_ -= segmentLines;
        totalRemovedLines += static_cast<LinesCount::UnderlyingType>( segmentLines );

        result.trimmedLines = result.trimmedLines + LinesCount( static_cast<LinesCount::UnderlyingType>( segmentLines ) );
        result.trimmedBytes += segmentBytes;

        segments_.erase( segments_.begin() );
    }

    // O(1) cumulative line count fixup: subtract removed lines from all remaining segments
    if ( totalRemovedLines > 0 ) {
        for ( auto& segment : segments_ ) {
            segment.cumulativeEndLine
                -= static_cast<qint64>( totalRemovedLines );
        }
    }

    // Only recompute maxLineLength if we removed the segment that held it
    if ( removedMaxLine ) {
        maxLineLength_ = 0;
        for ( const auto& segment : segments_ ) {
            for ( const auto len : segment.lineLengths ) {
                maxLineLength_ = qMax( maxLineLength_, len );
            }
        }
    }

    if ( result.trimmedLines > 0_lcount ) {
        lastModified_ = QDateTime::currentDateTime();
        lastTrimResult_ = result;

        LOG_INFO << "CaptureStore trimmed " << result.trimmedLines.get() << " lines ("
                 << result.trimmedBytes << " bytes), remaining: " << totalLines_ << " lines, "
                 << fileSize_ << " bytes";
    }

    return result;
}

CaptureStore::TrimResult CaptureStore::lastTrimResult() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return lastTrimResult_;
}

void CaptureStore::clearTrimResult()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    lastTrimResult_ = {};
}

bool CaptureStore::bindOutputFile( const QString& outputPath )
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    rollingOutput_.close();
    rollingOutput_ = RollingFileManager();
    boundOutputFile_ = outputPath;
    if ( boundOutputFile_.isEmpty() ) {
        return true;
    }

    QDir().mkpath( QFileInfo( boundOutputFile_ ).absolutePath() );

    rollingOutput_ = RollingFileManager( boundOutputFile_, limits_.rollingMaxFileSize,
                                         limits_.rollingBackupCount );
    if ( !rollingOutput_.open( true ) ) {
        boundOutputFile_.clear();
        return false;
    }

    // Write existing segments to the rolling file
    for ( const auto& segment : segments_ ) {
        if ( !writeSegmentToDevice( segment, rollingOutput_.currentFile() ) ) {
            boundOutputFile_.clear();
            rollingOutput_.close();
            return false;
        }
    }
    // Replay writes directly to QFile, bypassing RollingFileManager::write().
    // Sync the byte counter so needsRotation() reflects the true file size.
    rollingOutput_.resyncSize();
    rollingOutput_.flush();

    resetOutputFlushCounters();
    return true;
}

void CaptureStore::setOutputFlushedCallback( std::function<void()> callback )
{
    outputFlushedCallback_ = std::move( callback );
}

void CaptureStore::setLimits( Limits limits )
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    limits_ = sanitizeLimits( std::move( limits ) );
}

QString CaptureStore::boundOutputFile() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return boundOutputFile_;
}

QString CaptureStore::captureId() const
{
    return captureId_;
}

QString CaptureStore::capturePath() const
{
    return capturePath_;
}

QString CaptureStore::rootPath() const
{
    return rootPath_;
}

void CaptureStore::deleteCaptureFiles()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    flush();
    rollingOutput_.close();
    rollingOutput_ = RollingFileManager();
    persistBufferedSegmentsOnDestroy_ = false;
    partialLine_.clear();
    segments_.clear();
    fileSize_ = 0;
    memoryBytes_ = 0;
    totalLines_ = 0;
    maxLineLength_ = 0;
    nextSegmentId_ = 0;
    lastModified_ = QDateTime{};
    QDir captureDir( capturePath_ );
    if ( captureDir.exists() ) {
        captureDir.removeRecursively();
    }
}

SearchableLogData::RawLines CaptureStore::buildRawLines( LineNumber first, LinesCount number,
                                                         QTextCodec* codec,
                                                         const QRegularExpression& prefilterPattern ) const
{
    // Segment-based batch read: identify contiguous byte ranges per segment
    // under the lock, then bulk-copy directly into the output buffer.
    // This avoids per-line QByteArray allocations that dominate the old path.

    struct SegmentRead {
        std::shared_ptr<QByteArray> memoryData;
        QString filePath;
        qint64 byteStart;
        qint64 byteLength;
        qint64 lineCount;
    };

    LinesCount::UnderlyingType totalRequestedLines = 0;
    std::vector<SegmentRead> segmentReads;

    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        const auto totalLines = lineCount();
        const auto availableLines = qMax<LineNumber::UnderlyingType>(
            0, totalLines.get() - qMin( first.get(), totalLines.get() ) );
        totalRequestedLines
            = qMin( number.get(), static_cast<LinesCount::UnderlyingType>( availableLines ) );

        if ( totalRequestedLines == 0 ) {
            SearchableLogData::RawLines rawLines;
            rawLines.startLine = first;
            return rawLines;
        }

        segmentReads.reserve( 4 );

        auto segIt = std::lower_bound(
            segments_.cbegin(), segments_.cend(), first.get(),
            []( const Segment& segment, qint64 value ) {
                return segment.cumulativeEndLine <= value;
            } );

        if ( segIt == segments_.cend() ) {
            SearchableLogData::RawLines rawLines;
            rawLines.startLine = first;
            return rawLines;
        }

        LinesCount::UnderlyingType linesRemaining = totalRequestedLines;
        auto currentLine = first;

        while ( linesRemaining > 0 && segIt != segments_.cend() ) {
            const auto segIdx
                = static_cast<size_t>( std::distance( segments_.cbegin(), segIt ) );
            const qint64 prevEndLine
                = segIdx == 0 ? 0LL : segments_[ segIdx - 1 ].cumulativeEndLine;

            const auto localStart = static_cast<int>( currentLine.get<qint64>() - prevEndLine );
            const auto linesInThisSegment
                = qMin( linesRemaining,
                        static_cast<LinesCount::UnderlyingType>(
                            klogg::ssize( segIt->lineOffsets ) - localStart ) );

            if ( linesInThisSegment <= 0 || localStart < 0
                 || localStart >= klogg::ssize( segIt->lineOffsets ) ) {
                break;
            }

            SegmentRead read;
            read.memoryData = segIt->memoryData;
            read.filePath = segIt->filePath;
            read.lineCount = static_cast<qint64>( linesInThisSegment );

            read.byteStart = segIt->lineOffsets[ static_cast<size_t>( localStart ) ];
            const auto lastLocalLine = localStart + static_cast<int>( linesInThisSegment ) - 1;

            if ( lastLocalLine + 1 < klogg::ssize( segIt->lineOffsets ) ) {
                read.byteLength = segIt->lineOffsets[ static_cast<size_t>( lastLocalLine + 1 ) ]
                                - read.byteStart;
            } else {
                read.byteLength = segIt->byteSize - read.byteStart;
            }

            segmentReads.push_back( std::move( read ) );

            linesRemaining -= linesInThisSegment;
            currentLine = currentLine + LinesCount( linesInThisSegment );
            ++segIt;
        }
    }
    // mutex released

    const auto effectiveCodec = codec ? codec : QTextCodec::codecForName( "UTF-8" );
    const auto sourceEncodingParams = EncodingParameters( effectiveCodec );
    const auto canUseRawUtf8
        = sourceEncodingParams.isUtf8Compatible && prefilterPattern.pattern().isEmpty();

    SearchableLogData::RawLines rawLines;
    rawLines.startLine = first;
    auto* utf8Codec = QTextCodec::codecForName( "UTF-8" );
    rawLines.textDecoder.decoder.reset( utf8Codec->makeDecoder() );
    rawLines.textDecoder.encodingParams = sourceEncodingParams;
    rawLines.textDecoder.encodingParams.isUtf8Compatible = true;
    rawLines.textDecoder.encodingParams.lineFeedWidth = 1;
    rawLines.prefilterPattern = prefilterPattern;

    if ( canUseRawUtf8 ) {
        // Fast path: bulk copy from segments, derive endOfLines from \n scanning.
        // Segment data stores each line as content+\n, so scanning for \n gives
        // exact line boundaries without per-line allocation.
        qint64 totalBytes = 0;
        for ( const auto& read : segmentReads ) {
            totalBytes += read.byteLength;
        }
        rawLines.buffer.reserve( static_cast<size_t>( totalBytes ) );
        rawLines.endOfLines.reserve( static_cast<size_t>( totalRequestedLines ) );

        for ( const auto& read : segmentReads ) {
            const auto outputStart = klogg::ssize( rawLines.buffer );

            if ( read.memoryData ) {
                const auto* src = read.memoryData->constData() + read.byteStart;
                rawLines.buffer.insert( rawLines.buffer.end(), src, src + read.byteLength );
            } else {
                QFile file( read.filePath );
                if ( file.open( QIODevice::ReadOnly ) ) {
                    file.seek( read.byteStart );
                    const auto data = file.read( read.byteLength );
                    if ( data.size() > 0 ) {
                        rawLines.buffer.insert( rawLines.buffer.end(), data.constBegin(),
                                                data.constEnd() );
                    }
                }
            }

            // Scan for \n to compute endOfLines
            const auto* scanStart = rawLines.buffer.data() + outputStart;
            const auto scanLength = klogg::ssize( rawLines.buffer ) - outputStart;
            qint64 linesFound = 0;
            for ( qint64 pos = 0; pos < scanLength && linesFound < read.lineCount; ++pos ) {
                if ( scanStart[ pos ] == '\n' ) {
                    rawLines.endOfLines.push_back( outputStart + pos + 1 );
                    ++linesFound;
                }
            }

            // Handle unterminated last line
            if ( linesFound < read.lineCount ) {
                rawLines.buffer.push_back( '\n' );
                rawLines.endOfLines.push_back( klogg::ssize( rawLines.buffer ) );
            }
        }
    } else {
        // Slow path: bulk read per segment, per-line codec conversion or prefilter
        for ( const auto& read : segmentReads ) {
            QByteArray segmentData;
            if ( read.memoryData ) {
                segmentData = read.memoryData->mid( type_safe::narrow_cast<int>( read.byteStart ),
                                                    type_safe::narrow_cast<int>( read.byteLength ) );
            } else {
                QFile file( read.filePath );
                if ( file.open( QIODevice::ReadOnly ) ) {
                    file.seek( read.byteStart );
                    segmentData = file.read( read.byteLength );
                }
            }

            qint64 lineStart = 0;
            for ( qint64 lineIdx = 0; lineIdx < read.lineCount; ++lineIdx ) {
                const auto remaining = segmentData.size() - static_cast<int>( lineStart );
                if ( remaining <= 0 ) {
                    break;
                }
                const auto* nl = static_cast<const char*>(
                    std::memchr( segmentData.constData() + static_cast<int>( lineStart ), '\n',
                                 static_cast<size_t>( remaining ) ) );
                qint64 lineEnd;
                if ( nl ) {
                    lineEnd = nl - segmentData.constData();
                } else {
                    lineEnd = segmentData.size();
                }

                auto lineLength = lineEnd - lineStart;
                if ( lineLength > 0 && segmentData[ static_cast<int>( lineStart + lineLength - 1 ) ] == '\r' ) {
                    --lineLength;
                }

                const auto utf8Line = segmentData.mid( static_cast<int>( lineStart ),
                                                       static_cast<int>( lineLength ) );
                const auto lineStr = decodeUtf8Line( utf8Line, effectiveCodec, prefilterPattern );
                const auto lineUtf8 = lineStr.toUtf8();
                rawLines.buffer.insert( rawLines.buffer.end(), lineUtf8.begin(), lineUtf8.end() );
                rawLines.buffer.push_back( '\n' );
                rawLines.endOfLines.push_back( klogg::ssize( rawLines.buffer ) );

                lineStart = lineEnd + 1;
            }
        }
    }

    return rawLines;
}

QString CaptureStore::lineAt( LineNumber line, QTextCodec* codec,
                              const QRegularExpression& prefilterPattern ) const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    if ( line < 0_lnum || line >= lineCount() ) {
        return {};
    }

    const auto segmentIt
        = std::lower_bound( segments_.cbegin(), segments_.cend(), line.get(),
                            []( const Segment& segment, qint64 value ) {
                                return segment.cumulativeEndLine <= value;
                            } );
    if ( segmentIt == segments_.cend() ) {
        return {};
    }

    const auto segmentIndex = static_cast<size_t>( std::distance( segments_.cbegin(), segmentIt ) );
    const qint64 previousEndLine
        = segmentIndex == 0 ? 0LL : segments_[ segmentIndex - 1 ].cumulativeEndLine;
    const auto localLine = static_cast<int>( line.get<qint64>() - previousEndLine );

    const auto utf8Line = readSegmentLine( *segmentIt, localLine );
    return decodeUtf8Line( utf8Line, codec, prefilterPattern );
}

LineLength CaptureStore::lineLength( LineNumber line ) const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    if ( line < 0_lnum || line >= lineCount() ) {
        return 0_length;
    }

    const auto segmentIt
        = std::lower_bound( segments_.cbegin(), segments_.cend(), line.get(),
                            []( const Segment& segment, qint64 value ) {
                                return segment.cumulativeEndLine <= value;
                            } );
    if ( segmentIt == segments_.cend() ) {
        return 0_length;
    }

    const auto segmentIndex = static_cast<size_t>( std::distance( segments_.cbegin(), segmentIt ) );
    const qint64 previousEndLine
        = segmentIndex == 0 ? 0LL : segments_[ segmentIndex - 1 ].cumulativeEndLine;
    const auto localLine = static_cast<int>( line.get<qint64>() - previousEndLine );
    if ( localLine < 0 || localLine >= klogg::isize( segmentIt->lineLengths ) ) {
        return 0_length;
    }
    return LineLength( segmentIt->lineLengths[ static_cast<size_t>( localLine ) ] );
}

LinesCount CaptureStore::lineCount() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return LinesCount( static_cast<LinesCount::UnderlyingType>( totalLines_ ) );
}

LineLength CaptureStore::maxLineLength() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return LineLength( maxLineLength_ );
}

CaptureStore::Stats CaptureStore::stats() const
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    return Stats{ fileSize_, memoryBytes_, totalLines_, maxLineLength_, lastModified_ };
}

void CaptureStore::commitLine( const QByteArray& lineBytes, bool terminated )
{
    auto& segment = ensureActiveSegment();
    if ( !segment.memoryData ) {
        segment.memoryData = std::make_shared<QByteArray>();
        reserveSegmentMemory( *segment.memoryData, limits_.segmentTargetBytes,
                              limits_.memoryBudgetBytes );
    }

    const auto offset = segment.memoryData->size();
    segment.memoryData->append( lineBytes );
    if ( terminated ) {
        segment.memoryData->append( '\n' );
    }
    segment.lineOffsets.push_back( offset );
    segment.lineLengths.push_back( static_cast<int>( lineBytes.size() ) );
    segment.byteSize = segment.memoryData->size();
    segment.spilled = false;

    if ( rollingOutput_.isValid() ) {
        appendOutputBytes( terminated ? lineBytes + '\n' : lineBytes );
    }

    fileSize_ += lineBytes.size() + ( terminated ? 1 : 0 );
    memoryBytes_ += lineBytes.size() + ( terminated ? 1 : 0 );
    totalLines_ += 1;
    maxLineLength_ = qMax( maxLineLength_, static_cast<int>( lineBytes.size() ) );

    segment.cumulativeEndLine += 1;
    rotateSegmentIfNeeded();
    if ( memoryBytes_ > limits_.memoryBudgetBytes ) {
        enforceMemoryBudget();
    }
}

void CaptureStore::commitLines( const AppendResult& appendResult )
{
    if ( appendResult.endOfLines.empty() ) {
        return;
    }

    if ( rollingOutput_.isValid() ) {
        if ( appendResult.endOfLines.size()
             > static_cast<size_t>( std::numeric_limits<int>::max() ) ) {
            throw std::runtime_error( "Too many output lines while committing capture batch" );
        }
        appendOutputBytes( appendResult.rawUtf8Lines,
                           static_cast<int>( appendResult.endOfLines.size() ) );
    }

    size_t lineIndex = 0;
    qint64 rawLineStart = 0;
    const auto lineCount = appendResult.endOfLines.size();

    while ( lineIndex < lineCount ) {
        auto& segment = ensureActiveSegment();
        if ( !segment.memoryData ) {
            segment.memoryData = std::make_shared<QByteArray>();
            reserveSegmentMemory( *segment.memoryData, limits_.segmentTargetBytes,
                                  limits_.memoryBudgetBytes );
        }

        const auto segmentRawStart = rawLineStart;
        const auto firstLineInSegment = lineIndex;
        auto segmentBytes = qint64{ 0 };
        auto segmentMaxLineLength = 0;

        while ( lineIndex < lineCount ) {
            const auto lineEnd = appendResult.endOfLines[ lineIndex ];
            const auto lineBytes = lineEnd - rawLineStart;
            if ( lineIndex > firstLineInSegment
                 && segment.byteSize + segmentBytes + lineBytes > limits_.segmentTargetBytes ) {
                break;
            }

            if ( lineBytes <= 0 || lineBytes - 1 > std::numeric_limits<int>::max() ) {
                throw std::runtime_error( "Invalid append line length while committing capture batch" );
            }
            segmentMaxLineLength = qMax( segmentMaxLineLength,
                                         type_safe::narrow_cast<int>( lineBytes - 1 ) );
            segmentBytes += lineBytes;
            rawLineStart = lineEnd;
            ++lineIndex;

            if ( segment.byteSize + segmentBytes >= limits_.segmentTargetBytes ) {
                break;
            }
        }

        const auto segmentOffset = segment.memoryData->size();
        if ( segmentBytes < 0 || segmentBytes > std::numeric_limits<int>::max() ) {
            throw std::runtime_error( "Invalid segment byte count while committing capture batch" );
        }
        segment.memoryData->append( appendResult.rawUtf8Lines.constData() + segmentRawStart,
                                    type_safe::narrow_cast<int>( segmentBytes ) );

        qint64 localRawStart = segmentRawStart;
        qint64 localOffset = segmentOffset;
        for ( auto metadataIndex = firstLineInSegment; metadataIndex < lineIndex;
              ++metadataIndex ) {
            const auto lineEnd = appendResult.endOfLines[ metadataIndex ];
            const auto lineBytes = lineEnd - localRawStart;
            if ( lineBytes <= 0 || lineBytes - 1 > std::numeric_limits<int>::max() ) {
                throw std::runtime_error( "Invalid metadata line length while committing capture batch" );
            }
            segment.lineOffsets.push_back( localOffset );
            segment.lineLengths.push_back( type_safe::narrow_cast<int>( lineBytes - 1 ) );
            localOffset += lineBytes;
            localRawStart = lineEnd;
        }

        const auto committedLines
            = static_cast<qint64>( lineIndex - firstLineInSegment );
        segment.byteSize = segment.memoryData->size();
        segment.spilled = false;
        segment.cumulativeEndLine += committedLines;

        fileSize_ += segmentBytes;
        memoryBytes_ += segmentBytes;
        totalLines_ += committedLines;
        maxLineLength_ = qMax( maxLineLength_, segmentMaxLineLength );

        rotateSegmentIfNeeded();
        // Throttle spill operations to avoid frequent small spills under heavy load.
        // Emergency spill if memory exceeds 2x budget.
        if ( memoryBytes_ > limits_.memoryBudgetBytes ) {
            const auto now = QDateTime::currentMSecsSinceEpoch();
            if ( memoryBytes_ > limits_.memoryBudgetBytes * 2
                 || now - lastSpillTimeMs_ >= SpillThrottleMs ) {
                lastSpillTimeMs_ = now;
                enforceMemoryBudget();
            }
        }
    }

    // Trim oldest segments if total limits are exceeded
    if ( limits_.rollingMaxFileSize > 0 || limits_.maxTotalLines > 0 ) {
        trimToLimits();
    }
}

void CaptureStore::ensureCaptureDir()
{
    QDir().mkpath( capturePath_ );
}

CaptureStore::Segment& CaptureStore::ensureActiveSegment()
{
    if ( segments_.empty() || segments_.back().byteSize >= limits_.segmentTargetBytes
         || segments_.back().spilled || !segments_.back().memoryData ) {
        const auto prevCumulative
            = segments_.empty() ? 0LL : segments_.back().cumulativeEndLine;
        Segment segment;
        segment.id = nextSegmentId_++;
        segment.filePath = QDir( capturePath_ ).filePath( makeSegmentFileName( segment.id ) );
        segment.memoryData = std::make_shared<QByteArray>();
        reserveSegmentMemory( *segment.memoryData, limits_.segmentTargetBytes,
                              limits_.memoryBudgetBytes );
        segment.cumulativeEndLine = prevCumulative;
        segments_.push_back( std::move( segment ) );
    }

    return segments_.back();
}

void CaptureStore::rotateSegmentIfNeeded()
{
    if ( segments_.empty() || segments_.back().byteSize < limits_.segmentTargetBytes ) {
        return;
    }

    // Carry forward the cumulative line count so the new (empty) segment
    // doesn't break the sorted cumulativeEndLine invariant that lineAt()
    // and buildRawLines() rely on for binary search.
    const auto prevCumulative = segments_.back().cumulativeEndLine;

    Segment segment;
    segment.id = nextSegmentId_++;
    segment.filePath = QDir( capturePath_ ).filePath( makeSegmentFileName( segment.id ) );
    segment.memoryData = std::make_shared<QByteArray>();
    reserveSegmentMemory( *segment.memoryData, limits_.segmentTargetBytes,
                          limits_.memoryBudgetBytes );
    segment.cumulativeEndLine = prevCumulative;
    segments_.push_back( std::move( segment ) );
}

void CaptureStore::rebuildCumulativeLineCounts( bool onlyLast )
{
    if ( onlyLast && !segments_.empty() ) {
        // O(1) fast path: only update the active (last) segment.
        // commitLine() tracks memoryBytes_ incrementally, so we only need to
        // fix up cumulativeEndLine for the last segment.
        auto& last = segments_.back();
        const qint64 prevEnd = segments_.size() > 1
                                   ? segments_[ segments_.size() - 2 ].cumulativeEndLine
                                   : 0;
        last.cumulativeEndLine = prevEnd + klogg::ssize( last.lineOffsets );
        return;
    }

    // Full rebuild -- used after loadFromDisk(), clear(), etc.
    qint64 cumulative = 0;
    memoryBytes_ = 0;
    for ( auto& segment : segments_ ) {
        cumulative += klogg::ssize( segment.lineOffsets );
        segment.cumulativeEndLine = cumulative;
        if ( segment.memoryData ) {
            memoryBytes_ += segment.memoryData->size();
        }
    }
}

void CaptureStore::enforceMemoryBudget()
{
    for ( auto& segment : segments_ ) {
        if ( memoryBytes_ <= limits_.memoryBudgetBytes ) {
            break;
        }
        if ( &segment == &segments_.back() ) {
            break;
        }
        if ( segment.memoryData && spillSegmentToDisk( segment ) ) {
            memoryBytes_ -= segment.memoryData->size();
            segment.memoryData.reset();
        }
    }
}

void CaptureStore::scanSegment( Segment& segment )
{
    QFile file( segment.filePath );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return;
    }

    segment.byteSize = file.size();
    segment.spilled = true;
    lastModified_ = QFileInfo( segment.filePath ).lastModified();

    qint64 offset = 0;
    while ( !file.atEnd() ) {
        const auto lineBytes = file.readLine();
        if ( lineBytes.isEmpty() ) {
            break;
        }
        auto lineLength = lineBytes.endsWith( '\n' ) ? lineBytes.size() - 1 : lineBytes.size();
        if ( lineLength > 0 && lineBytes[ lineLength - 1 ] == '\r' ) {
            --lineLength;
        }
        segment.lineOffsets.push_back( offset );
        segment.lineLengths.push_back( type_safe::narrow_cast<int>( lineLength ) );
        fileSize_ += lineBytes.size();
        maxLineLength_ = qMax( maxLineLength_, type_safe::narrow_cast<int>( lineLength ) );
        offset += lineBytes.size();
    }
    totalLines_ += klogg::ssize( segment.lineOffsets );
}

bool CaptureStore::spillSegmentToDisk( Segment& segment )
{
    if ( segment.spilled || !segment.memoryData ) {
        return true;
    }
    if ( segment.memoryData->isEmpty() ) {
        return true;
    }

    ensureCaptureDir();

    QFile file( segment.filePath );
    if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        LOG_WARNING << "Failed to spill capture segment " << segment.filePath;
        return false;
    }

    if ( file.write( *segment.memoryData ) != segment.memoryData->size() ) {
        LOG_WARNING << "Failed to write capture segment " << segment.filePath;
        return false;
    }

    segment.spilled = true;
    return true;
}

void CaptureStore::persistBufferedSegments()
{
    for ( auto& segment : segments_ ) {
        spillSegmentToDisk( segment );
    }
}

QByteArray CaptureStore::readSegmentLine( const Segment& segment, int localLine ) const
{
    if ( localLine < 0 || localLine >= klogg::isize( segment.lineOffsets ) ) {
        return {};
    }

    if ( segment.memoryData ) {
        const auto lineOffset = segment.lineOffsets[ static_cast<size_t>( localLine ) ];
        const auto lineLength = segment.lineLengths[ static_cast<size_t>( localLine ) ];
        return segment.memoryData->mid( type_safe::narrow_cast<int>( lineOffset ), lineLength );
    }

    QFile file( segment.filePath );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return {};
    }
    const auto lineOffset = segment.lineOffsets[ static_cast<size_t>( localLine ) ];
    const auto lineLength = segment.lineLengths[ static_cast<size_t>( localLine ) ];
    file.seek( lineOffset );
    return file.read( lineLength );
}

bool CaptureStore::writeSegmentToDevice( const Segment& segment, QIODevice* device ) const
{
    if ( !device ) {
        return false;
    }

    if ( segment.memoryData ) {
        return device->write( *segment.memoryData ) == segment.memoryData->size();
    }

    QFile file( segment.filePath );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    while ( true ) {
        const auto bytesRead = file.read( buffer.data(), type_safe::narrow_cast<qint64>( buffer.size() ) );
        if ( bytesRead < 0 ) {
            return false;
        }
        if ( bytesRead == 0 ) {
            break;
        }
        if ( device->write( buffer.data(), bytesRead ) != bytesRead ) {
            return false;
        }
    }

    return true;
}

void CaptureStore::appendOutputBytes( const QByteArray& bytes, int lineCount )
{
    if ( !rollingOutput_.isValid() ) {
        return;
    }

    const auto written = rollingOutput_.write( bytes );
    if ( written <= 0 ) {
        LOG_WARNING << "Rolling output file write failed, unbinding: " << boundOutputFile_;
        rollingOutput_.close();
        rollingOutput_ = RollingFileManager();
        boundOutputFile_.clear();
        return;
    }

    // write() now writes the whole batch across rotations and reports whether it
    // rotated. The previous size-before/after heuristic missed rotations that
    // left the new file at least as large as the old one (e.g. a single write
    // that fills, rotates and writes a large remainder from an empty file),
    // skipping the in-memory window trim on the single-line commit path.
    if ( rollingOutput_.rotated() ) {
        trimToWindowSize();
    }

    unflushedOutputBytes_ += written;
    unflushedOutputLines_ += lineCount;
    flushOutputIfNeeded();
}

void CaptureStore::flushOutputIfNeeded()
{
    if ( !rollingOutput_.isValid() || unflushedOutputBytes_ == 0 ) {
        return;
    }

    if ( unflushedOutputBytes_ >= OutputFlushBytesThreshold
         || unflushedOutputLines_ >= OutputFlushLinesThreshold ) {
        if ( !rollingOutput_.flush() ) {
            LOG_WARNING << "Rolling output file flush failed, unbinding: "
                        << boundOutputFile_;
            rollingOutput_.close();
            rollingOutput_ = RollingFileManager();
            boundOutputFile_.clear();
            resetOutputFlushCounters();
            return;
        }
        resetOutputFlushCounters();
        if ( outputFlushedCallback_ ) {
            outputFlushedCallback_();
        }
    }
}

void CaptureStore::resetOutputFlushCounters()
{
    unflushedOutputBytes_ = 0;
    unflushedOutputLines_ = 0;
}

void CaptureStore::trimToWindowSize()
{
    // Called when the rolling file rotates. Trim in-memory segments to match the window.
    // trimToLimits() already checks whether any limits are configured and
    // acquires the (recursive) mutex — safe because the caller chain
    // (appendOutputBytes → commitLines → appendUtf8) already holds it.
    trimToLimits();
}
