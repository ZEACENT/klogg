#include "streaminglogdata.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QSaveFile>

#include "logfiltereddata.h"
#include "stagedoutputfile.h"

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
            closeDisplayOutputFile( false );
            reportCaptureOutputFailure( CaptureOutputError::Flush );
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

std::optional<StreamingLogData::OutputExportCandidate>
StreamingLogData::beginOutputExport( LiveLogSaveAnsiMode ansiMode, qint64 maximumTailBytes )
{
    std::lock_guard<std::recursive_mutex> lock( appendOrderingMutex_ );
    if ( pendingOutputExport_.has_value() || maximumTailBytes <= 0 ) {
        return std::nullopt;
    }

    if ( nextOutputExportId_ == std::numeric_limits<std::uint64_t>::max() ) {
        nextOutputExportId_ = 0;
    }
    PendingOutputExport pending;
    pending.id = ++nextOutputExportId_;
    pending.firstTailSequence = nextOutputDeliverySequence_ + 1u;
    pending.maximumTailBytes = maximumTailBytes;
    pending.snapshotFinalRecordUnterminated
        = captureStore_.finalRecordUnterminated();
    pending.ansiMode = ansiMode;
    pending.codecName = codec_.codec()->name();
    pending.prefilterPattern = prefilterPattern_.pattern();
    pendingOutputExport_ = pending;

    OutputExportCandidate candidate;
    candidate.id = pending.id;
    candidate.firstTailSequence = pending.firstTailSequence;
    candidate.snapshot = captureStore_.snapshot();
    candidate.snapshotFinalRecordUnterminated
        = pending.snapshotFinalRecordUnterminated;
    candidate.ansiMode = pending.ansiMode;
    candidate.codecName = pending.codecName;
    candidate.prefilterPattern = pending.prefilterPattern;
    return candidate;
}

StreamingLogData::OutputExportTail
StreamingLogData::takeOutputExportTail( std::uint64_t candidateId )
{
    std::lock_guard<std::recursive_mutex> lock( appendOrderingMutex_ );
    OutputExportTail result;
    if ( !pendingOutputExport_.has_value() || pendingOutputExport_->id != candidateId ) {
        result.failure = OutputExportFailure::Cancelled;
        return result;
    }
    if ( pendingOutputExport_->failure.has_value() ) {
        result.failure = pendingOutputExport_->failure;
        return result;
    }
    result.batches.reserve( pendingOutputExport_->tail.size() );
    while ( !pendingOutputExport_->tail.empty() ) {
        result.batches.push_back( std::move( pendingOutputExport_->tail.front() ) );
        pendingOutputExport_->tail.pop_front();
    }
    pendingOutputExport_->tailBytes = 0;
    return result;
}

void StreamingLogData::cancelOutputExport( std::uint64_t candidateId )
{
    std::lock_guard<std::recursive_mutex> lock( appendOrderingMutex_ );
    if ( pendingOutputExport_.has_value() && pendingOutputExport_->id == candidateId ) {
        pendingOutputExport_.reset();
    }
}

StreamingLogData::OutputExportActivation StreamingLogData::publishStagedOutputExport(
    std::uint64_t candidateId, const QString& outputPath,
    OutputExportEncodingState encodingState, QSaveFile& stagedOutput,
    const std::function<void()>& afterPublish )
{
    std::lock_guard<std::recursive_mutex> lock( appendOrderingMutex_ );
    if ( !pendingOutputExport_.has_value() || pendingOutputExport_->id != candidateId ) {
        stagedOutput.cancelWriting();
        return { false, OutputExportFailure::Cancelled };
    }
    if ( pendingOutputExport_->failure.has_value() ) {
        const auto failure = pendingOutputExport_->failure;
        pendingOutputExport_.reset();
        stagedOutput.cancelWriting();
        return { false, failure };
    }

    OutputExportCandidate candidate;
    candidate.id = pendingOutputExport_->id;
    candidate.firstTailSequence = pendingOutputExport_->firstTailSequence;
    candidate.snapshotFinalRecordUnterminated
        = pendingOutputExport_->snapshotFinalRecordUnterminated;
    candidate.ansiMode = pendingOutputExport_->ansiMode;
    candidate.codecName = pendingOutputExport_->codecName;
    candidate.prefilterPattern = pendingOutputExport_->prefilterPattern;
    std::vector<OutputExportBatch> tail;
    try {
        tail.reserve( pendingOutputExport_->tail.size() );
        while ( !pendingOutputExport_->tail.empty() ) {
            tail.push_back( std::move( pendingOutputExport_->tail.front() ) );
            pendingOutputExport_->tail.pop_front();
        }
    }
    catch ( ... ) {
        pendingOutputExport_.reset();
        stagedOutput.cancelWriting();
        return { false, OutputExportFailure::TailOverflow };
    }

    std::optional<klogg::platform::FileIdentity> publishedIdentity;
    std::optional<SuspendedOutputBinding> suspendedOutput;
    bool published = false;
    RollingFileManager candidateOutput( outputPath, rollingMaxFileSize_, rollingBackupCount_ );
    try {
        const auto stagedWrite = [ &stagedOutput ]( const QByteArray& bytes ) {
            return stagedOutput.write( bytes );
        };
        if ( !writeOutputExportBatches( candidate, tail, encodingState, stagedWrite ) ) {
            pendingOutputExport_.reset();
            stagedOutput.cancelWriting();
            return { false, OutputExportFailure::Write };
        }

        publishedIdentity = klogg::platform::fileIdentity( stagedOutput );
        if ( !publishedIdentity.has_value() ) {
            pendingOutputExport_.reset();
            stagedOutput.cancelWriting();
            return { false, OutputExportFailure::Publish };
        }
        if ( !suspendOutputForReplacement( outputPath, suspendedOutput ) ) {
            pendingOutputExport_.reset();
            stagedOutput.cancelWriting();
            return { false, OutputExportFailure::Publish };
        }
        if ( !stagedOutput.commit() ) {
            restoreOutputAfterFailedReplacement( suspendedOutput );
            pendingOutputExport_.reset();
            return { false, OutputExportFailure::Publish };
        }
        published = true;
        if ( !candidateOutput.openExisting( publishedIdentity ) ) {
            abandonOutputAfterFailedReplacement(
                suspendedOutput, CaptureStore::OutputFailure::Open );
            pendingOutputExport_.reset();
            return { false, OutputExportFailure::PublishedReopen };
        }
    }
    catch ( ... ) {
        if ( published ) {
            abandonOutputAfterFailedReplacement(
                suspendedOutput, CaptureStore::OutputFailure::Open );
        }
        else {
            restoreOutputAfterFailedReplacement( suspendedOutput );
            stagedOutput.cancelWriting();
        }
        pendingOutputExport_.reset();
        return { false, published ? OutputExportFailure::PublishedReopen
                                  : OutputExportFailure::Write };
    }

    if ( afterPublish ) {
        try {
            afterPublish();
        }
        catch ( const std::exception& error ) {
            LOG_ERROR << "Live export post-publication test hook failed: " << error.what();
        }
        catch ( ... ) {
            LOG_ERROR << "Live export post-publication test hook failed with unknown exception";
        }
    }

    // Keep the verified publication handle open through cutover. On platforms
    // that permit unlinking an open file this also prevents inode reuse from
    // making a same-name replacement compare equal to the published identity.
    if ( !candidateOutput.refersToPath( outputPath ) ) {
        abandonOutputAfterFailedReplacement(
            suspendedOutput, CaptureStore::OutputFailure::Open );
        pendingOutputExport_.reset();
        return { false, OutputExportFailure::PublishedReopen };
    }

    if ( candidate.ansiMode == LiveLogSaveAnsiMode::Strip ) {
        if ( !candidateOutput.flush() ) {
            abandonOutputAfterFailedReplacement(
                suspendedOutput, CaptureStore::OutputFailure::Flush );
            pendingOutputExport_.reset();
            return { false, OutputExportFailure::PublishedCutover };
        }
        rollingDisplayOutput_ = std::move( candidateOutput );
        captureStore_.bindOutputFile( QString{} );
    }
    else {
        if ( !captureStore_.adoptPublishedOutputFile(
                 std::move( candidateOutput ), outputPath, encodingState.needsSeparator ) ) {
            abandonOutputAfterFailedReplacement(
                suspendedOutput, CaptureStore::OutputFailure::Open );
            pendingOutputExport_.reset();
            return { false, OutputExportFailure::PublishedCutover };
        }
        closeDisplayOutputFile( false );
    }

    outputSaveAnsiMode_ = candidate.ansiMode;
    displayOutputNeedsSeparator_ = encodingState.needsSeparator;
    {
        const std::lock_guard<std::mutex> pathLock( boundOutputFileMutex_ );
        boundOutputFile_ = outputPath;
    }
    pendingOutputExport_.reset();
    reportCaptureOutputHealthy();
    startOutputFlushTimer();
    return { true, std::nullopt };
}

