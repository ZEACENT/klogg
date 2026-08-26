#include "streaminglogdata.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>

#include "logfiltereddata.h"

namespace {
constexpr qint64 CachedRawBatchBytesLimit = 256 * 1024 * 1024;
constexpr int LiveAppendRefreshIntervalMs = 33;
constexpr size_t AnsiDisplayCacheLineLimit = 4096;
}

StreamingLogData::StreamingLogData( QString captureId, QString captureRoot )
    : SearchableLogData()
    , captureStore_( std::move( captureId ), std::move( captureRoot ) )
    , codec_( QTextCodec::codecForName( "UTF-8" ) )
{
    captureStore_.loadFromDisk();
    loadingFinishedTimer_.setSingleShot( true );
    connect( &loadingFinishedTimer_, &QTimer::timeout, this, [ this ] {
        loadingFinishedQueued_ = false;
        Q_EMIT loadingFinished( LoadingStatus::Successful );
    } );
    scheduleLoadingFinished();

    outputFlushTimer_.setInterval( 1000 );
    connect( &outputFlushTimer_, &QTimer::timeout, this, [ this ] {
        if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Strip
             && rollingDisplayOutput_.isValid() && !rollingDisplayOutput_.flush() ) {
            closeDisplayOutputFile();
            reportCaptureOutputFailure(
                QStringLiteral( "The bound capture output could not be flushed." ) );
        }
        if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Preserve ) {
            captureStore_.flush();
            checkPreservedOutputState();
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
    if ( !outputFlushTimer_.isActive() && !boundOutputFile().isEmpty() ) {
        startOutputFlushTimer();
    }

    const auto previousLineCount = captureStore_.lineCount();

#ifdef KLOGG_PERF_MEASURE_STREAMING
    const auto t1 = std::chrono::steady_clock::now();
#endif

    const auto appendResult = captureStore_.appendUtf8( data );
    checkPreservedOutputState();

#ifdef KLOGG_PERF_MEASURE_STREAMING
    const auto t2 = std::chrono::steady_clock::now();
#endif

    // consumeTrimResult() clears line-keyed caches when CaptureStore trimmed
    // during the append (oldest segments removed -> absolute line numbers shift).
    const auto trimResult = consumeTrimResult();
    const bool wasTrimmed = trimResult.trimmedLines > 0_lcount;

    // Cache the appended batch unless trimming removed some of its own lines
    // (a burst larger than the whole window): then the batch no longer lines up
    // with the surviving tail and serving it would return stale data.
    const auto preAppendTotal = static_cast<qint64>( previousLineCount.get() );
    if ( !wasTrimmed
         || static_cast<qint64>( trimResult.trimmedLines.get() ) <= preAppendTotal ) {
        rememberAppendedRawLines( appendResult );
    }

#ifdef KLOGG_PERF_MEASURE_STREAMING
    const auto t3 = std::chrono::steady_clock::now();
#endif

    const auto currentLineCount = captureStore_.lineCount();
    // Write the freshly appended lines to the Strip-mode display file.  This
    // MUST go by appended count / tail position (see writeAppendedDisplayLines),
    // NOT by a [previousLineCount, currentLineCount) delta: trimming can remove
    // more lines than were appended, making currentLineCount < previousLineCount
    // and underflowing the uint64 range -> std::length_error -> SIGABRT.
    writeAppendedDisplayLines( appendResult );
    if ( wasTrimmed ) {
        Q_EMIT fileChanged( MonitoredFileStatus::Truncated );
    }
    if ( currentLineCount != previousLineCount ) {
        Q_EMIT fileChanged( MonitoredFileStatus::DataAdded );
        scheduleLoadingFinished( LiveAppendRefreshIntervalMs );
    }

#ifdef KLOGG_PERF_MEASURE_STREAMING
    const auto t4 = std::chrono::steady_clock::now();
    const auto captureUs = std::chrono::duration_cast<std::chrono::microseconds>( t2 - t1 ).count();
    const auto cacheUs = std::chrono::duration_cast<std::chrono::microseconds>( t3 - t2 ).count();
    const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>( t4 - t0 ).count();
    LOG_INFO << "PERF [streaming] appendUtf8 size=" << data.size()
             << " lines=" << appendResult.lineCount.get()
             << " trimmed=" << trimResult.trimmedLines.get()
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
    checkPreservedOutputState();

    // finishInput() can rotate+trim the Preserve-mode output via the single-line
    // commit path (commitLine -> appendOutputBytes). Handle it exactly like
    // appendUtf8() so caches stay consistent and Truncated fires — previously
    // finishInput() skipped this entirely.
    const auto trimResult = consumeTrimResult();
    const bool wasTrimmed = trimResult.trimmedLines > 0_lcount;
    const auto preAppendTotal = static_cast<qint64>( previousLineCount.get() );
    if ( !wasTrimmed
         || static_cast<qint64>( trimResult.trimmedLines.get() ) <= preAppendTotal ) {
        rememberAppendedRawLines( appendResult );
    }

    // Same tail-position write as appendUtf8() — never an underflowing delta.
    writeAppendedDisplayLines( appendResult );
    if ( wasTrimmed ) {
        Q_EMIT fileChanged( MonitoredFileStatus::Truncated );
    }
    const auto currentLineCount = captureStore_.lineCount();
    if ( currentLineCount != previousLineCount ) {
        Q_EMIT fileChanged( MonitoredFileStatus::DataAdded );
        scheduleLoadingFinished();
    }
    if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Strip
         && rollingDisplayOutput_.isValid() && !rollingDisplayOutput_.flush() ) {
        closeDisplayOutputFile();
        reportCaptureOutputFailure( QStringLiteral( "The bound capture output could not be flushed." ) );
    }
}

