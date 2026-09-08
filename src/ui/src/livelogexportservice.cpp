/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#include "livelogexportservice.h"

#include <chrono>
#include <utility>

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QMetaType>
#include <QSaveFile>
#include <QThread>

namespace klogg::livelog {

LiveLogExportJob::LiveLogExportJob(
    std::shared_ptr<StreamingLogData> data,
    StreamingLogData::OutputExportCandidate candidate, QString outputPath,
    std::function<void()> beforeSnapshotWrite, std::function<void()> afterPublish )
    : data_( std::move( data ) )
    , candidate_( std::move( candidate ) )
    , outputPath_( std::move( outputPath ) )
    , beforeSnapshotWrite_( std::move( beforeSnapshotWrite ) )
    , afterPublish_( std::move( afterPublish ) )
{
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
    if ( publicationStarted_.load( std::memory_order_acquire ) ) {
        return;
    }
    cancelRequested_.store( true, std::memory_order_release );
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
        QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
    }
    if ( worker_.joinable() && worker_.get_id() != std::this_thread::get_id() ) {
        worker_.join();
    }
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
    QSaveFile stagedOutput( outputPath_ );
    if ( !stagedOutput.open( QIODevice::WriteOnly ) ) {
        cancelCandidate();
        complete( LiveLogExportResult::WriteFailed );
        return;
    }

    qint64 bytesWritten = 0;
    bool writeFailed = false;
    const auto write = [ this, &stagedOutput, &bytesWritten,
                         &writeFailed ]( const QByteArray& bytes ) -> qint64 {
        const auto written = stagedOutput.write( bytes );
        if ( written <= 0 ) {
            writeFailed = true;
            return written;
        }
        bytesWritten += written;
        Q_EMIT progressChanged( bytesWritten );
        return written;
    };
    const auto cancelled = [ this ] {
        return cancelRequested_.load( std::memory_order_acquire );
    };

    StreamingLogData::OutputExportEncodingState encodingState;
    if ( !StreamingLogData::writeOutputExportSnapshot( candidate_, encodingState, write,
                                                       cancelled ) ) {
        stagedOutput.cancelWriting();
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

    const auto tail = takeCandidateTail();
    if ( tail.failure.has_value() ) {
        stagedOutput.cancelWriting();
        cancelCandidate();
        complete( mapFailure( *tail.failure ) );
        return;
    }
    if ( !StreamingLogData::writeOutputExportBatches( candidate_, tail.batches, encodingState,
                                                      write ) ) {
        stagedOutput.cancelWriting();
        cancelCandidate();
        complete( LiveLogExportResult::WriteFailed );
        return;
    }
    if ( cancelled() ) {
        stagedOutput.cancelWriting();
        cancelCandidate();
        complete( LiveLogExportResult::Cancelled );
        return;
    }

    publicationStarted_.store( true, std::memory_order_release );
    const auto identity = klogg::platform::fileIdentity( stagedOutput );
    if ( !identity.has_value() || !stagedOutput.commit() ) {
        cancelCandidate();
        complete( LiveLogExportResult::PublishFailed );
        return;
    }

    if ( afterPublish_ ) {
        afterPublish_();
    }
    const auto self = shared_from_this();
    QMetaObject::invokeMethod(
        data_.get(),
        [ self, identity = *identity, encodingState = std::move( encodingState ) ]() mutable {
            self->completePublished( identity, std::move( encodingState ) );
        },
        Qt::QueuedConnection );
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
    if ( QThread::currentThread() == data_->thread() ) {
        cancel();
        return;
    }
    QMetaObject::invokeMethod( data_.get(), cancel, Qt::BlockingQueuedConnection );
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
    if ( QThread::currentThread() == data_->thread() ) {
        take();
        return tail;
    }
    if ( !QMetaObject::invokeMethod( data_.get(), take, Qt::BlockingQueuedConnection ) ) {
        tail.failure = StreamingLogData::OutputExportFailure::Cancelled;
    }
    return tail;
}

void LiveLogExportJob::completePublished(
    const klogg::platform::FileIdentity& identity,
    StreamingLogData::OutputExportEncodingState encodingState )
{
    const auto activation = data_->activatePublishedOutputExport(
        candidate_.id, outputPath_, identity, std::move( encodingState ) );
    complete( activation.success ? LiveLogExportResult::Succeeded
                                 : mapFailure( activation.failure.value_or(
                                       StreamingLogData::OutputExportFailure::PublishedCutover ) ) );
}

void LiveLogExportJob::complete( LiveLogExportResult result )
{
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
        afterPublishForTesting_ ) );
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
