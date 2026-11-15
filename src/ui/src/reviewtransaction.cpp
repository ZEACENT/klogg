#include "reviewtransaction.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <QThread>

ReviewTransaction::Labels* ReviewTransaction::captureLabels( ColorLabelsController& owner )
{
    Q_ASSERT( QThread::currentThread() == owner.thread() );
    if ( status_ != Status::Open ) {
        return nullptr;
    }
    const auto found = std::find_if( labels_.begin(), labels_.end(),
                                    [ &owner ]( const auto& entry ) {
                                        return entry.owner == &owner;
                                    } );
    if ( found != labels_.end() ) {
        return &*found;
    }
    if ( labels_.size() + filters_.size() >= MaxOwners ) {
        finish( Status::ResourceLimit );
        return nullptr;
    }
    labels_.push_back( { &owner, owner.revision_, owner.manager_ } );
    return &labels_.back();
}

std::optional<ColorLabelsController::Snapshot> ReviewTransaction::labels(
    ColorLabelsController& owner )
{
    const auto* entry = captureLabels( owner );
    if ( !entry ) {
        return {};
    }
    return ColorLabelsController::Snapshot{ entry->revision, entry->candidate.colorLabels() };
}

std::optional<SearchToolbar::FilterDraftSnapshot> ReviewTransaction::filterDraft(
    SearchToolbar& owner )
{
    Q_ASSERT( QThread::currentThread() == owner.thread() );
    if ( status_ != Status::Open ) {
        return {};
    }
    const auto found = std::find_if( filters_.cbegin(), filters_.cend(),
                                    [ &owner ]( const auto& entry ) {
                                        return entry.owner == &owner;
                                    } );
    if ( found != filters_.cend() ) {
        return found->snapshot;
    }
    if ( labels_.size() + filters_.size() >= MaxOwners ) {
        finish( Status::ResourceLimit );
        return {};
    }
    const auto snapshot = owner.filterDraftSnapshot();
    filters_.push_back( { &owner, snapshot } );
    return snapshot;
}

ReviewTransaction::Status ReviewTransaction::stageLabels(
    ColorLabelsController& owner, const std::vector<ColorLabelsController::Edit>& edits )
{
    auto* entry = captureLabels( owner );
    if ( !entry ) {
        return status_;
    }
    if ( edits.size() > ColorLabelsController::MaxEdits - commandCount_ ) {
        return finish( Status::ResourceLimit );
    }
    auto nextTextUnits = textUnits_;
    auto nextTextCount = textCount_;
    if ( const auto error = ColorLabelsController::validateEdits(
             edits, entry->candidate.colorLabels().size(), nextTextUnits, nextTextCount ) ) {
        return finish( *error == ColorLabelsController::EditResult::ResourceLimit
                           ? Status::ResourceLimit : Status::InvalidArgument );
    }
    auto candidate = entry->candidate;
    for ( const auto& edit : edits ) {
        ColorLabelsController::applyEdit( candidate, edit );
    }
    entry->candidate = std::move( candidate );
    commandCount_ += edits.size();
    textUnits_ = nextTextUnits;
    textCount_ = nextTextCount;
    return status_;
}

ReviewTransaction::Status ReviewTransaction::commit()
{
    if ( status_ != Status::Open ) {
        return status_;
    }
    // No event-loop turn or user callback is allowed between this validation
    // and installation. New/removed views are resolved during preparation below.
    for ( const auto& entry : filters_ ) {
        if ( !entry.owner ) {
            return finish( Status::TargetClosed );
        }
        Q_ASSERT( QThread::currentThread() == entry.owner->thread() );
        if ( entry.owner->filterDraftSnapshot().revision != entry.snapshot.revision ) {
            return finish( Status::Conflict );
        }
    }
    for ( const auto& entry : labels_ ) {
        if ( !entry.owner ) {
            return finish( Status::TargetClosed );
        }
        Q_ASSERT( QThread::currentThread() == entry.owner->thread() );
        if ( entry.owner->revision_ != entry.revision ) {
            return finish( Status::Conflict );
        }
        if ( entry.revision == std::numeric_limits<uint64_t>::max()
             && !entry.owner->manager_.hasSameState( entry.candidate ) ) {
            return finish( Status::ResourceLimit );
        }
    }

    using Prepared = ColorLabelsController::PreparedPublication;
    std::vector<std::pair<QPointer<ColorLabelsController>, Prepared>> prepared;
    std::vector<QPointer<ColorLabelsController>> notifications;
    prepared.reserve( labels_.size() );
    notifications.reserve( labels_.size() );
    for ( const auto& entry : labels_ ) {
        auto publication = entry.owner->preparePublication( entry.candidate );
        if ( publication.changed ) {
            notifications.push_back( entry.owner );
        }
        prepared.emplace_back( entry.owner, std::move( publication ) );
    }

    for ( auto& entry : prepared ) {
        entry.first->install( std::move( entry.second ) );
    }
    // Terminal before callbacks: reentrant commit/abort cannot replay or undo
    // this transaction. Dependency snapshots can now release their references.
    finish( Status::Committed );
    for ( const auto& owner : notifications ) {
        if ( owner ) {
            Q_EMIT owner->labelsChanged();
        }
    }
    return Status::Committed;
}

ReviewTransaction::Status ReviewTransaction::finish( Status status )
{
    status_ = status;
    labels_.clear();
    filters_.clear();
    return status_;
}

ReviewTransaction::Status ReviewTransaction::abort()
{
    return status_ == Status::Open ? finish( Status::Aborted ) : status_;
}