bool StreamingLogData::writeAllOutputBytes( const QByteArray& bytes,
                                            const OutputExportWrite& write )
{
    qint64 offset = 0;
    while ( offset < bytes.size() ) {
        const auto written = write( bytes.mid( static_cast<int>( offset ) ) );
        if ( written <= 0 || written > bytes.size() - offset ) {
            return false;
        }
        offset += written;
    }
    return true;
}

QByteArray StreamingLogData::transformOutputRecord( const OutputExportCandidate& candidate,
                                                    const QByteArray& bytes, bool terminated )
{
    QByteArray output;
    if ( candidate.ansiMode == LiveLogSaveAnsiMode::Preserve ) {
        output = bytes;
    }
    else {
        auto* codec = QTextCodec::codecForName( candidate.codecName );
        if ( codec == nullptr ) {
            codec = QTextCodec::codecForName( "UTF-8" );
        }
        auto line = codec->toUnicode( bytes );
        if ( !candidate.prefilterPattern.isEmpty() ) {
            line.remove( QRegularExpression( candidate.prefilterPattern ) );
        }
        output = processAnsiSequences( line, AnsiProcessingMode::Strip ).text.toUtf8();
    }
    if ( terminated ) {
        output.append( '\n' );
    }
    return output;
}

bool StreamingLogData::writeOutputExportSnapshot( const OutputExportCandidate& candidate,
                                                  OutputExportEncodingState& state,
                                                  const OutputExportWrite& write,
                                                  const OutputExportCancelled& cancelled )
{
    CaptureStore::Snapshot::Cursor cursor;
    if ( candidate.ansiMode == LiveLogSaveAnsiMode::Preserve ) {
        bool wroteAny = false;
        while ( true ) {
            if ( cancelled && cancelled() ) {
                return false;
            }
            const auto chunk = candidate.snapshot.readChunk( cursor, 64 * 1024 );
            if ( chunk.readFailed || !writeAllOutputBytes( chunk.bytes, write ) ) {
                return false;
            }
            if ( !chunk.bytes.isEmpty() ) {
                wroteAny = true;
                state.needsSeparator = chunk.bytes.back() != '\n';
            }
            if ( chunk.complete ) {
                state.needsSeparator
                    = candidate.snapshotFinalRecordUnterminated
                      || ( wroteAny && state.needsSeparator );
                return true;
            }
        }
    }

    while ( true ) {
        if ( cancelled && cancelled() ) {
            return false;
        }
        const auto chunk = candidate.snapshot.readChunk( cursor, 64 * 1024 );
        if ( chunk.readFailed ) {
            return false;
        }
        klogg::ContainerIndex start = 0;
        while ( start < chunk.bytes.size() ) {
            const auto end = chunk.bytes.indexOf( '\n', start );
            if ( end < 0 ) {
                state.partialRecord.append( chunk.bytes.constData() + start,
                                            type_safe::narrow_cast<int>( chunk.bytes.size() - start ) );
                break;
            }
            state.partialRecord.append( chunk.bytes.constData() + start,
                                        type_safe::narrow_cast<int>( end - start ) );
            auto record = transformOutputRecord( candidate, state.partialRecord, true );
            if ( state.needsSeparator ) {
                record.prepend( '\n' );
            }
            if ( !writeAllOutputBytes( record, write ) ) {
                return false;
            }
            state.partialRecord.clear();
            state.needsSeparator = false;
            start = end + 1;
        }
        if ( chunk.complete ) {
            const auto hadUnterminatedBytes = !state.partialRecord.isEmpty();
            if ( hadUnterminatedBytes ) {
                auto record = transformOutputRecord( candidate, state.partialRecord, false );
                if ( state.needsSeparator ) {
                    record.prepend( '\n' );
                }
                if ( !writeAllOutputBytes( record, write ) ) {
                    return false;
                }
                state.partialRecord.clear();
            }
            state.needsSeparator
                = candidate.snapshotFinalRecordUnterminated
                  || hadUnterminatedBytes;
            return true;
        }
    }
}

