#include "streaminglogdata.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>

#include "logfiltereddata.h"

namespace {
constexpr qint64 OutputFlushBytesThreshold = 1024 * 1024;
constexpr LinesCount::UnderlyingType OutputFlushLinesThreshold = 1000;
constexpr qint64 CachedRawBatchBytesLimit = 256 * 1024 * 1024;
}

StreamingLogData::StreamingLogData( QString captureId, QString captureRoot )
    : SearchableLogData()
    , captureStore_( std::move( captureId ), std::move( captureRoot ) )
    , codec_( QTextCodec::codecForName( "UTF-8" ) )
{
    captureStore_.loadFromDisk();
    scheduleLoadingFinished();

    outputFlushTimer_.setInterval( 1000 );
    connect( &outputFlushTimer_, &QTimer::timeout, this, [this] {
        if ( boundOutputHandle_.isOpen() ) {
            boundOutputHandle_.flush();
        }
        if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Preserve ) {
            captureStore_.flush();
        }
    } );

}

StreamingLogData::~StreamingLogData()
{
    closeDisplayOutputFile();
}

void StreamingLogData::appendUtf8( const QByteArray& data )
{
#ifdef KLOGG_PERF_MEASURE_STREAMING
    const auto t0 = std::chrono::steady_clock::now();
#endif

    // Restart the flush timer if new data arrives while an output file is bound
    // but the timer is stopped (e.g. after finishInput from a reconnect cycle).
    if ( !outputFlushTimer_.isActive() && !boundOutputFile_.isEmpty() ) {
        startOutputFlushTimer();
    }

    const auto previousLineCount = captureStore_.lineCount();

#ifdef KLOGG_PERF_MEASURE_STREAMING
    const auto t1 = std::chrono::steady_clock::now();
#endif

    const auto appendResult = captureStore_.appendUtf8( data );

#ifdef KLOGG_PERF_MEASURE_STREAMING
    const auto t2 = std::chrono::steady_clock::now();
#endif

    rememberAppendedRawLines( appendResult );

#ifdef KLOGG_PERF_MEASURE_STREAMING
    const auto t3 = std::chrono::steady_clock::now();
#endif

    const auto currentLineCount = captureStore_.lineCount();
    if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Strip
         && currentLineCount != previousLineCount ) {
        writeDisplayLinesToOutput( LineNumber( previousLineCount.get() ),
                                   currentLineCount - previousLineCount );
    }
    if ( currentLineCount != previousLineCount ) {
        Q_EMIT fileChanged( MonitoredFileStatus::DataAdded );
        scheduleLoadingFinished();
    }

#ifdef KLOGG_PERF_MEASURE_STREAMING
    const auto t4 = std::chrono::steady_clock::now();
    const auto captureUs = std::chrono::duration_cast<std::chrono::microseconds>( t2 - t1 ).count();
    const auto cacheUs = std::chrono::duration_cast<std::chrono::microseconds>( t3 - t2 ).count();
    const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>( t4 - t0 ).count();
    LOG_INFO << "PERF [streaming] appendUtf8 size=" << data.size()
             << " lines=" << ( currentLineCount.get() - previousLineCount.get() )
             << " capture_us=" << captureUs
             << " cache_us=" << cacheUs
             << " total_us=" << totalUs;
#endif
}

void StreamingLogData::finishInput()
{
    stopOutputFlushTimer();
    const auto previousLineCount = captureStore_.lineCount();
    const auto appendResult = captureStore_.finishInput();
    rememberAppendedRawLines( appendResult );
    const auto currentLineCount = captureStore_.lineCount();
    if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Strip
         && currentLineCount != previousLineCount ) {
        writeDisplayLinesToOutput( LineNumber( previousLineCount.get() ),
                                   currentLineCount - previousLineCount );
    }
    if ( currentLineCount != previousLineCount ) {
        Q_EMIT fileChanged( MonitoredFileStatus::DataAdded );
        scheduleLoadingFinished();
    }
    if ( boundOutputHandle_.isOpen() ) {
        boundOutputHandle_.flush();
    }
}

