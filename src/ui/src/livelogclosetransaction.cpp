/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#include "livelogclosetransaction.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "livelogcontroller.h"

namespace klogg::livelog {

LiveLogCloseTransaction::LiveLogCloseTransaction(
    LiveLogController& controller, AdbLogcatSource& source,
    LiveLogExportService& exportService, Mode mode, QObject* parent )
    : QObject( parent )
    , controller_( controller )
    , source_( source )
    , exportService_( exportService )
    , mode_( mode )
{
    timer_.setParent( this );
    timer_.setSingleShot( true );
    timer_.setTimerType( Qt::PreciseTimer );
    QObject::connect( &timer_, &QTimer::timeout, this, &LiveLogCloseTransaction::advance );
}

void LiveLogCloseTransaction::setCallbacks( FailureCallback failure,
                                            FinishedCallback finished )
{
    failureCallback_ = std::move( failure );
    finishedCallback_ = std::move( finished );
}

void LiveLogCloseTransaction::start()
{
    if ( stage_ != Stage::Idle ) {
        return;
    }
    stage_ = Stage::AwaitingStop;
    controller_.stopRequested( klogg::livecapture::StopDisposition::SettleAccepted );
    scheduleAdvance();
}

void LiveLogCloseTransaction::retry()
{
    if ( stage_ != Stage::AwaitingDecision ) {
        return;
    }
    const auto failureKind = lastFailureKind_;
    lastFailureKind_.reset();
    stage_ = Stage::Persisting;
    if ( failureKind == FailureKind::OutputFlush ) {
        scheduleAdvance();
        return;
    }
    const auto delay = lastPersistence_.has_value()
                           ? lastPersistence_->retryAfterMs.value_or( 1 )
                           : qint64{ 1 };
    scheduleAdvance( static_cast<int>( std::clamp<qint64>(
        delay, 1, std::numeric_limits<int>::max() ) ) );
}

void LiveLogCloseTransaction::cancel()
{
    if ( stage_ == Stage::Finished || stage_ == Stage::Idle ) {
        return;
    }
    timer_.stop();
    finish( Result::Cancelled );
}

void LiveLogCloseTransaction::closeAnywayPossibleLoss()
{
    if ( stage_ != Stage::AwaitingDecision ) {
        return;
    }
    finish( Result::CloseAnywayPossibleLoss );
}

bool LiveLogCloseTransaction::isRunning() const noexcept
{
    return stage_ != Stage::Idle && stage_ != Stage::Finished;
}

void LiveLogCloseTransaction::scheduleAdvance( int delayMs )
{
    timer_.start( std::max( 1, delayMs ) );
}

void LiveLogCloseTransaction::advance()
{
    switch ( stage_ ) {
    case Stage::Idle:
    case Stage::AwaitingDecision:
    case Stage::Finished:
        return;
    case Stage::AwaitingStop:
        if ( !source_.isInputTerminated() ) {
            scheduleAdvance();
            return;
        }
        if ( const auto job = exportService_.activeJob(); job ) {
            if ( !job->isFinished() ) {
                job->cancel();
                stage_ = Stage::AwaitingExport;
                scheduleAdvance();
                return;
            }
            job->waitForFinished();
        }
        stage_ = Stage::Persisting;
        advance();
        return;
    case Stage::AwaitingExport:
        if ( const auto job = exportService_.activeJob(); job ) {
            if ( !job->isFinished() ) {
                scheduleAdvance();
                return;
            }
            job->waitForFinished();
        }
        stage_ = Stage::Persisting;
        advance();
        return;
    case Stage::Persisting:
        if ( mode_ == Mode::Discard ) {
            const auto outputError = source_.flushOutputForClose();
            if ( outputError.has_value() ) {
                stage_ = Stage::AwaitingDecision;
                lastFailureKind_ = FailureKind::OutputFlush;
                if ( failureCallback_ ) {
                    failureCallback_( Failure{ FailureKind::OutputFlush,
                                               CaptureStore::PersistenceResult{}, outputError } );
                }
                return;
            }
            finish( Result::ReadyToRemove );
            return;
        }
        persistTurn();
        return;
    }
}

void LiveLogCloseTransaction::persistTurn()
{
    const auto persistence = source_.persistForClose( 32 );
    if ( !persistence.has_value() ) {
        stage_ = Stage::AwaitingStop;
        scheduleAdvance();
        return;
    }
    lastPersistence_ = persistence;
    if ( persistence->complete() ) {
        flushAndFinish();
        return;
    }
    if ( persistence->failure.has_value() ) {
        stage_ = Stage::AwaitingDecision;
        lastFailureKind_ = FailureKind::Persistence;
        if ( failureCallback_ ) {
            failureCallback_( Failure{ FailureKind::Persistence, *persistence, std::nullopt } );
        }
        return;
    }
    scheduleAdvance( static_cast<int>( std::clamp<qint64>(
        persistence->retryAfterMs.value_or( 1 ), 1,
        std::numeric_limits<int>::max() ) ) );
}

void LiveLogCloseTransaction::flushAndFinish()
{
    const auto outputError = source_.flushOutputForClose();
    if ( outputError.has_value() ) {
        stage_ = Stage::AwaitingDecision;
        lastFailureKind_ = FailureKind::OutputFlush;
        if ( failureCallback_ ) {
            failureCallback_( Failure{ FailureKind::OutputFlush,
                                       lastPersistence_.value_or(
                                           CaptureStore::PersistenceResult{} ),
                                       outputError } );
        }
        return;
    }
    finish( Result::ReadyToRemove );
}

void LiveLogCloseTransaction::finish( Result result )
{
    if ( stage_ == Stage::Finished ) {
        return;
    }
    stage_ = Stage::Finished;
    timer_.stop();
    if ( finishedCallback_ ) {
        finishedCallback_( result );
    }
}

} // namespace klogg::livelog