bool StreamingLogData::writeOutputExportBatches( const OutputExportCandidate& candidate,
                                                 const std::vector<OutputExportBatch>& batches,
                                                 OutputExportEncodingState& state,
                                                 const OutputExportWrite& write )
{
    if ( !state.nextTailSequence.has_value() ) {
        state.nextTailSequence = candidate.firstTailSequence;
    }
    for ( const auto& batch : batches ) {
        if ( batch.sequence != *state.nextTailSequence ) {
            return false;
        }
        qint64 start = 0;
        for ( const auto end : batch.endOfLines ) {
            const auto unterminated
                = batch.finalRecordUnterminated && end == batch.endOfLines.back();
            auto record = transformOutputRecord(
                candidate,
                batch.rawUtf8Lines.mid( static_cast<int>( start ),
                                       static_cast<int>( end - start - 1 ) ),
                !unterminated );
            if ( state.needsSeparator ) {
                record.prepend( '\n' );
            }
            if ( !writeAllOutputBytes( record, write ) ) {
                return false;
            }
            state.needsSeparator = unterminated;
            start = end;
        }
        state.nextTailSequence = batch.sequence + 1u;
    }
    return true;
}

bool StreamingLogData::hasPendingOutputExport() const
{
    std::lock_guard<std::recursive_mutex> lock( appendOrderingMutex_ );
    return pendingOutputExport_.has_value();
}

CaptureStore::AppendResult StreamingLogData::appendUtf8( const QByteArray& data )
{
    std::lock_guard<std::recursive_mutex> orderingLock( appendOrderingMutex_ );
#ifdef KLOGG_PERF_MEASURE_STREAMING
    const auto t0 = std::chrono::steady_clock::now();
#endif

    // Restart the flush timer if new data arrives while an output file is bound
    // but the timer is stopped (e.g. after finishInput from a reconnect cycle).
    if ( !outputFlushTimer_.isActive() && isOutputFileActive() ) {
        startOutputFlushTimer();
    }

    const auto previousLineCount = captureStore_.lineCount();

#ifdef KLOGG_PERF_MEASURE_STREAMING
    const auto t1 = std::chrono::steady_clock::now();
#endif

    auto appendResult = captureStore_.appendUtf8( data );
    journalOutputExport( appendResult );
    try {
        // Output consumes the accepted batch before any cache allocation or observer.
        writeAppendedDisplayLines( appendResult );
        if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Strip && appendResult.outputFailure ) {
            reportCaptureOutputFailure( captureStoreOutputError( appendResult.outputFailure ) );
        }
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
    if ( wasTrimmed ) {
        Q_EMIT fileChanged( MonitoredFileStatus::Truncated );
    }
    if ( currentLineCount != previousLineCount ) {
        Q_EMIT fileChanged( MonitoredFileStatus::DataAdded );
    }
    // A rolling replacement is dirty even when its retained count is unchanged.
    if ( appendResult.lineCount > 0_lcount || wasTrimmed ) {
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
    reportPersistenceState( appendResult.persistence );
    } catch ( const std::exception& error ) {
        LOG_ERROR << "Streaming observer failed after capture commit: " << error.what();
        appendResult.notificationFailed = true;
    } catch ( ... ) {
        LOG_ERROR << "Streaming observer failed after capture commit with unknown exception";
        appendResult.notificationFailed = true;
    }
    return appendResult;
}

CaptureStore::AppendResult StreamingLogData::finishInput()
{
    std::lock_guard<std::recursive_mutex> orderingLock( appendOrderingMutex_ );
    stopOutputFlushTimer();
    const auto previousLineCount = captureStore_.lineCount();
    auto appendResult = captureStore_.finishInput();
    journalOutputExport( appendResult );
    try {
        // Output consumes the accepted batch before any cache allocation or observer.
        writeAppendedDisplayLines( appendResult );
        if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Strip && appendResult.outputFailure ) {
            reportCaptureOutputFailure( captureStoreOutputError( appendResult.outputFailure ) );
        }
        checkPreservedOutputState();

        // finishInput() can rotate+trim the Preserve-mode output. Handle it exactly like
        // appendUtf8() so caches stay consistent and Truncated fires — previously
        // finishInput() skipped this entirely.
        const auto trimResult = consumeTrimResult();
        const bool wasTrimmed = trimResult.trimmedLines > 0_lcount;
        const auto preAppendTotal = static_cast<qint64>( previousLineCount.get() );
        if ( !wasTrimmed
             || static_cast<qint64>( trimResult.trimmedLines.get() ) <= preAppendTotal ) {
            rememberAppendedRawLines( appendResult );
        }

        if ( wasTrimmed ) {
            Q_EMIT fileChanged( MonitoredFileStatus::Truncated );
        }
        const auto currentLineCount = captureStore_.lineCount();
        if ( currentLineCount != previousLineCount ) {
            Q_EMIT fileChanged( MonitoredFileStatus::DataAdded );
        }
        if ( appendResult.lineCount > 0_lcount || wasTrimmed ) {
            scheduleLoadingFinished();
        }
        if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Strip && rollingDisplayOutput_.isValid()
             && !rollingDisplayOutput_.flush() ) {
            closeDisplayOutputFile( false );
            appendResult.outputFailure = CaptureStore::OutputFailure::Flush;
            reportCaptureOutputFailure( CaptureOutputError::Flush );
        }
        reportPersistenceState( appendResult.persistence );
    } catch ( const std::exception& error ) {
        LOG_ERROR << "Streaming observer failed after capture commit: " << error.what();
        appendResult.notificationFailed = true;
    } catch ( ... ) {
        LOG_ERROR << "Streaming observer failed after capture commit with unknown exception";
        appendResult.notificationFailed = true;
    }
    return appendResult;
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
        cachedRawMetadataBytes_ = 0;
    }
    clearAnsiDisplayCache();
    return trimResult;
}

