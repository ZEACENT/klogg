#include "capturestore.h"

#include <algorithm>
#include <array>

#include <QDir>
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
} // namespace

QString CaptureStore::defaultRootPath()
{
    return QDir( QDir::tempPath() ).filePath( "klogg_live" );
}

void CaptureStore::cleanupUnusedCaptures( const QSet<QString>& retainCaptureIds,
                                          const QString& rootPath )
{
    QDir capturesRoot( rootPath.isEmpty() ? defaultRootPath() : rootPath );
    if ( !capturesRoot.exists() ) {
        return;
    }

    const auto entries = capturesRoot.entryInfoList( QDir::Dirs | QDir::NoDotAndDotDot );
    for ( const auto& entry : entries ) {
        if ( retainCaptureIds.contains( entry.fileName() ) ) {
            continue;
        }

        QDir orphanCaptureDir( entry.absoluteFilePath() );
        orphanCaptureDir.removeRecursively();
    }
}

CaptureStore::CaptureStore( QString captureId, QString rootPath )
    : CaptureStore( std::move( captureId ), std::move( rootPath ), Limits{} )
{
}

CaptureStore::CaptureStore( QString captureId, QString rootPath, Limits limits )
    : captureId_( std::move( captureId ) )
    , rootPath_( rootPath.isEmpty() ? defaultRootPath() : std::move( rootPath ) )
    , capturePath_( QDir( rootPath_ ).filePath( captureId_ ) )
    , limits_( limits )
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

void CaptureStore::appendUtf8( const QByteArray& data )
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    if ( data.isEmpty() ) {
        return;
    }

    partialLine_.append( data );

    auto newlineIndex = partialLine_.indexOf( '\n' );
    while ( newlineIndex >= 0 ) {
        auto lineBytes = partialLine_.left( newlineIndex );
        partialLine_.remove( 0, newlineIndex + 1 );

        if ( lineBytes.endsWith( '\r' ) ) {
            lineBytes.chop( 1 );
        }

        commitLine( lineBytes, true );
        newlineIndex = partialLine_.indexOf( '\n' );
    }
}

void CaptureStore::finishInput()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    if ( !partialLine_.isEmpty() ) {
        auto lineBytes = partialLine_;
        partialLine_.clear();
        if ( lineBytes.endsWith( '\r' ) ) {
            lineBytes.chop( 1 );
        }

        commitLine( lineBytes, false );
    }

    // Flush any pending output data
    if ( boundOutputHandle_ && unflushedOutputBytes_ > 0 ) {
        boundOutputHandle_->flush();
        resetOutputFlushCounters();
    }
}

void CaptureStore::flush()
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    if ( boundOutputHandle_ ) {
        boundOutputHandle_->flush();
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

    const auto files = QDir( capturePath_ ).entryList( QDir::Files | QDir::NoDotAndDotDot );
    for ( const auto& fileName : files ) {
        QFile::remove( QDir( capturePath_ ).filePath( fileName ) );
    }

    if ( !boundOutputFile_.isEmpty() ) {
        QFile outputFile( boundOutputFile_ );
        if ( outputFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
            outputFile.close();
        }
        boundOutputHandle_.reset();
        bindOutputFile( boundOutputFile_ );
    }
}

