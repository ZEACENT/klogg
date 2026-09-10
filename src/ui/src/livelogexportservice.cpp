/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#include "livelogexportservice.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QMetaType>
#include <QSaveFile>
#include <QThread>
#include <QTimer>

#include "logger.h"

namespace klogg::livelog {

struct LiveLogExportJob::OwnerCall {
    explicit OwnerCall( std::function<void()> callback )
        : operation( std::move( callback ) )
    {
    }

    std::function<void()> operation;
    std::mutex mutex;
    std::condition_variable finished;
    bool completed = false;
    std::exception_ptr failure;
};

LiveLogExportJob::LiveLogExportJob(
    std::shared_ptr<StreamingLogData> data,
    StreamingLogData::OutputExportCandidate candidate, QString outputPath,
    std::function<void()> beforeSnapshotWrite, std::function<void()> beforePublication,
    std::function<void()> afterPublish )
    : data_( std::move( data ) )
    , candidate_( std::move( candidate ) )
    , outputPath_( std::move( outputPath ) )
    , beforeSnapshotWrite_( std::move( beforeSnapshotWrite ) )
    , beforePublication_( std::move( beforePublication ) )
    , afterPublish_( std::move( afterPublish ) )
{
    moveToThread( data_->thread() );
    static const auto registered
        = qRegisterMetaType<LiveLogExportResult>( "klogg::livelog::LiveLogExportResult" );
    Q_UNUSED( registered );
}

LiveLogExportJob::~LiveLogExportJob()
{
    cancel();
    waitForFinished();
    if ( worker_.joinable() && worker_.get_id() != std::this_thread::get_id() ) {
        worker_.join();
    }
}

void LiveLogExportJob::start()
{
    const auto self = shared_from_this();
    worker_ = std::thread( [ self ] { self->run(); } );
}

void LiveLogExportJob::cancel()
{
    auto expected = PublicationDecision::Writing;
    if ( !publicationDecision_.compare_exchange_strong(
             expected, PublicationDecision::Cancelled,
             std::memory_order_acq_rel, std::memory_order_acquire ) ) {
        return;
    }
    cancelCandidate();
}

void LiveLogExportJob::waitForFinished()
{
    while ( true ) {
        {
            std::unique_lock<std::mutex> lock( stateMutex_ );
            if ( result_.has_value() ) {
                break;
            }
            if ( data_ == nullptr || QThread::currentThread() != data_->thread() ) {
                finishedCondition_.wait_for( lock, std::chrono::milliseconds{ 10 } );
                continue;
            }
        }
        constexpr auto eventFlags = QEventLoop::ExcludeUserInputEvents;
        if ( ownerEventPumpForTesting_ ) {
            try {
                ownerEventPumpForTesting_( eventFlags, 10 );
            } catch ( ... ) {
                QCoreApplication::processEvents( eventFlags, 10 );
            }
        }
        else {
            QCoreApplication::processEvents( eventFlags, 10 );
        }
    }
    if ( worker_.joinable() && worker_.get_id() != std::this_thread::get_id() ) {
        worker_.join();
    }
}

void LiveLogExportJob::onFinished(
    QObject* context, std::function<void( LiveLogExportResult )> callback )
{
    if ( context == nullptr || !callback ) {
        return;
    }

    std::optional<LiveLogExportResult> completedResult;
    {
        const std::lock_guard<std::mutex> lock( stateMutex_ );
        if ( !result_.has_value() ) {
            QObject::connect( this, &LiveLogExportJob::finished, context,
                              std::move( callback ) );
            return;
        }
        completedResult = result_;
    }

    QTimer::singleShot(
        0, Qt::PreciseTimer, context,
        [ callback = std::move( callback ), result = completedResult.value() ] {
            callback( result );
        } );
}

bool LiveLogExportJob::isFinished() const
{
    const std::lock_guard<std::mutex> lock( stateMutex_ );
    return result_.has_value();
}

std::optional<LiveLogExportResult> LiveLogExportJob::result() const
{
    const std::lock_guard<std::mutex> lock( stateMutex_ );
    return result_;
}

QString LiveLogExportJob::outputPath() const
{
    return outputPath_;
}

std::thread::id LiveLogExportJob::workerThreadId() const
{
    const std::lock_guard<std::mutex> lock( stateMutex_ );
    return workerThreadId_;
}

LiveLogExportResult
LiveLogExportJob::mapFailure( StreamingLogData::OutputExportFailure failure )
{
    switch ( failure ) {
    case StreamingLogData::OutputExportFailure::Busy:
        return LiveLogExportResult::Busy;
    case StreamingLogData::OutputExportFailure::Cancelled:
        return LiveLogExportResult::Cancelled;
    case StreamingLogData::OutputExportFailure::TailOverflow:
        return LiveLogExportResult::TailOverflow;
    case StreamingLogData::OutputExportFailure::PartialUnknown:
        return LiveLogExportResult::PartialUnknown;
    case StreamingLogData::OutputExportFailure::SnapshotRead:
        return LiveLogExportResult::SnapshotReadFailed;
    case StreamingLogData::OutputExportFailure::Write:
        return LiveLogExportResult::WriteFailed;
    case StreamingLogData::OutputExportFailure::Publish:
        return LiveLogExportResult::PublishFailed;
    case StreamingLogData::OutputExportFailure::PublishedReopen:
        return LiveLogExportResult::PublishedReopenFailed;
    case StreamingLogData::OutputExportFailure::PublishedCutover:
        return LiveLogExportResult::PublishedCutoverFailed;
    }
    return LiveLogExportResult::WriteFailed;
}

void LiveLogExportJob::run()
{
    {
        const std::lock_guard<std::mutex> lock( stateMutex_ );
        workerThreadId_ = std::this_thread::get_id();
    }

    if ( beforeSnapshotWrite_ ) {
        beforeSnapshotWrite_();
    }
    auto stagedOutput = std::make_unique<QSaveFile>( outputPath_ );
    if ( !stagedOutput->open( QIODevice::WriteOnly ) ) {
        cancelCandidate();
        complete( LiveLogExportResult::WriteFailed );
        return;
    }

    qint64 bytesWritten = 0;
    bool writeFailed = false;
    const auto write = [ this, &stagedOutput, &bytesWritten,
                         &writeFailed ]( const QByteArray& bytes ) -> qint64 {
        const auto written = stagedOutput->write( bytes );
        if ( written <= 0 ) {
            writeFailed = true;
            return written;
        }
        bytesWritten += written;
        Q_EMIT progressChanged( bytesWritten );
        return written;
    };
    const auto cancelled = [ this ] {
        return publicationDecision_.load( std::memory_order_acquire )
               == PublicationDecision::Cancelled;
    };

    StreamingLogData::OutputExportEncodingState encodingState;
    if ( !StreamingLogData::writeOutputExportSnapshot( candidate_, encodingState, write,
                                                       cancelled ) ) {
        stagedOutput->cancelWriting();
        cancelCandidate();
        auto failure = LiveLogExportResult::SnapshotReadFailed;
        if ( cancelled() ) {
            failure = LiveLogExportResult::Cancelled;
        }
        else if ( writeFailed ) {
            failure = LiveLogExportResult::WriteFailed;
        }
        complete( failure );
        return;
    }

    StreamingLogData::OutputExportTail tail;
    try {
        tail = takeCandidateTail();
    }
    catch ( ... ) {
        // An owner-side tail handoff that threw leaves the tail state unknown;
        // the export cannot claim a complete or replayable result.
        stagedOutput->cancelWriting();
        cancelCandidate();
        complete( LiveLogExportResult::PartialUnknown );
        return;
    }
    if ( tail.failure.has_value() ) {
        stagedOutput->cancelWriting();
        cancelCandidate();
        complete( mapFailure( *tail.failure ) );
        return;
    }
    if ( !StreamingLogData::writeOutputExportBatches( candidate_, tail.batches, encodingState,
                                                      write ) ) {
        stagedOutput->cancelWriting();
        cancelCandidate();
        complete( LiveLogExportResult::WriteFailed );
        return;
    }
    if ( cancelled() ) {
        stagedOutput->cancelWriting();
        cancelCandidate();
        complete( LiveLogExportResult::Cancelled );
        return;
    }
    if ( beforePublication_ ) {
        beforePublication_();
    }

    auto expectedDecision = PublicationDecision::Writing;
    if ( !publicationDecision_.compare_exchange_strong(
             expectedDecision, PublicationDecision::Publishing,
             std::memory_order_acq_rel, std::memory_order_acquire ) ) {
        stagedOutput->cancelWriting();
        complete( LiveLogExportResult::Cancelled );
        return;
    }
    if ( QThread::currentThread() != data_->thread() ) {
        stagedOutput->moveToThread( data_->thread() );
    }
    auto* ownerStagedOutput = stagedOutput.release();
    StreamingLogData::OutputExportActivation activation;
    auto publish = [ this, ownerStagedOutput, &activation,
                     encodingState = std::move( encodingState ) ]() mutable {
        const std::unique_ptr<QSaveFile> stagedOutputOwner( ownerStagedOutput );
        {
            const std::lock_guard<std::mutex> lock( stateMutex_ );
            dataAccessThreadId_ = std::this_thread::get_id();
        }
        activation = data_->publishStagedOutputExport(
            candidate_.id, outputPath_, std::move( encodingState ),
            *stagedOutputOwner, afterPublish_ );
    };

    bool invoked = false;
    try {
        invoked = invokeOnDataThread( publish );
    }
    catch ( ... ) {
        // The owner-side publication threw: whether the destination was
        // replaced is unknown, so the result must not claim success.
        complete( LiveLogExportResult::PartialUnknown );
        return;
    }
    if ( !invoked ) {
        ownerStagedOutput->deleteLater();
        cancelCandidate();
        complete( LiveLogExportResult::Cancelled );
        return;
    }

    complete( activation.success
                  ? LiveLogExportResult::Succeeded
                  : mapFailure( activation.failure.value_or(
                        StreamingLogData::OutputExportFailure::PublishedCutover ) ) );
}

void LiveLogExportJob::cancelCandidate()
{
    const auto cancel = [ this ] {
        {
            const std::lock_guard<std::mutex> lock( stateMutex_ );
            dataAccessThreadId_ = std::this_thread::get_id();
        }
        data_->cancelOutputExport( candidate_.id );
    };
    // Cancellation also runs from destructors; a failed owner call must not
    // escape it. The owner's state remains authoritative for any later call.
    try {
        invokeOnDataThread( cancel );
    }
    catch ( const std::exception& error ) {
        LOG_ERROR << "Live save cancellation owner call failed: " << error.what();
    }
    catch ( ... ) {
        LOG_ERROR << "Live save cancellation owner call failed";
    }
}

bool LiveLogExportJob::invokeOnDataThread( const std::function<void()>& operation )
{
    if ( QThread::currentThread() == data_->thread() ) {
        operation();
        return true;
    }

    const auto call = std::make_shared<OwnerCall>( operation );
    {
        const std::lock_guard<std::mutex> lock( ownerCallsMutex_ );
        ownerCalls_.push_back( call );
    }

    // Qt transports only a wakeup, never a transient callable or result. The
    // mailbox publishes requests and each call's condition publishes results,
    // including when Qt itself is not instrumented by ThreadSanitizer.
    if ( !QMetaObject::invokeMethod( this, "executeOwnerCalls", Qt::QueuedConnection ) ) {
        const std::lock_guard<std::mutex> lock( ownerCallsMutex_ );
        const auto queued = std::find( ownerCalls_.begin(), ownerCalls_.end(), call );
        if ( queued != ownerCalls_.end() ) {
            ownerCalls_.erase( queued );
            return false;
        }
        // An earlier wakeup already took this call. Its result remains binding.
    }

    std::unique_lock<std::mutex> lock( call->mutex );
    call->finished.wait( lock, [ &call ] { return call->completed; } );
    if ( call->failure ) {
        std::rethrow_exception( call->failure );
    }
    return true;
}

void LiveLogExportJob::executeOwnerCalls()
{
    while ( true ) {
        std::shared_ptr<OwnerCall> call;
        {
            const std::lock_guard<std::mutex> lock( ownerCallsMutex_ );
            if ( ownerCalls_.empty() ) {
                return;
            }
            call = std::move( ownerCalls_.front() );
            ownerCalls_.pop_front();
        }

        std::exception_ptr failure;
        try {
            call->operation();
        }
        catch ( ... ) {
            failure = std::current_exception();
        }
        {
            const std::lock_guard<std::mutex> lock( call->mutex );
            call->failure = failure;
            call->completed = true;
        }
        call->finished.notify_all();
    }
}

StreamingLogData::OutputExportTail LiveLogExportJob::takeCandidateTail()
{
    StreamingLogData::OutputExportTail tail;
    const auto take = [ this, &tail ] {
        {
            const std::lock_guard<std::mutex> lock( stateMutex_ );
            dataAccessThreadId_ = std::this_thread::get_id();
        }
        tail = data_->takeOutputExportTail( candidate_.id );
    };
    if ( !invokeOnDataThread( take ) ) {
        tail.failure = StreamingLogData::OutputExportFailure::Cancelled;
    }
    return tail;
}

void LiveLogExportJob::complete( LiveLogExportResult result )
{
    // A completed job retains only its observable result. Releasing the
    // immutable snapshot here lets CaptureStore retire trimmed spill files
    // even while the service keeps the latest job available to the UI.
    candidate_.snapshot = CaptureStore::Snapshot{};

    {
        const std::lock_guard<std::mutex> lock( stateMutex_ );
        if ( result_.has_value() ) {
            return;
        }
        result_ = result;
    }
    finishedCondition_.notify_all();
    Q_EMIT finished( result );
}

LiveLogExportService::LiveLogExportService( std::shared_ptr<StreamingLogData> data )
    : data_( std::move( data ) )
{
}

LiveLogExportService::~LiveLogExportService()
{
    cancelAndWait();
}

std::shared_ptr<LiveLogExportJob>
LiveLogExportService::start( const QString& outputPath, LiveLogSaveAnsiMode ansiMode,
                             qint64 maximumTailBytes )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if ( activeJob_ && !activeJob_->isFinished() ) {
        return {};
    }
    const auto candidate = data_->beginOutputExport( ansiMode, maximumTailBytes );
    if ( !candidate.has_value() ) {
        return {};
    }
    auto job = std::shared_ptr<LiveLogExportJob>( new LiveLogExportJob(
        data_, *candidate, outputPath, beforeSnapshotWriteForTesting_,
        beforePublicationForTesting_, afterPublishForTesting_ ) );
    activeJob_ = job;
    job->start();
    return job;
}

std::shared_ptr<LiveLogExportJob> LiveLogExportService::activeJob() const
{
    const std::lock_guard<std::mutex> lock( mutex_ );
    return activeJob_;
}

void LiveLogExportService::cancelAndWait() const
{
    auto job = activeJob();
    if ( job ) {
        if ( !job->isFinished() ) {
            job->cancel();
        }
        job->waitForFinished();
    }
}

} // namespace klogg::livelog
