#include "abstractlogview.h"

#include <QFutureWatcher>
#include <QProgressDialog>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>
#include <exception>

#include "clipboard.h"
#include "log.h"

QRegularExpression AbstractLogView::matchSelectionExpression( bool quickFind ) const
{
    if ( quickFind ) {
        return quickFindPattern_ != nullptr && quickFindPattern_->isActive()
                   ? quickFindPattern_->regularExpression() : QRegularExpression{};
    }
    if ( searchPattern_.isBoolean || searchPattern_.isExclude || searchPattern_.pattern.isEmpty() ) {
        return {};
    }
    return searchPattern_.toRegularExpression();
}

bool AbstractLogView::canSelectAllMatches( bool quickFind ) const
{
    const auto expression = matchSelectionExpression( quickFind );
    return !expression.pattern().isEmpty() && expression.isValid();
}

void AbstractLogView::bindSourceInterpretation()
{
    disconnect( interpretationConnection_ );
    interpretationConnection_ = connect( logData_, &AbstractLogData::interpretationChanged,
        this, [ this ] {
            visibleLineMapKeyValid_ = false;
            invalidateMatchSelection();
        } );
}

klogg::LogSnapshot::Ptr AbstractLogView::remapMatchSelection(
    Selection& selection, const klogg::LogSnapshot::Ptr& previous ) const
{
    try {
        auto presentation = logData_->createReadSnapshot( klogg::SnapshotScope::PresentedRows );
        if ( !presentation ) {
            return {};
        }
        if ( previous && presentation->preservesPresentationOf( *previous ) ) {
            return presentation;
        }
        selection.remapCapturedRows( [ & ]( const klogg::LogRowId& identity ) {
            if ( !logData_->containsRowIdentity( identity ) ) {
                throw klogg::SnapshotError{ "Selected source row no longer exists" };
            }
            return presentation->presentedRowForIdentity( identity );
        } );
        return presentation;
    }
    catch ( const std::exception& error ) {
        LOG_DEBUG << "Match selection is no longer current: " << error.what();
        return {};
    }
}

bool AbstractLogView::hasMatchSelection() const
{
    return matchSelection_ || selection_.hasCapturedPayload() || selection_.hasStableNavigationAnchor();
}

void AbstractLogView::refreshMatchSelection()
{
    if ( selection_.hasCapturedPayload() || selection_.hasStableNavigationAnchor() ) {
        matchSelectionPresentation_ = remapMatchSelection( selection_, matchSelectionPresentation_ );
        if ( !matchSelectionPresentation_ ) {
            invalidateMatchSelection();
        }
    }
    else {
        matchSelectionPresentation_.reset();
    }
}

void AbstractLogView::showMatchSelectionProgress()
{
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    auto* progress = new QProgressDialog( tr( "Selecting match fragments..." ), tr( "Cancel" ),
                                          0, 1000, this );
    matchSelection_->progress = progress;
    progress->setWindowModality( Qt::NonModal );
    progress->setMinimumDuration( 750 );
    progress->setAutoClose( false );
    progress->setAutoReset( false );
    connect( progress, &QProgressDialog::canceled, this, &AbstractLogView::cancelMatchSelection );
}

bool AbstractLogView::selectAllMatches( bool quickFind, bool copy )
{
    if ( !canSelectAllMatches( quickFind ) ) {
        return false;
    }
    try {
        const auto source = logData_->createReadSnapshot(
            quickFind ? klogg::SnapshotScope::PresentedRows : klogg::SnapshotScope::Matches );
        if ( !source || source->lineCount() == 0_lcount ) {
            return false;
        }
        const auto presentation = quickFind ? source
            : logData_->createReadSnapshot( klogg::SnapshotScope::PresentedRows );
        if ( !presentation ) {
            return false;
        }
        cancelMatchSelection();
        quickFind_->stopSearch();
        acceptQuickFindResults_ = false;
        disableFollow();
        auto operation = std::make_unique<MatchSelection>();
        operation->totalRows = source->lineCount();
        operation->presentation = presentation;
        operation->copyWhenFinished = copy;
        operation->job = std::make_shared<klogg::MatchSelectionJob>(
            source, presentation, matchSelectionExpression( quickFind ), operation->cancelled );
        matchSelection_ = std::move( operation );
        showMatchSelectionProgress();
        const auto generation = ++matchSelectionGeneration_;
        QTimer::singleShot( 0, this, [ this, generation ] { continueMatchSelection( generation ); } );
        return true;
    }
    catch ( const std::exception& error ) {
        LOG_ERROR << "Cannot acquire match selection: " << error.what();
        cancelMatchSelection();
        return false;
    }
}