void StreamingLogData::clearCapture()
{
    const auto timerWasActive = outputFlushTimer_.isActive();
    stopOutputFlushTimer();
    captureStore_.clear();
    {
        std::lock_guard<std::mutex> lock( cachedRawBatchesMutex_ );
        cachedRawBatches_.clear();
        cachedRawBytes_ = 0;
    }
    if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Strip && !boundOutputFile_.isEmpty() ) {
        openDisplayOutputFile( boundOutputFile_ );
    }

    // clear() internally rebinds the output file if one was bound.
    // Only restart the timer if it was running before the clear,
    // so a clearCapture after finishInput does not revive the timer.
    if ( timerWasActive && !boundOutputFile_.isEmpty() ) {
        startOutputFlushTimer();
    }

    Q_EMIT fileChanged( MonitoredFileStatus::Truncated );
    scheduleLoadingFinished();
}

bool StreamingLogData::bindOutputFile( const QString& outputPath )
{
    return bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip );
}

bool StreamingLogData::bindOutputFile( const QString& outputPath, LiveLogSaveAnsiMode ansiMode )
{
    stopOutputFlushTimer();
    outputSaveAnsiMode_ = ansiMode;

    if ( outputPath.isEmpty() ) {
        closeDisplayOutputFile();
        captureStore_.bindOutputFile( QString{} );
        return true;
    }

    bool result = false;
    if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Preserve ) {
        closeDisplayOutputFile();
        result = captureStore_.bindOutputFile( outputPath );
        boundOutputFile_ = result ? outputPath : QString{};
    }
    else {
        captureStore_.bindOutputFile( QString{} );
        result = openDisplayOutputFile( outputPath );
    }

    if ( !outputPath.isEmpty() && result ) {
        startOutputFlushTimer();
    }
    return result;
}

QString StreamingLogData::boundOutputFile() const
{
    return boundOutputFile_;
}

QString StreamingLogData::captureId() const
{
    return captureStore_.captureId();
}

QString StreamingLogData::capturePath() const
{
    return captureStore_.capturePath();
}

void StreamingLogData::deleteCaptureFiles()
{
    closeDisplayOutputFile();
    captureStore_.bindOutputFile( QString{} );
    captureStore_.deleteCaptureFiles();
}

void StreamingLogData::interruptLoading()
{
}

std::unique_ptr<LogFilteredData> StreamingLogData::getNewFilteredData() const
{
    return std::make_unique<LogFilteredData>( this );
}

qint64 StreamingLogData::getFileSize() const
{
    return captureStore_.stats().fileSize;
}

QDateTime StreamingLogData::getLastModifiedDate() const
{
    return captureStore_.stats().lastModified;
}

void StreamingLogData::reload( QTextCodec* forcedEncoding )
{
    if ( forcedEncoding ) {
        codec_.setCodec( forcedEncoding );
    }
    scheduleLoadingFinished();
}

QTextCodec* StreamingLogData::getDetectedEncoding() const
{
    return codec_.codec();
}

void StreamingLogData::setPrefilter( const QString& prefilterPattern )
{
    prefilterPattern_.setPattern( prefilterPattern );
}

void StreamingLogData::setAnsiProcessingMode( AnsiProcessingMode mode )
{
    ansiProcessingMode_ = mode;
}

SearchableLogData::RawLines StreamingLogData::getLinesRaw( LineNumber first, LinesCount number ) const
{
    const auto encodingParams = EncodingParameters( codec_.codec() );
    if ( encodingParams.isUtf8Compatible ) {
        if ( auto cachedRawLines = tryBuildCachedRawLines( first, number ) ) {
            cachedRawLines->prefilterPattern = prefilterPattern_;
            cachedRawLines->ansiProcessingMode = ansiProcessingMode_;
            return *std::move( cachedRawLines );
        }
    }

    auto rawLines = captureStore_.buildRawLines( first, number, codec_.codec(), prefilterPattern_ );
    rawLines.ansiProcessingMode = ansiProcessingMode_;
    return rawLines;
}

bool StreamingLogData::isLiveSource() const
{
    return true;
}

QString StreamingLogData::doGetLineString( LineNumber line ) const
{
    return processAnsiSequences( captureStore_.lineAt( line, codec_.codec(), prefilterPattern_ ),
                                 ansiProcessingMode_ )
        .text;
}