void StreamingLogData::clearCapture()
{
    std::lock_guard<std::recursive_mutex> orderingLock( appendOrderingMutex_ );
    pendingOutputExport_.reset();
    const auto timerWasActive = outputFlushTimer_.isActive();
    stopOutputFlushTimer();
    captureStore_.clear();
    clearAnsiDisplayCache();
    {
        std::lock_guard<std::mutex> lock( cachedRawBatchesMutex_ );
        cachedRawBatches_.clear();
        cachedRawBytes_ = 0;
        cachedRawMetadataBytes_ = 0;
    }
    const auto boundPath = boundOutputFile();
    if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Strip && !boundPath.isEmpty() ) {
        if ( rollingDisplayOutput_.clearIfCurrent() ) {
            displayOutputNeedsSeparator_ = false;
            reportCaptureOutputHealthy();
        }
        else {
            closeDisplayOutputFile( false );
            reportCaptureOutputFailure( CaptureOutputError::Reopen );
        }
    }
    else if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Preserve && !boundPath.isEmpty() ) {
        if ( !captureStore_.boundOutputFile().isEmpty() ) {
            reportCaptureOutputHealthy();
        }
        else {
            reportCaptureOutputFailure( CaptureOutputError::Reopen );
        }
    }

    // Only restart the timer if it was running before the clear,
    // so a clearCapture after finishInput does not revive the timer.
    if ( timerWasActive && isOutputFileActive() ) {
        startOutputFlushTimer();
    }

    reportPersistenceState( captureStore_.persistenceState() );
    Q_EMIT fileChanged( MonitoredFileStatus::Truncated );
    scheduleLoadingFinished();
}

void StreamingLogData::setCaptureLimits( CaptureStore::Limits limits )
{
    rollingMaxFileSize_ = limits.rollingMaxFileSize;
    rollingBackupCount_ = limits.rollingBackupCount;
    captureStore_.setLimits( std::move( limits ) );
    const auto trimmed = consumeTrimResult();
    if ( trimmed.trimmedLines > 0_lcount ) {
        Q_EMIT fileChanged( MonitoredFileStatus::Truncated );
        scheduleLoadingFinished();
    }
    reportPersistenceState( captureStore_.persistenceState() );
}

CaptureOutputError
StreamingLogData::captureStoreOutputError( std::optional<CaptureStore::OutputFailure> failure )
{
    if ( !failure.has_value() ) {
        return CaptureOutputError::Open;
    }
    switch ( *failure ) {
    case CaptureStore::OutputFailure::Open:
        return CaptureOutputError::Open;
    case CaptureStore::OutputFailure::Write:
        return CaptureOutputError::Write;
    case CaptureStore::OutputFailure::Flush:
        return CaptureOutputError::Flush;
    }
    return CaptureOutputError::Open;
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
    std::lock_guard<std::recursive_mutex> orderingLock( appendOrderingMutex_ );
    if ( pendingOutputExport_.has_value() ) {
        return false;
    }
    stopOutputFlushTimer();
    const auto previousOutputPath = boundOutputFile();
    const auto previousAnsiMode = outputSaveAnsiMode_;
    const auto hadBoundOutput = !previousOutputPath.isEmpty();
    const auto hadActiveOutput = hadBoundOutput && isOutputFileActive();

    if ( outputPath.isEmpty() ) {
        outputSaveAnsiMode_ = ansiMode;
        closeDisplayOutputFile();
        captureStore_.bindOutputFile( QString{} );
        reportCaptureOutputHealthy();
        return true;
    }

    const auto normalizedPath = []( const QString& path ) {
        const QFileInfo info( path );
        const auto canonicalPath = info.canonicalFilePath();
        return canonicalPath.isEmpty() ? info.absoluteFilePath() : canonicalPath;
    };
    const auto sameLexicalPath
        = QDir::cleanPath( outputPath ) == QDir::cleanPath( previousOutputPath );
    const auto refersToActiveOutput = outputRefersToPath( outputPath );
    const auto rebindsActivePath
        = hadBoundOutput
          && ( sameLexicalPath
               || normalizedPath( outputPath ) == normalizedPath( previousOutputPath )
               || refersToActiveOutput );
    if ( mode == OutputBindMode::FreshSave && rebindsActivePath && isOutputFileActive() ) {
        if ( refersToActiveOutput ) {
            if ( ansiMode != previousAnsiMode ) {
                startOutputFlushTimer();
                return false;
            }
            reportCaptureOutputHealthy();
            startOutputFlushTimer();
            return true;
        }
        // The active handle may refer to a pathname that was moved or deleted.
        // Rebind below so Save As can recreate the requested path from capture.
    }

    outputSaveAnsiMode_ = ansiMode;

    // FreshSave truncates the destination and replays the current capture
    // (user-initiated "Save As" — overwrite was already confirmed by the
    // dialog). Restore always uses append/create semantics so a path created
    // concurrently cannot be truncated. openedNewFile() determines whether a
    // missing destination was created and therefore needs capture replay.
    const bool preserveExisting = mode == OutputBindMode::Restore;

    OutputBindResult binding;
    if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Preserve ) {
        binding.success = captureStore_.bindOutputFile( outputPath, preserveExisting );
        binding.error = captureStoreOutputError( captureStore_.outputFailure() );
        if ( binding.success ) {
            closeDisplayOutputFile( false );
            const std::lock_guard<std::mutex> lock( boundOutputFileMutex_ );
            boundOutputFile_ = outputPath;
        }
    }
    else {
        binding = openDisplayOutputFile( outputPath, preserveExisting );
        if ( binding.success ) {
            captureStore_.bindOutputFile( QString{} );
        }
    }

    if ( binding.success ) {
        reportCaptureOutputHealthy();
        startOutputFlushTimer();
    }
    else {
        outputSaveAnsiMode_ = previousAnsiMode;
        if ( hadActiveOutput ) {
            // The previous manager and its open handle were not touched until the
            // candidate succeeded, so rollback is identity-preserving and needs no
            // pathname reopen.
            reportCaptureOutputHealthy();
            startOutputFlushTimer();
        }
        else if ( !hadBoundOutput && mode == OutputBindMode::Restore ) {
            outputSaveAnsiMode_ = ansiMode;
            {
                const std::lock_guard<std::mutex> lock( boundOutputFileMutex_ );
                boundOutputFile_ = outputPath;
            }
            reportCaptureOutputFailure( binding.error );
        }
    }
    return binding.success;
}