CaptureStore::TrimResult StreamingLogData::consumeTrimResult()
{
    const auto trimResult = captureStore_.lastTrimResult();
    if ( trimResult.trimmedLines <= 0_lcount ) {
        return trimResult;
    }
    captureStore_.clearTrimResult();
    // Trimming removes oldest segments, shifting every absolute line number
    // down; both caches are keyed by absolute line number, so drop them all.
    {
        std::lock_guard<std::mutex> lock( cachedRawBatchesMutex_ );
        cachedRawBatches_.clear();
        cachedRawBytes_ = 0;
    }
    clearAnsiDisplayCache();
    return trimResult;
}

void StreamingLogData::clearCapture()
{
    const auto timerWasActive = outputFlushTimer_.isActive();
    stopOutputFlushTimer();
    captureStore_.clear();
    clearAnsiDisplayCache();
    {
        std::lock_guard<std::mutex> lock( cachedRawBatchesMutex_ );
        cachedRawBatches_.clear();
        cachedRawBytes_ = 0;
    }
    const auto boundPath = boundOutputFile();
    if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Strip && !boundPath.isEmpty() ) {
        openDisplayOutputFile( boundPath );
    }

    // clear() internally rebinds the output file if one was bound.
    // Only restart the timer if it was running before the clear,
    // so a clearCapture after finishInput does not revive the timer.
    if ( timerWasActive && !boundOutputFile().isEmpty() ) {
        startOutputFlushTimer();
    }

    Q_EMIT fileChanged( MonitoredFileStatus::Truncated );
    scheduleLoadingFinished();
}

void StreamingLogData::setCaptureLimits( CaptureStore::Limits limits )
{
    rollingMaxFileSize_ = limits.rollingMaxFileSize;
    rollingBackupCount_ = limits.rollingBackupCount;
    captureStore_.setLimits( std::move( limits ) );
}

bool StreamingLogData::bindOutputFile( const QString& outputPath )
{
    return bindOutputFile( outputPath, LiveLogSaveAnsiMode::Strip, OutputBindMode::FreshSave );
}

bool StreamingLogData::bindOutputFile( const QString& outputPath, LiveLogSaveAnsiMode ansiMode )
{
    return bindOutputFile( outputPath, ansiMode, OutputBindMode::FreshSave );
}