QString StreamingLogData::doGetExpandedLineString( LineNumber line ) const
{
    return doGetLineString( line );
}

klogg::vector<AnsiColorSpan> StreamingLogData::doGetLineAnsiColors( LineNumber line ) const
{
    return processAnsiSequences( captureStore_.lineAt( line, codec_.codec(), prefilterPattern_ ),
                                 ansiProcessingMode_ )
        .colorSpans;
}

klogg::vector<QString> StreamingLogData::doGetLines( LineNumber first, LinesCount number ) const
{
    return getLines( first, number );
}

klogg::vector<QString> StreamingLogData::doGetExpandedLines( LineNumber first,
                                                             LinesCount number ) const
{
    return getLines( first, number );
}

LineNumber StreamingLogData::doGetLineNumber( LineNumber index ) const
{
    return index;
}

LinesCount StreamingLogData::doGetNbLine() const
{
    return captureStore_.lineCount();
}

LineLength StreamingLogData::doGetMaxLength() const
{
    return captureStore_.maxLineLength();
}

LineLength StreamingLogData::doGetLineLength( LineNumber line ) const
{
    return captureStore_.lineLength( line );
}

void StreamingLogData::doSetDisplayEncoding( const char* encoding )
{
    codec_.setCodec( QTextCodec::codecForName( encoding ) );
}

QTextCodec* StreamingLogData::doGetDisplayEncoding() const
{
    return codec_.codec();
}

void StreamingLogData::doAttachReader() const
{
}

void StreamingLogData::doDetachReader() const
{
}

void StreamingLogData::scheduleLoadingFinished()
{
    if ( loadingFinishedQueued_ ) {
        return;
    }

    loadingFinishedQueued_ = true;
    QMetaObject::invokeMethod(
        this,
        [ this ] {
            loadingFinishedQueued_ = false;
            Q_EMIT loadingFinished( LoadingStatus::Successful );
        },
        Qt::QueuedConnection );
}

void StreamingLogData::startOutputFlushTimer()
{
    if ( !outputFlushTimer_.isActive() ) {
        outputFlushTimer_.start();
    }
}

void StreamingLogData::stopOutputFlushTimer()
{
    outputFlushTimer_.stop();
}

bool StreamingLogData::openDisplayOutputFile( const QString& outputPath )
{
    const auto outputPathCopy = outputPath;
    closeDisplayOutputFile();
    boundOutputFile_ = outputPathCopy;

    if ( boundOutputFile_.isEmpty() ) {
        return true;
    }

    QDir().mkpath( QFileInfo( boundOutputFile_ ).absolutePath() );
    boundOutputHandle_.setFileName( boundOutputFile_ );
    if ( !boundOutputHandle_.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        boundOutputFile_.clear();
        return false;
    }

    if ( !writeDisplayLinesToOutput( 0_lnum, captureStore_.lineCount() ) ) {
        closeDisplayOutputFile();
        return false;
    }

    boundOutputHandle_.flush();
    return true;
}

void StreamingLogData::closeDisplayOutputFile()
{
    if ( boundOutputHandle_.isOpen() ) {
        boundOutputHandle_.flush();
        boundOutputHandle_.close();
    }
    boundOutputFile_.clear();
}

bool StreamingLogData::writeDisplayLinesToOutput( LineNumber first, LinesCount count )
{
    if ( !boundOutputHandle_.isOpen() || count <= 0_lcount ) {
        return true;
    }

    qint64 unflushedBytes = 0;
    LinesCount::UnderlyingType unflushedLines = 0;
    const auto lines = getLines( first, count );
    for ( const auto& line : lines ) {
        const auto outputLine = processAnsiSequences( line, AnsiProcessingMode::Strip ).text.toUtf8();
        if ( boundOutputHandle_.write( outputLine ) != outputLine.size()
             || boundOutputHandle_.write( "\n", 1 ) != 1 ) {
            closeDisplayOutputFile();
            return false;
        }

        unflushedBytes += outputLine.size() + 1;
        ++unflushedLines;
        if ( unflushedBytes >= OutputFlushBytesThreshold
             || unflushedLines >= OutputFlushLinesThreshold ) {
            if ( !boundOutputHandle_.flush() ) {
                closeDisplayOutputFile();
                return false;
            }
            unflushedBytes = 0;
            unflushedLines = 0;
        }
    }

    return true;
}