void AbstractLogView::continueMatchSelection( quint64 generation )
{
    if ( !matchSelection_ || generation != matchSelectionGeneration_ ) {
        return;
    }
    // QObject parent owns the watcher until its deferred deletion.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    auto* watcher = new QFutureWatcher<klogg::MatchSelectionStep>( this );
    connect( watcher, &QFutureWatcher<klogg::MatchSelectionStep>::finished, this,
             [ this, watcher, generation ] {
        watcher->deleteLater();
        matchSelectionWorkers_.erase(
            std::remove_if( matchSelectionWorkers_.begin(), matchSelectionWorkers_.end(),
                            []( const auto& worker ) { return worker.isFinished(); } ),
            matchSelectionWorkers_.end() );
        if ( !matchSelection_ || generation != matchSelectionGeneration_ ) {
            return;
        }
        try {
            const auto step = watcher->result();
            if ( step.outcome == klogg::MatchSelectionStep::Outcome::Failed ) {
                LOG_ERROR << "Match selection failed: " << step.error;
                finishMatchSelection( false );
                return;
            }
            if ( step.outcome == klogg::MatchSelectionStep::Outcome::Complete ) {
                if ( matchSelection_->serializing ) {
                    if ( step.withLineNumbers != matchSelection_->withLineNumbers ) {
                        // A newer Copy command changed policy after this worker started.
                        // Keep the accepted payload, but never publish obsolete formatting.
                        QTimer::singleShot( 0, this,
                            [ this, generation ] { continueMatchSelection( generation ); } );
                        return;
                    }
                    matchSelection_->clipboard = step.clipboard;
                    finishMatchSelection( step.clipboard.has_value() );
                    return;
                }
                matchSelection_->result = step.selection;
                if ( !matchSelection_->copyWhenFinished ) {
                    finishMatchSelection( true );
                    return;
                }
                matchSelection_->serializing = true;
                matchSelection_->progress->setLabelText( tr( "Copying match fragments..." ) );
            }
            const auto total = matchSelection_->totalRows.get();
            matchSelection_->progress->setValue( total == 0 ? 0 : static_cast<int>(
                1000.0 * static_cast<double>( step.processed ) / static_cast<double>( total ) ) );
            const QPointer<AbstractLogView> guard{ this };
            Q_EMIT matchSelectionProgress( step.processed, total );
            if ( guard && matchSelection_ && generation == matchSelectionGeneration_ ) {
                QTimer::singleShot( 0, this,
                    [ this, generation ] { continueMatchSelection( generation ); } );
            }
        }
        catch ( const std::exception& error ) {
            LOG_ERROR << "Match selection completion failed: " << error.what();
            finishMatchSelection( false );
        }
        catch ( ... ) {
            LOG_ERROR << "Unknown match selection completion failure";
            finishMatchSelection( false );
        }
    } );
    QFuture<klogg::MatchSelectionStep> future;
    if ( matchSelection_->serializing ) {
        const auto payload = matchSelection_->result;
        const auto cancelled = matchSelection_->cancelled;
        const auto numbered = matchSelection_->withLineNumbers;
        future = QtConcurrent::run( [ payload, cancelled, numbered ] {
            klogg::MatchSelectionStep step;
            step.withLineNumbers = numbered;
            try {
                const auto stopped = [ & ] { return cancelled->load( std::memory_order_relaxed ); };
                if ( !stopped() ) {
                    step.clipboard = payload->isEmpty() ? std::optional<QString>{ QString{} }
                        : payload->serializeCapturedText( numbered, stopped );
                }
                if ( step.clipboard ) {
                    step.clipboard->replace( QChar::Null, QChar::Space );
                }
                step.outcome = klogg::MatchSelectionStep::Outcome::Complete;
            }
            catch ( const std::exception& error ) {
                step.outcome = klogg::MatchSelectionStep::Outcome::Failed;
                step.error = QString::fromUtf8( error.what() );
            }
            return step;
        } );
    }
    else {
        const auto job = matchSelection_->job;
        future = QtConcurrent::run( [ job ] { return job->step(); } );
    }
    matchSelectionWorkers_.push_back( future );
    watcher->setFuture( future );
}