QString StreamingLogData::boundOutputFile() const
{
    const std::lock_guard<std::mutex> lock( boundOutputFileMutex_ );
    return boundOutputFile_;
}

bool StreamingLogData::hasActiveOutputBinding( const QString& outputPath,
                                               LiveLogSaveAnsiMode ansiMode ) const
{
    const std::lock_guard<std::recursive_mutex> orderingLock( appendOrderingMutex_ );
    return outputSaveAnsiMode_ == ansiMode && !captureOutputError_.has_value()
           && isOutputFileActive() && outputRefersToPath( outputPath );
}

std::optional<CaptureOutputError> StreamingLogData::captureOutputError() const
{
    return captureOutputError_;
}

std::optional<CaptureOutputError> StreamingLogData::flushOutputForClose()
{
    std::lock_guard<std::recursive_mutex> orderingLock( appendOrderingMutex_ );
    if ( outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Strip ) {
        if ( rollingDisplayOutput_.isValid() && !rollingDisplayOutput_.flush() ) {
            reportCaptureOutputFailure( CaptureOutputError::Flush );
            return CaptureOutputError::Flush;
        }
    }
    else {
        captureStore_.flush();
        checkPreservedOutputState();
    }
    return captureOutputError_;
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
    std::lock_guard<std::recursive_mutex> orderingLock( appendOrderingMutex_ );
    pendingOutputExport_.reset();
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

StreamingLogData::OutputBindResult
StreamingLogData::openDisplayOutputFile( const QString& outputPath, bool preserveExisting )
{
    if ( outputPath.isEmpty() ) {
        closeDisplayOutputFile();
        return { true, CaptureOutputError::Open };
    }

    const auto outputDirectory = QFileInfo( outputPath ).absoluteDir();
    QDir().mkpath( outputDirectory.absolutePath() );
    RollingFileManager candidateOutput( outputPath, rollingMaxFileSize_,
                                        rollingBackupCount_ );

    // Restore never publishes a partial replay at the requested pathname. Open
    // an existing file append-only; if it is missing, seed a unique sibling and
    // publish it with a no-overwrite rename. If another process creates the
    // destination first, discard the staged replay and preserve that file.
    if ( preserveExisting ) {
        if ( !candidateOutput.openExisting() ) {
            CaptureOutputError replayError = CaptureOutputError::Write;
            const auto stagedResult = klogg::stagedoutput::publishSibling(
                outputPath, [ this, &replayError ]( QIODevice* output ) {
                    const auto replay = writeDisplayLinesToDevice( output );
                    replayError = replay.error;
                    return replay.success;
                } );
            std::optional<klogg::platform::FileIdentity> publishedIdentity;
            switch ( stagedResult.result ) {
            case klogg::stagedoutput::Result::Published:
                publishedIdentity = stagedResult.identity;
                if ( !publishedIdentity.has_value() ) {
                    return { false, CaptureOutputError::Open };
                }
                break;
            case klogg::stagedoutput::Result::DestinationExists:
                break;
            case klogg::stagedoutput::Result::WriteFailure:
                return { false, replayError };
            case klogg::stagedoutput::Result::FlushFailure:
                return { false, CaptureOutputError::Flush };
            case klogg::stagedoutput::Result::OpenFailure:
            case klogg::stagedoutput::Result::PublishFailure:
                return { false, CaptureOutputError::Open };
            }
            candidateOutput = RollingFileManager(
                outputPath, rollingMaxFileSize_, rollingBackupCount_ );
            if ( !candidateOutput.openExisting( publishedIdentity ) ) {
                return { false, CaptureOutputError::Open };
            }
        }
    }
    else {
        // FreshSave publishes through QSaveFile after overwrite confirmation, so
        // a replay or commit failure cannot expose a truncated public destination.
        QSaveFile stagedOutput( outputPath );
        if ( !stagedOutput.open( QIODevice::WriteOnly ) ) {
            return { false, CaptureOutputError::Open };
        }
        const auto replay = writeDisplayLinesToDevice( &stagedOutput );
        if ( !replay.success ) {
            stagedOutput.cancelWriting();
            return { false, replay.error };
        }
        const auto publishedIdentity = klogg::platform::fileIdentity( stagedOutput );
        if ( !publishedIdentity.has_value() ) {
            stagedOutput.cancelWriting();
            return { false, CaptureOutputError::Open };
        }
        if ( !stagedOutput.commit() ) {
            return { false, CaptureOutputError::Flush };
        }
        if ( !candidateOutput.openExisting( publishedIdentity ) ) {
            return { false, CaptureOutputError::Open };
        }
    }

    QFile tail( outputPath );
    if ( !tail.open( QIODevice::ReadOnly ) ) {
        return { false, CaptureOutputError::Open };
    }
    displayOutputNeedsSeparator_
        = tail.size() > 0 && ( !tail.seek( tail.size() - 1 ) || tail.read( 1 ) != "\n" );
    rollingDisplayOutput_ = std::move( candidateOutput );
    const std::lock_guard<std::mutex> lock( boundOutputFileMutex_ );
    boundOutputFile_ = outputPath;
    return { true, CaptureOutputError::Open };
}

void StreamingLogData::closeDisplayOutputFile( bool clearBinding )
{
    rollingDisplayOutput_.close();
    rollingDisplayOutput_ = RollingFileManager();
    if ( clearBinding ) {
        const std::lock_guard<std::mutex> lock( boundOutputFileMutex_ );
        boundOutputFile_.clear();
    }
}

QByteArray StreamingLogData::displayOutputRecord( const QByteArray& bytes, bool terminated ) const
{
    auto line = codec_.codec()->toUnicode( bytes );
    if ( !prefilterPattern_.pattern().isEmpty() ) {
        line.remove( prefilterPattern_ );
    }
    auto output = processAnsiSequences( line, AnsiProcessingMode::Strip ).text.toUtf8();
    if ( terminated ) {
        output.append( '\n' );
    }
    return output;
}

StreamingLogData::OutputBindResult StreamingLogData::writeDisplayLinesToDevice( QIODevice* output )
{
    if ( !output ) {
        return { false, CaptureOutputError::Write };
    }
    OutputExportCandidate candidate;
    candidate.snapshot = captureStore_.snapshot();
    candidate.snapshotFinalRecordUnterminated
        = captureStore_.finalRecordUnterminated();
    candidate.ansiMode = LiveLogSaveAnsiMode::Strip;
    candidate.codecName = codec_.codec()->name();
    candidate.prefilterPattern = prefilterPattern_.pattern();
    OutputExportEncodingState state;
    replayPeakBufferForTesting_ = 0;
    const auto write = [ this, output ]( const QByteArray& bytes ) {
        replayPeakBufferForTesting_
            = qMax<qint64>( replayPeakBufferForTesting_, bytes.size() );
        return output->write( bytes );
    };
    return writeOutputExportSnapshot( candidate, state, write )
               ? OutputBindResult{ true, CaptureOutputError::Open }
               : OutputBindResult{ false, CaptureOutputError::Write };
}

bool StreamingLogData::isOutputFileActive() const
{
    if ( boundOutputFile().isEmpty() ) {
        return false;
    }
    return outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Preserve
               ? !captureStore_.boundOutputFile().isEmpty()
               : rollingDisplayOutput_.isValid();
}

bool StreamingLogData::outputRefersToPath( const QString& path ) const
{
    return outputSaveAnsiMode_ == LiveLogSaveAnsiMode::Preserve
               ? captureStore_.outputRefersToPath( path )
               : rollingDisplayOutput_.refersToPath( path );
}

bool StreamingLogData::suspendOutputForReplacement(
    const QString& outputPath,
    std::optional<SuspendedOutputBinding>& suspended )
{
    suspended.reset();
    if ( !outputRefersToPath( outputPath ) ) {
        return true;
    }

    SuspendedOutputBinding binding;
    binding.ansiMode = outputSaveAnsiMode_;
    const auto identity
        = binding.ansiMode == LiveLogSaveAnsiMode::Preserve
              ? captureStore_.suspendOutputForReplacement( outputPath )
              : rollingDisplayOutput_.suspendForReplacement( outputPath );
    if ( !identity.has_value() ) {
        abandonOutputAfterFailedReplacement(
            binding, CaptureStore::OutputFailure::Flush );
        return false;
    }
    binding.identity = *identity;
    suspended = binding;
    return true;
}

bool StreamingLogData::restoreOutputAfterFailedReplacement(
    const std::optional<SuspendedOutputBinding>& suspended )
{
    if ( !suspended.has_value() ) {
        return true;
    }
    const auto restored
        = suspended->ansiMode == LiveLogSaveAnsiMode::Preserve
              ? captureStore_.restoreOutputAfterFailedReplacement(
                    suspended->identity )
              : rollingDisplayOutput_.openExisting( suspended->identity );
    if ( !restored ) {
        abandonOutputAfterFailedReplacement(
            suspended, CaptureStore::OutputFailure::Open );
        return false;
    }
    reportCaptureOutputHealthy();
    return true;
}

void StreamingLogData::abandonOutputAfterFailedReplacement(
    const std::optional<SuspendedOutputBinding>& suspended,
    CaptureStore::OutputFailure failure )
{
    if ( !suspended.has_value() ) {
        // An unrelated destination failed; the committed writer was untouched.
        return;
    }
    if ( suspended->ansiMode == LiveLogSaveAnsiMode::Preserve ) {
        captureStore_.abandonOutputAfterFailedReplacement( failure );
    }
    else {
        closeDisplayOutputFile( false );
    }
    stopOutputFlushTimer();
    reportCaptureOutputFailure( captureStoreOutputError( failure ) );
}

void StreamingLogData::reportCaptureOutputHealthy()
{
    if ( !captureOutputError_.has_value() ) {
        return;
    }
    const auto recoveredError = *captureOutputError_;
    captureOutputError_.reset();
    Q_EMIT captureOutputChanged( true, recoveredError );
}

void StreamingLogData::reportCaptureOutputFailure( CaptureOutputError error )
{
    if ( captureOutputError_ == error ) {
        return;
    }
    captureOutputError_ = error;
    Q_EMIT captureOutputChanged( false, error );
}

void StreamingLogData::checkPreservedOutputState()
{
    if ( outputSaveAnsiMode_ != LiveLogSaveAnsiMode::Preserve
         || boundOutputFile().isEmpty() || !captureStore_.boundOutputFile().isEmpty() ) {
        return;
    }
    reportCaptureOutputFailure( captureStoreOutputError( captureStore_.outputFailure() ) );
}

void StreamingLogData::journalOutputExport(
    const CaptureStore::AppendResult& appendResult ) noexcept
{
    if ( !pendingOutputExport_.has_value() || pendingOutputExport_->failure.has_value() ) {
        return;
    }
    if ( appendResult.disposition == CaptureStore::AppendDisposition::PartialUnknown ) {
        pendingOutputExport_->failure = OutputExportFailure::PartialUnknown;
        pendingOutputExport_->tail.clear();
        pendingOutputExport_->tailBytes = 0;
        return;
    }
    if ( appendResult.lineCount <= 0_lcount || appendResult.rawUtf8Lines.isEmpty() ) {
        return;
    }

    const auto incomingBytes = static_cast<qint64>( appendResult.rawUtf8Lines.size() );
    if ( incomingBytes > pendingOutputExport_->maximumTailBytes
         || pendingOutputExport_->tailBytes
                    > pendingOutputExport_->maximumTailBytes - incomingBytes ) {
        pendingOutputExport_->failure = OutputExportFailure::TailOverflow;
        pendingOutputExport_->tail.clear();
        pendingOutputExport_->tailBytes = 0;
        return;
    }

    try {
        if ( beforeOutputExportJournalForTesting_ ) {
            beforeOutputExportJournalForTesting_();
        }
        if ( nextOutputDeliverySequence_ == std::numeric_limits<std::uint64_t>::max() ) {
            throw std::overflow_error( "live output export sequence exhausted" );
        }
        OutputExportBatch batch;
        const auto sequence = nextOutputDeliverySequence_ + 1u;
        batch.sequence = sequence;
        batch.rawUtf8Lines = appendResult.rawUtf8Lines;
        batch.endOfLines = appendResult.endOfLines;
        batch.finalRecordUnterminated = appendResult.finalRecordUnterminated;
        pendingOutputExport_->tail.push_back( std::move( batch ) );
        pendingOutputExport_->tailBytes += incomingBytes;
        nextOutputDeliverySequence_ = sequence;
    } catch ( ... ) {
        pendingOutputExport_->failure = OutputExportFailure::TailOverflow;
        pendingOutputExport_->tail.clear();
        pendingOutputExport_->tailBytes = 0;
    }
}

void StreamingLogData::writeAppendedDisplayLines( CaptureStore::AppendResult& appendResult )
{
    if ( outputSaveAnsiMode_ != LiveLogSaveAnsiMode::Strip ) {
        return;
    }

    const auto appended = appendResult.lineCount.get();
    if ( appended == 0 ) {
        return;
    }

    // The normalized committed batch owns records even if retention has already
    // removed them. Never reconstruct an output transaction from the display tail.
    appendResult.outputAttempted = rollingDisplayOutput_.isValid();
    appendResult.outputBytes = 0;
    try {
        qint64 start = 0;
        for ( const auto end : appendResult.endOfLines ) {
            if ( !rollingDisplayOutput_.isValid() ) {
                break;
            }
            const auto bytes = appendResult.rawUtf8Lines.mid( static_cast<int>( start ),
                                                              static_cast<int>( end - start - 1 ) );
            const bool unterminated
                = appendResult.finalRecordUnterminated && end == appendResult.endOfLines.back();
            auto output = displayOutputRecord( bytes, !unterminated );
            if ( displayOutputNeedsSeparator_ ) {
                output.prepend( '\n' );
            }
            qint64 offset = 0;
            while ( offset < output.size() ) {
                const auto remaining = output.mid( static_cast<int>( offset ) );
                const auto written = outputWriteForTesting_
                                         ? outputWriteForTesting_( remaining )
                                         : rollingDisplayOutput_.write( remaining );
                if ( written <= 0 ) {
                    closeDisplayOutputFile( false );
                    appendResult.outputFailure = CaptureStore::OutputFailure::Write;
                    return;
                }
                *appendResult.outputBytes += written;
                offset += written;
            }
            displayOutputNeedsSeparator_ = unterminated;
            start = end;
        }
        if ( rollingDisplayOutput_.isValid() && !rollingDisplayOutput_.flush() ) {
            closeDisplayOutputFile( false );
            appendResult.outputFailure = CaptureStore::OutputFailure::Flush;
        }
    } catch ( const std::exception& error ) {
        LOG_ERROR << "Streaming output exception after capture commit: " << error.what();
        appendResult.outputBytes.reset();
        appendResult.outputFailure = CaptureStore::OutputFailure::Write;
        closeDisplayOutputFile( false );
    } catch ( ... ) {
        LOG_ERROR << "Streaming output unknown exception after capture commit";
        appendResult.outputBytes.reset();
        appendResult.outputFailure = CaptureStore::OutputFailure::Write;
        closeDisplayOutputFile( false );
    }
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

qint64 StreamingLogData::cachedRawBatchMetadataBytes( const CachedRawBatch& batch )
{
    constexpr auto fixedBytes = static_cast<qint64>( sizeof( CachedRawBatch ) );
    constexpr auto entryBytes = static_cast<qint64>( sizeof( qint64 ) );
    const auto maximumEntries = static_cast<std::uintmax_t>(
        ( std::numeric_limits<qint64>::max() - fixedBytes ) / entryBytes );
    const auto capacity = static_cast<std::uintmax_t>( batch.endOfLines.capacity() );
    if ( capacity > maximumEntries ) {
        return std::numeric_limits<qint64>::max();
    }
    return fixedBytes + static_cast<qint64>( capacity ) * entryBytes;
}

void StreamingLogData::rememberAppendedRawLines( const CaptureStore::AppendResult& appendResult )
{
    if ( appendResult.lineCount <= 0_lcount || appendResult.rawUtf8Lines.isEmpty() ) {
        return;
    }

    std::lock_guard<std::mutex> lock( cachedRawBatchesMutex_ );
    const auto incomingBytes = static_cast<qint64>( appendResult.rawUtf8Lines.size() );
    bool merged = false;
    if ( !cachedRawBatches_.empty() ) {
        auto& tail = cachedRawBatches_.back();
        const auto combinedBytes = static_cast<qint64>( tail->rawUtf8Lines.size() ) + incomingBytes;
        const auto combinedLines = tail->lineCount.get() + appendResult.lineCount.get();
        if ( tail.use_count() == 1 && tail->firstLine + tail->lineCount == appendResult.firstLine
             && combinedBytes <= CachedRawBatchTargetBytes
             && combinedLines <= CachedRawBatchLineLimit ) {
            const auto previousMetadataBytes = cachedRawBatchMetadataBytes( *tail );
            const auto byteOffset = static_cast<qint64>( tail->rawUtf8Lines.size() );
            tail->rawUtf8Lines.append( appendResult.rawUtf8Lines );
            for ( const auto lineEnd : appendResult.endOfLines ) {
                tail->endOfLines.push_back( byteOffset + lineEnd );
            }
            tail->lineCount = LinesCount( combinedLines );
            cachedRawMetadataBytes_
                = qMax<qint64>( 0, cachedRawMetadataBytes_ - previousMetadataBytes );
            const auto mergedMetadataBytes = cachedRawBatchMetadataBytes( *tail );
            cachedRawMetadataBytes_
                = mergedMetadataBytes
                          > std::numeric_limits<qint64>::max() - cachedRawMetadataBytes_
                      ? std::numeric_limits<qint64>::max()
                      : cachedRawMetadataBytes_ + mergedMetadataBytes;
            merged = true;
        }
    }

    if ( !merged ) {
        auto batch = std::make_shared<CachedRawBatch>();
        batch->firstLine = appendResult.firstLine;
        batch->lineCount = appendResult.lineCount;
        batch->rawUtf8Lines = appendResult.rawUtf8Lines;
        batch->endOfLines = appendResult.endOfLines;
        const auto metadataBytes = cachedRawBatchMetadataBytes( *batch );
        cachedRawMetadataBytes_
            = metadataBytes > std::numeric_limits<qint64>::max() - cachedRawMetadataBytes_
                  ? std::numeric_limits<qint64>::max()
                  : cachedRawMetadataBytes_ + metadataBytes;
        cachedRawBatches_.push_back( std::move( batch ) );
    }
    cachedRawBytes_ += incomingBytes;

    while ( !cachedRawBatches_.empty()
            && ( cachedRawBytes_ > CachedRawBatchBytesLimit
                 || cachedRawBatches_.size() > cachedRawBatchCountLimit_
                 || cachedRawMetadataBytes_ > cachedRawMetadataBytesLimit_ ) ) {
        const auto& oldest = *cachedRawBatches_.front();
        cachedRawBytes_ -= oldest.rawUtf8Lines.size();
        cachedRawMetadataBytes_
            = qMax<qint64>( 0, cachedRawMetadataBytes_ - cachedRawBatchMetadataBytes( oldest ) );
        cachedRawBatches_.pop_front();
    }
    if ( cachedRawBatches_.empty() ) {
        cachedRawBytes_ = 0;
        cachedRawMetadataBytes_ = 0;
    }
}

std::optional<SearchableLogData::RawLines>
StreamingLogData::tryBuildCachedRawLines( LineNumber first, LinesCount number ) const
{
    if ( number <= 0_lcount ) {
        return RawLines{};
    }

    struct CachedRawSlice {
        std::shared_ptr<const CachedRawBatch> batch;
        size_t localStart = 0;
        size_t localEnd = 0;
    };

    auto nextLine = first;
    const auto requestedEnd = first + number;
    std::vector<CachedRawSlice> slices;
    slices.reserve( 4u );
    {
        std::lock_guard<std::mutex> lock( cachedRawBatchesMutex_ );
        auto batchIt = std::lower_bound(
            cachedRawBatches_.cbegin(), cachedRawBatches_.cend(), nextLine,
            [ this ]( const auto& batch, LineNumber line ) {
                ++cachedRawLookupBatchVisitsForTesting_;
                return batch->firstLine + batch->lineCount <= line;
            } );
        for ( ; batchIt != cachedRawBatches_.cend(); ++batchIt ) {
            ++cachedRawLookupBatchVisitsForTesting_;
            const auto& batch = **batchIt;
            const auto batchEnd = batch.firstLine + batch.lineCount;
            if ( batch.firstLine > nextLine ) {
                break;
            }

            const auto localStart
                = static_cast<size_t>( nextLine.get() - batch.firstLine.get() );
            const auto localEnd = static_cast<size_t>(
                qMin( batchEnd.get(), requestedEnd.get() ) - batch.firstLine.get() );
            if ( localStart >= localEnd || localEnd > batch.endOfLines.size() ) {
                break;
            }

            slices.push_back( { *batchIt, localStart, localEnd } );
            nextLine = LineNumber(
                batch.firstLine.get() + static_cast<LineNumber::UnderlyingType>( localEnd ) );
            if ( nextLine >= requestedEnd ) {
                break;
            }
        }
    }
    if ( nextLine < requestedEnd ) {
        return std::nullopt;
    }

    RawLines rawLines;
    rawLines.startLine = first;
    auto* utf8Codec = QTextCodec::codecForName( "UTF-8" );
    rawLines.textDecoder.decoder.reset( utf8Codec->makeDecoder() );
    rawLines.textDecoder.encodingParams = EncodingParameters( utf8Codec );
    rawLines.textDecoder.encodingParams.isUtf8Compatible = true;
    rawLines.textDecoder.encodingParams.lineFeedWidth = 1;

    for ( const auto& slice : slices ) {
        const auto& batch = *slice.batch;
        const auto byteStart
            = slice.localStart == 0 ? 0 : batch.endOfLines[ slice.localStart - 1 ];
        const auto byteEnd = batch.endOfLines[ slice.localEnd - 1 ];
        if ( byteStart < 0 || byteEnd < byteStart
             || byteEnd > static_cast<qint64>( batch.rawUtf8Lines.size() ) ) {
            return std::nullopt;
        }
        const auto existingBytes = klogg::ssize( rawLines.buffer );
        rawLines.buffer.insert( rawLines.buffer.end(), batch.rawUtf8Lines.constData() + byteStart,
                                batch.rawUtf8Lines.constData() + byteEnd );
        for ( auto line = slice.localStart; line < slice.localEnd; ++line ) {
            rawLines.endOfLines.push_back( existingBytes + batch.endOfLines[ line ] - byteStart );
        }
    }
    return rawLines;
}

void StreamingLogData::reportPersistenceState( const CaptureStore::PersistenceResult& state )
{
    if ( state.failure == persistenceFailure_ ) {
        return;
    }
    const auto error = state.failure ? *state.failure : *persistenceFailure_;
    persistenceFailure_ = state.failure;
    Q_EMIT capturePersistenceChanged( !state.failure, error );
}

CaptureStore::PersistenceResult StreamingLogData::persistCapture( int maxSegments )
{
    const auto result = captureStore_.persistCapture( maxSegments );
    reportPersistenceState( result );
    return result;
}

CaptureStore::PersistenceResult StreamingLogData::retryPersistence( int maxSegments )
{
    const auto result = captureStore_.retryPersistence( maxSegments );
    reportPersistenceState( result );
    return result;
}

CaptureStore::PersistenceResult StreamingLogData::persistenceState() const
{
    return captureStore_.persistenceState();
}

CaptureStore::Snapshot StreamingLogData::captureSnapshot() const
{
    return captureStore_.snapshot();
}