bool StreamingLogData::bindOutputFile( const QString& outputPath, LiveLogSaveAnsiMode ansiMode,
                                       OutputBindMode mode )
{
    stopOutputFlushTimer();
    outputSaveAnsiMode_ = ansiMode;

    if ( outputPath.isEmpty() ) {
        closeDisplayOutputFile();
        captureStore_.bindOutputFile( QString{} );
        reportCaptureOutputHealthy();
        return true;
    }

    // FreshSave truncates the destination and replays the current capture
    // (user-initiated "Save As" — overwrite was already confirmed by the
    // dialog). Restore opens the destination append-only so content already
    // streamed to the file survives a restart even when the volatile capture
    // (OS temp dir) has been cleared. If the destination is gone (moved or
    // deleted while klogg was closed) but the capture still reloaded, fall back
    // to the fresh replay path so the rebuilt file carries the restored history
    // instead of starting empty.
    const bool preserveExisting
        = ( mode == OutputBindMode::Restore ) && QFileInfo::exists( outputPath );

    bool result = false;
    if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Preserve ) {
        closeDisplayOutputFile();
        result = captureStore_.bindOutputFile( outputPath, preserveExisting );
        {
            const std::lock_guard<std::mutex> lock( boundOutputFileMutex_ );
            boundOutputFile_ = result ? outputPath : QString{};
        }
    }
    else {
        captureStore_.bindOutputFile( QString{} );
        result = openDisplayOutputFile( outputPath, preserveExisting );
    }

    if ( !outputPath.isEmpty() && result ) {
        reportCaptureOutputHealthy();
        startOutputFlushTimer();
    }
    return result;
}

QString StreamingLogData::boundOutputFile() const
{
    const std::lock_guard<std::mutex> lock( boundOutputFileMutex_ );
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
    // A bound output file is what the tab's path points at and what a
    // single-file open of the same path would index: report its true on-disk
    // size so both views agree. The capture store's byte counter is a
    // rolling-window watermark — trimming subtracts from it while the bound
    // file keeps every byte, and a Restore binding carries pre-restart
    // content the (volatile) capture never saw.
    const auto boundPath = boundOutputFile();
    if ( !boundPath.isEmpty() ) {
        QFileInfo boundInfo( boundPath );
        if ( boundInfo.exists() ) {
            return boundInfo.size();
        }
    }
    // No file bound (pure in-memory capture) or the bound file is gone:
    // the retained capture bytes are the only meaningful size.
    return captureStore_.stats().fileSize;
}

QDateTime StreamingLogData::getLastModifiedDate() const
{
    // Symmetric with getFileSize(): the bound output file's on-disk mtime is
    // what a single-file open of the same path reports, so both tabs show the
    // same "modified on" timestamp.
    const auto boundPath = boundOutputFile();
    if ( !boundPath.isEmpty() ) {
        QFileInfo boundInfo( boundPath );
        if ( boundInfo.exists() ) {
            return boundInfo.lastModified();
        }
    }
    return captureStore_.stats().lastModified;
}