void AbstractLogView::finishMatchSelection( bool completed )
{
    if ( !matchSelection_ ) {
        return;
    }
    auto operation = std::move( matchSelection_ );
    const auto presentation = completed && operation->result
        ? remapMatchSelection( *operation->result, operation->presentation ) : klogg::LogSnapshot::Ptr{};
    if ( completed && !presentation ) {
        completed = false;
        matchSelectionPresentation_.reset();
        if ( selection_.hasCapturedPayload() ) {
            selection_.clear();
            forceRefresh();
        }
    }
    operation->cancelled->store( true, std::memory_order_relaxed );
    ++matchSelectionGeneration_;
    if ( completed && operation->publishSelection ) {
        selection_ = std::move( *operation->result );
        matchSelectionPresentation_ = selection_.hasCapturedPayload() ? presentation : nullptr;
        if ( !selection_.isEmpty() ) {
            selectionStartPos_ = selection_.getPreviousPosition();
            selectionCurrentEndPos_ = selection_.getNextPosition();
        }
        forceRefresh();
    }
    if ( operation->progress ) {
        disconnect( operation->progress.data(), nullptr, this, nullptr );
        operation->progress->hide();
        operation->progress->deleteLater();
    }
    const QPointer<AbstractLogView> guard{ this };
    if ( completed && operation->clipboard ) {
        sendTextToClipboard( std::move( *operation->clipboard ), true );
    }
    if ( guard ) {
        Q_EMIT matchSelectionFinished( completed );
    }
}

bool AbstractLogView::serializeMatchSelection( bool withLineNumbers )
{
    if ( matchSelection_ ) {
        matchSelection_->copyWhenFinished = true;
        matchSelection_->withLineNumbers = withLineNumbers;
        return true;
    }
    if ( !selection_.hasCapturedPayload() ) {
        return false;
    }
    auto operation = std::make_unique<MatchSelection>();
    operation->result = std::make_shared<Selection>( selection_ );
    operation->presentation = matchSelectionPresentation_;
    operation->serializing = true;
    operation->publishSelection = false;
    operation->withLineNumbers = withLineNumbers;
    matchSelection_ = std::move( operation );
    showMatchSelectionProgress();
    matchSelection_->progress->setLabelText( tr( "Copying match fragments..." ) );
    const auto generation = ++matchSelectionGeneration_;
    QTimer::singleShot( 0, this, [ this, generation ] { continueMatchSelection( generation ); } );
    return true;
}

bool AbstractLogView::deferCopyUntilMatchSelection()
{
    return serializeMatchSelection( false );
}

void AbstractLogView::cancelMatchSelection()
{
    matchSelectionPresentation_.reset();
    finishMatchSelection( false );
}

void AbstractLogView::invalidateMatchSelection()
{
    quickFind_->incrementalSearchAbort();
    clearCurrentMatchSelection();
}

void AbstractLogView::clearCurrentMatchSelection()
{
    acceptQuickFindResults_ = false;
    cancelMatchSelection();
    if ( selection_.isMultiPortion() || selection_.hasStableNavigationAnchor() ) {
        selection_.clear();
        forceRefresh();
    }
}