klogg::vector<QString> StreamingLogData::getLines( LineNumber first, LinesCount number ) const
{
    klogg::vector<QString> lines;
    const auto lastLine = qMin( first.get() + number.get(), doGetNbLine().get() );
    lines.reserve( qMax<LinesCount::UnderlyingType>( 0, lastLine - first.get() ) );
    for ( auto line = first.get(); line < lastLine; ++line ) {
        lines.push_back( doGetLineString( LineNumber( line ) ) );
    }
    return lines;
}

void StreamingLogData::rememberAppendedRawLines( const CaptureStore::AppendResult& appendResult )
{
    if ( appendResult.lineCount <= 0_lcount || appendResult.rawUtf8Lines.isEmpty() ) {
        return;
    }

    CachedRawBatch batch;
    batch.firstLine = appendResult.firstLine;
    batch.lineCount = appendResult.lineCount;
    batch.rawUtf8Lines = appendResult.rawUtf8Lines;
    batch.endOfLines = appendResult.endOfLines;

    std::lock_guard<std::mutex> lock( cachedRawBatchesMutex_ );
    cachedRawBytes_ += batch.rawUtf8Lines.size();
    cachedRawBatches_.push_back( std::move( batch ) );

    while ( cachedRawBytes_ > CachedRawBatchBytesLimit && !cachedRawBatches_.empty() ) {
        cachedRawBytes_ -= cachedRawBatches_.front().rawUtf8Lines.size();
        cachedRawBatches_.pop_front();
    }
}

std::optional<SearchableLogData::RawLines>
StreamingLogData::tryBuildCachedRawLines( LineNumber first, LinesCount number ) const
{
    if ( number <= 0_lcount ) {
        return RawLines{};
    }

    RawLines rawLines;
    rawLines.startLine = first;
    auto* utf8Codec = QTextCodec::codecForName( "UTF-8" );
    rawLines.textDecoder.decoder.reset( utf8Codec->makeDecoder() );
    rawLines.textDecoder.encodingParams = EncodingParameters( utf8Codec );
    rawLines.textDecoder.encodingParams.isUtf8Compatible = true;
    rawLines.textDecoder.encodingParams.lineFeedWidth = 1;

    auto nextLine = first;
    const auto requestedEnd = first + number;

    std::lock_guard<std::mutex> lock( cachedRawBatchesMutex_ );
    for ( const auto& batch : cachedRawBatches_ ) {
        const auto batchEnd = batch.firstLine + batch.lineCount;
        if ( batchEnd <= nextLine ) {
            continue;
        }
        if ( batch.firstLine > nextLine ) {
            break;
        }

        const auto localStart = static_cast<size_t>( nextLine.get() - batch.firstLine.get() );
        const auto localEnd = static_cast<size_t>(
            qMin( batchEnd.get(), requestedEnd.get() ) - batch.firstLine.get() );
        if ( localStart >= localEnd || localEnd > batch.endOfLines.size() ) {
            break;
        }

        const auto byteStart = localStart == 0 ? 0 : batch.endOfLines[ localStart - 1 ];
        const auto byteEnd = batch.endOfLines[ localEnd - 1 ];
        const auto existingBytes = klogg::ssize( rawLines.buffer );
        rawLines.buffer.insert( rawLines.buffer.end(), batch.rawUtf8Lines.constData() + byteStart,
                                batch.rawUtf8Lines.constData() + byteEnd );
        for ( auto i = localStart; i < localEnd; ++i ) {
            rawLines.endOfLines.push_back( existingBytes + batch.endOfLines[ i ] - byteStart );
        }

        nextLine = LineNumber( batch.firstLine.get() + static_cast<LineNumber::UnderlyingType>( localEnd ) );
        if ( nextLine >= requestedEnd ) {
            return rawLines;
        }
    }

    return std::nullopt;
}