bool CaptureStore::bindOutputFile( const QString& outputPath )
{
    const std::lock_guard<std::recursive_mutex> lock( mutex_ );
    boundOutputHandle_.reset();
    boundOutputFile_ = outputPath;
    if ( boundOutputFile_.isEmpty() ) {
        return true;
    }

    QDir().mkpath( QFileInfo( boundOutputFile_ ).absolutePath() );
    auto outputFile = std::make_unique<QFile>( boundOutputFile_ );
    if ( !outputFile->open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        boundOutputFile_.clear();
        return false;
    }

    if ( !writeCaptureToDevice( outputFile.get() ) || !outputFile->flush() ) {
        boundOutputFile_.clear();
        return false;
    }

    boundOutputHandle_ = std::move( outputFile );
    resetOutputFlushCounters();
    return true;
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
    boundOutputHandle_.reset();
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
    // Snapshot-based two-phase read: take lightweight references under the lock,
    // then build the output buffer without holding the mutex.  This prevents the
    // search worker thread from blocking appendUtf8() on the main thread.

    struct LineRef {
        std::shared_ptr<QByteArray> memoryData;
        QString filePath;
        qint64 offset;
        int length;
    };

    LinesCount::UnderlyingType requestedLines = 0;
    std::vector<LineRef> lineRefs;

    {
        const std::lock_guard<std::recursive_mutex> lock( mutex_ );
        const auto totalLines = lineCount();
        const auto availableLines = qMax<LineNumber::UnderlyingType>(
            0, totalLines.get() - qMin( first.get(), totalLines.get() ) );
        requestedLines
            = qMin( number.get(), static_cast<LinesCount::UnderlyingType>( availableLines ) );
        lineRefs.reserve( requestedLines );

        // Cache the current segment to avoid repeated binary searches.
        // Consecutive lines almost always fall within the same segment.
        auto cachedSegIt = segments_.cend();
        qint64 cachedPrevEnd = 0;

        for ( LinesCount::UnderlyingType lineOffset = 0; lineOffset < requestedLines;
              ++lineOffset ) {
            const auto lineNum = first + LinesCount( lineOffset );
            if ( lineNum < 0_lnum || lineNum >= totalLines ) {
                break;
            }

            // Advance the cached segment iterator if the line is beyond it.
            if ( cachedSegIt == segments_.cend()
                 || lineNum.get<qint64>() >= cachedSegIt->cumulativeEndLine ) {
                cachedSegIt = std::lower_bound(
                    segments_.cbegin(), segments_.cend(), lineNum.get(),
                    []( const Segment& segment, qint64 value ) {
                        return segment.cumulativeEndLine <= value;
                    } );
                if ( cachedSegIt == segments_.cend() ) {
                    break;
                }
                const auto segIdx
                    = static_cast<size_t>( std::distance( segments_.cbegin(), cachedSegIt ) );
                cachedPrevEnd = segIdx == 0 ? 0LL : segments_[ segIdx - 1 ].cumulativeEndLine;
            }

            const auto localLine
                = static_cast<int>( lineNum.get<qint64>() - cachedPrevEnd );

            if ( localLine < 0 || localLine >= klogg::isize( cachedSegIt->lineOffsets ) ) {
                break;
            }

            LineRef ref;
            ref.memoryData = cachedSegIt->memoryData; // shared_ptr copy keeps data alive
            ref.filePath = cachedSegIt->filePath;
            ref.offset = cachedSegIt->lineOffsets[ static_cast<size_t>( localLine ) ];
            ref.length = cachedSegIt->lineLengths[ static_cast<size_t>( localLine ) ];
            lineRefs.push_back( std::move( ref ) );
        }
    }
    // mutex released — main thread can now append data freely

    const auto effectiveCodec = codec ? codec : QTextCodec::codecForName( "UTF-8" );

    SearchableLogData::RawLines rawLines;
    rawLines.startLine = first;
    rawLines.textDecoder.decoder.reset( effectiveCodec->makeDecoder() );
    rawLines.textDecoder.encodingParams = EncodingParameters( effectiveCodec );
    rawLines.textDecoder.encodingParams.isUtf8Compatible = true;
    rawLines.textDecoder.encodingParams.lineFeedWidth = 1;
    rawLines.prefilterPattern = prefilterPattern;

    // Cache file handle for spilled segments to avoid repeated open/close
    // when consecutive lines come from the same segment file.
    QString cachedFilePath;
    std::unique_ptr<QFile> cachedFile;

    for ( const auto& ref : lineRefs ) {
        QByteArray utf8Line;
        if ( ref.memoryData ) {
            utf8Line = ref.memoryData->mid( type_safe::narrow_cast<int>( ref.offset ), ref.length );
        }
        else {
            if ( cachedFilePath != ref.filePath || !cachedFile ) {
                cachedFile = std::make_unique<QFile>( ref.filePath );
                if ( !cachedFile->open( QIODevice::ReadOnly ) ) {
                    cachedFile.reset();
                }
                cachedFilePath = ref.filePath;
            }
            if ( cachedFile && cachedFile->isOpen() ) {
                cachedFile->seek( ref.offset );
                utf8Line = cachedFile->read( ref.length );
            }
        }
        const auto lineStr = decodeUtf8Line( utf8Line, effectiveCodec, prefilterPattern );
        const auto lineUtf8 = lineStr.toUtf8();
        rawLines.buffer.insert( rawLines.buffer.end(), lineUtf8.begin(), lineUtf8.end() );
        rawLines.buffer.push_back( '\n' );
        rawLines.endOfLines.push_back( klogg::ssize( rawLines.buffer ) );
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

    appendOutputBytes( terminated ? lineBytes + '\n' : lineBytes );

    fileSize_ += lineBytes.size() + ( terminated ? 1 : 0 );
    memoryBytes_ += lineBytes.size() + ( terminated ? 1 : 0 );
    totalLines_ += 1;
    maxLineLength_ = qMax( maxLineLength_, static_cast<int>( lineBytes.size() ) );
    lastModified_ = QDateTime::currentDateTime();

    rebuildCumulativeLineCounts( true );
    rotateSegmentIfNeeded();
    enforceMemoryBudget();
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

    // Full rebuild — used after loadFromDisk(), clear(), etc.
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

bool CaptureStore::writeCaptureToDevice( QIODevice* device ) const
{
    for ( const auto& segment : segments_ ) {
        if ( !writeSegmentToDevice( segment, device ) ) {
            return false;
        }
    }

    return true;
}

void CaptureStore::appendOutputBytes( const QByteArray& bytes )
{
    if ( !boundOutputHandle_ ) {
        return;
    }

    if ( boundOutputHandle_->write( bytes ) != bytes.size() ) {
        LOG_WARNING << "Bound output file write failed, unbinding: " << boundOutputFile_;
        boundOutputHandle_.reset();
        boundOutputFile_.clear();
        return;
    }

    unflushedOutputBytes_ += bytes.size();
    unflushedOutputLines_++;
    flushOutputIfNeeded();
}

void CaptureStore::flushOutputIfNeeded()
{
    if ( !boundOutputHandle_ || unflushedOutputBytes_ == 0 ) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if ( unflushedOutputBytes_ >= OutputFlushBytesThreshold
         || unflushedOutputLines_ >= OutputFlushLinesThreshold
         || now - lastOutputFlushTime_ >= OutputFlushTimeThreshold ) {
        if ( !boundOutputHandle_->flush() ) {
            LOG_WARNING << "Bound output file flush failed, unbinding: " << boundOutputFile_;
            boundOutputHandle_.reset();
            boundOutputFile_.clear();
            return;
        }
        resetOutputFlushCounters();
    }
}

void CaptureStore::resetOutputFlushCounters()
{
    unflushedOutputBytes_ = 0;
    unflushedOutputLines_ = 0;
    lastOutputFlushTime_ = std::chrono::steady_clock::now();
}