void StreamingLogData::reload( QTextCodec* forcedEncoding )
{
    if ( forcedEncoding ) {
        codec_.setCodec( forcedEncoding );
        clearAnsiDisplayCache();
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
    clearAnsiDisplayCache();
}

void StreamingLogData::setAnsiProcessingMode( AnsiProcessingMode mode )
{
    if ( ansiProcessingMode_ == mode ) {
        return;
    }
    ansiProcessingMode_ = mode;
    clearAnsiDisplayCache();
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
    return processedAnsiLine( line ).text;
}

QString StreamingLogData::doGetExpandedLineString( LineNumber line ) const
{
    return doGetLineString( line );
}

klogg::vector<AnsiColorSpan> StreamingLogData::doGetLineAnsiColors( LineNumber line ) const
{
    return processedAnsiLine( line ).colorSpans;
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
    clearAnsiDisplayCache();
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

void StreamingLogData::scheduleLoadingFinished( int delayMs )
{
    if ( loadingFinishedQueued_ ) {
        if ( delayMs > 0 && loadingFinishedTimer_.isActive()
             && loadingFinishedTimer_.remainingTime() > delayMs ) {
            loadingFinishedTimer_.start( delayMs );
        }
        return;
    }

    loadingFinishedQueued_ = true;
    loadingFinishedTimer_.start( delayMs );
}

ProcessedAnsiLine StreamingLogData::processedAnsiLine( LineNumber line ) const
{
    if ( ansiProcessingMode_ == AnsiProcessingMode::Render ) {
        const auto key = line.get();
        {
            std::lock_guard<std::mutex> lock( ansiDisplayCacheMutex_ );
            const auto cached = ansiDisplayCache_.find( key );
            if ( cached != ansiDisplayCache_.end() ) {
                return cached->second;
            }
        }

        auto processed = processAnsiSequences(
            captureStore_.lineAt( line, codec_.codec(), prefilterPattern_ ), ansiProcessingMode_ );

        std::lock_guard<std::mutex> lock( ansiDisplayCacheMutex_ );
        if ( ansiDisplayCache_.find( key ) == ansiDisplayCache_.end() ) {
            ansiDisplayCacheOrder_.push_back( key );
        }
        ansiDisplayCache_[ key ] = processed;
        while ( ansiDisplayCacheOrder_.size() > AnsiDisplayCacheLineLimit ) {
            ansiDisplayCache_.erase( ansiDisplayCacheOrder_.front() );
            ansiDisplayCacheOrder_.pop_front();
        }
        return processed;
    }

    return processAnsiSequences( captureStore_.lineAt( line, codec_.codec(), prefilterPattern_ ),
                                 ansiProcessingMode_ );
}

void StreamingLogData::clearAnsiDisplayCache()
{
    std::lock_guard<std::mutex> lock( ansiDisplayCacheMutex_ );
    ansiDisplayCache_.clear();
    ansiDisplayCacheOrder_.clear();
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

bool StreamingLogData::openDisplayOutputFile( const QString& outputPath, bool preserveExisting )
{
    const auto outputPathCopy = outputPath;
    closeDisplayOutputFile();
    {
        const std::lock_guard<std::mutex> lock( boundOutputFileMutex_ );
        boundOutputFile_ = outputPathCopy;
    }

    if ( outputPathCopy.isEmpty() ) {
        return true;
    }

    QDir().mkpath( QFileInfo( outputPathCopy ).absolutePath() );

    // Use CaptureStore's rolling settings for the display file
    rollingDisplayOutput_ = RollingFileManager( outputPathCopy, rollingMaxFileSize_,
                                                 rollingBackupCount_ );
    // FreshSave truncates and dumps the current capture; Restore opens
    // append-only so existing on-disk content is preserved.
    if ( !rollingDisplayOutput_.open( /*truncate=*/!preserveExisting ) ) {
        const std::lock_guard<std::mutex> lock( boundOutputFileMutex_ );
        boundOutputFile_.clear();
        return false;
    }

    // Replay only into a file this open actually created. Gating on
    // openedNewFile() (rather than the pre-open preserveExisting flag) closes
    // the TOCTOU window: a Restore open that found the file already missing
    // created a new empty file, which must be seeded from the capture.
    if ( rollingDisplayOutput_.openedNewFile()
         && !writeDisplayLinesToOutput( 0_lnum, captureStore_.lineCount() ) ) {
        closeDisplayOutputFile();
        return false;
    }

    if ( !rollingDisplayOutput_.flush() ) {
        closeDisplayOutputFile();
        return false;
    }
    return true;
}

void StreamingLogData::closeDisplayOutputFile()
{
    rollingDisplayOutput_.close();
    rollingDisplayOutput_ = RollingFileManager();
    const std::lock_guard<std::mutex> lock( boundOutputFileMutex_ );
    boundOutputFile_.clear();
}

bool StreamingLogData::writeDisplayLinesToOutput( LineNumber first, LinesCount count )
{
    if ( !rollingDisplayOutput_.isValid() || count <= 0_lcount ) {
        return true;
    }

    const auto lines = getLines( first, count );
    for ( const auto& line : lines ) {
        auto outputLine = processAnsiSequences( line, AnsiProcessingMode::Strip ).text.toUtf8();
        outputLine.append( '\n' );

        const auto sizeBefore = rollingDisplayOutput_.currentFileSize();
        const auto written = rollingDisplayOutput_.write( outputLine );
        if ( written <= 0 ) {
            closeDisplayOutputFile();
            reportCaptureOutputFailure(
                QStringLiteral( "The bound capture output could not be written." ) );
            return false;
        }

        // If rotation happened, the display file is now a rolling window.
        // No need to trim CaptureStore here — it handles its own trimming
        // when the Preserve mode rolling file rotates.
        Q_UNUSED( sizeBefore );
    }

    if ( !rollingDisplayOutput_.flush() ) {
        closeDisplayOutputFile();
        reportCaptureOutputFailure( QStringLiteral( "The bound capture output could not be flushed." ) );
        return false;
    }
    return true;
}

void StreamingLogData::reportCaptureOutputHealthy()
{
    if ( !captureOutputDegraded_ ) {
        return;
    }
    captureOutputDegraded_ = false;
    Q_EMIT captureOutputChanged( true, QString{} );
}

void StreamingLogData::reportCaptureOutputFailure( const QString& detail )
{
    if ( captureOutputDegraded_ ) {
        return;
    }
    captureOutputDegraded_ = true;
    Q_EMIT captureOutputChanged( false, detail );
}

void StreamingLogData::checkPreservedOutputState()
{
    if ( outputSaveAnsiMode_ != LiveLogSaveAnsiMode::Preserve
         || boundOutputFile().isEmpty() || !captureStore_.boundOutputFile().isEmpty() ) {
        return;
    }
    reportCaptureOutputFailure( QStringLiteral( "The bound capture output could not be written." ) );
}

void StreamingLogData::writeAppendedDisplayLines( const CaptureStore::AppendResult& appendResult )
{
    if ( outputSaveAnsiMode_ != LiveLogSaveAnsiMode::Strip ) {
        return;
    }

    const auto appended = appendResult.lineCount.get();
    if ( appended == 0 ) {
        return;
    }

    // CaptureStore::trimToLimits() removes oldest segments from the FRONT, so
    // the freshly appended lines always live at the TAIL, at
    // [totalLines - appended, totalLines).  Addressing them there is correct
    // whether or not trimming occurred, and cannot underflow.
    //
    // The previous code used writeDisplayLinesToOutput(previousLineCount,
    // currentLineCount - previousLineCount): when trimming removed more lines
    // than were appended, currentLineCount < previousLineCount, the uint64
    // subtraction wrapped to ~2^64, and getLines() reserve() threw
    // std::length_error("vector") -> uncaught on the macOS main event loop ->
    // SIGABRT (objc_exception_rethrow -> -[NSApplication run]).
    const auto totalLines = captureStore_.lineCount().get();
    const auto safeAppended = std::min( appended, totalLines );
    const auto firstLine = totalLines - safeAppended;
    writeDisplayLinesToOutput( LineNumber( firstLine ), LinesCount( safeAppended ) );
}

klogg::vector<QString> StreamingLogData::getLines( LineNumber first, LinesCount number ) const
{
    // Clamp to the valid [0, nbLine) range.  A caller may pass a first/count
    // derived from line counts that shifted (e.g. after trimming); never throw
    // std::length_error from reserve() on an out-of-range or inverted request —
    // returning the available subset is the correct, crash-free behavior.
    const auto totalLines = doGetNbLine().get();
    const auto begin = std::min( first.get(), totalLines );
    const auto count = std::min( number.get(), totalLines - begin );

    klogg::vector<QString> lines;
    lines.reserve( static_cast<size_t>( count ) );
    for ( auto line = begin; line < begin + count; ++line ) {
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
