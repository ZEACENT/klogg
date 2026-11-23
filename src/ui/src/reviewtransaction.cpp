#include "reviewtransaction.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <new>
#include <utility>

#include <QCoreApplication>
#include <QThread>
#include "ownedtaskfuture.h"

#include "asyncworkbudget.h"
#include "configuration.h"

namespace {
using Status = ReviewTransaction::Status;
using Preparation = ReviewTransaction::Preparation;
using UpdateResult = HighlighterSetCollection::UpdateResult;

Status stageFailure( Status status )
{
    switch ( status ) {
    case Status::Open: case Status::Preparing: case Status::Committed: return Status::PreparationFailed;
    default: return status;
    }
}

Status updateFailure( UpdateResult error )
{
    switch ( error ) {
    case UpdateResult::Conflict: return Status::Conflict;
    case UpdateResult::ResourceLimit: return Status::ResourceLimit;
    case UpdateResult::InvalidArgument: return Status::InvalidArgument;
    default: return Status::PreparationFailed;
    }
}

klogg::AsyncWorkBudget::Lease acquirePreparationPermit()
{
    static const klogg::AsyncWorkBudget budget{ ReviewTransaction::MaxConcurrentPreparations };
    return budget.tryAcquire();
}

ReviewTransaction::PreparationTask prepareReview( ReviewTransaction::PreparationInput captured )
{
    return klogg::runSingleOwnerTask( [input = std::move( captured )]() mutable {
        Preparation outcome;
        // Completed-but-unpublished projections retain admission as well as
        // queued/running work. Cancellation never frees a still-live worker.
        outcome.permit = std::move( input.permit );
        try {
            if ( *input.cancelled ) { outcome.failure = Status::Aborted; return outcome; }
            if ( input.highlighters ) {
                outcome.update = std::make_shared<HighlighterSetCollection::PreparedUpdate>(
                    HighlighterSetCollection::prepareUpdate( std::move( *input.highlighters ), input.engine ) );
                if ( const auto error = outcome.update->error() ) {
                    outcome.failure = updateFailure( *error );
                    return outcome;
                }
            }
            for ( const auto& prepare : input.marks ) {
                if ( *input.cancelled ) { outcome.failure = Status::Aborted; return outcome; }
                auto marks = prepare( *input.cancelled );
                if ( !marks ) {
                    outcome.failure = *input.cancelled ? Status::Aborted : Status::PreparationFailed;
                    return outcome;
                }
                outcome.marks.push_back( std::move( marks ) );
            }
            outcome.failure = *input.cancelled ? Status::Aborted : Status::Open;
        }
        catch ( const std::bad_alloc& ) { outcome.failure = Status::ResourceLimit; }
        catch ( ... ) { outcome.failure = Status::PreparationFailed; }
        return outcome;
    } );
}
}

ReviewTransaction::ReviewTransaction( Preparer prepare )
    : preparer_( prepare ? std::move( prepare ) : prepareReview )
{
}
ReviewTransaction::~ReviewTransaction() { abort(); }

size_t ReviewTransaction::ownerCount() const
{
    return labels_.size() + filters_.size() + marks_.size() + ( highlighters_ ? 1U : 0U ) + ( participant_ ? 1U : 0U );
}

ReviewTransaction::Status ReviewTransaction::stageParticipant( Participant participant )
{
    if ( status_ != Status::Open ) return status_;
    if ( !participant.validate || !participant.install || !participant.release || !participant.notify )
        return finish( Status::InvalidArgument );
    if ( participant_ || ownerCount() >= MaxOwners || commandCount_ >= ColorLabelsController::MaxEdits )
        return finish( Status::ResourceLimit );
    participant_ = std::move( participant );
    ++commandCount_;
    return Status::Open;
}

ReviewTransaction::Status ReviewTransaction::beginAsyncStage( std::function<void()> cancel )
{
    if ( status_ != Status::Open ) return status_;
    if ( asyncStagePending_ ) return Status::ResourceLimit;
    // Private filters may append kept panes without adding visible participants.
    // Count those stages too, so one private document has bounded command growth.
    if ( commandCount_ >= ColorLabelsController::MaxEdits ) return finish( Status::ResourceLimit );
    ++commandCount_;
    asyncStagePending_ = true;
    cancelAsyncStage_ = std::move( cancel );
    return Status::Open;
}

void ReviewTransaction::finishAsyncStage()
{
    asyncStagePending_ = false;
    cancelAsyncStage_ = {};
}

void ReviewTransaction::failAsyncStage( Status failure )
{
    if ( asyncStagePending_ ) finish( stageFailure( failure ) );
}

std::optional<klogg::ReviewMarksSnapshot> ReviewTransaction::marks( klogg::ReviewMarksOwner& owner )
{
    const auto* entry = captureMarks( owner );
    return entry ? std::optional<klogg::ReviewMarksSnapshot>{ entry->candidate } : std::nullopt;
}

ReviewTransaction::Marks* ReviewTransaction::captureMarks( klogg::ReviewMarksOwner& owner )
{
    if ( status_ != Status::Open ) return {};
    auto* lifetime = owner.reviewMarksObject();
    if ( !lifetime ) {
        finish( Status::TargetClosed );
        return {};
    }
    Q_ASSERT( QThread::currentThread() == lifetime->thread() );
    const auto found = std::find_if( marks_.begin(), marks_.end(), [&]( const auto& entry ) {
        return entry.owner == &owner && entry.lifetime == lifetime;
    } );
    if ( found != marks_.end() ) return &*found;
    if ( ownerCount() >= MaxOwners ) {
        finish( Status::ResourceLimit );
        return {};
    }
    const auto snapshot = owner.reviewMarksSnapshot();
    if ( !snapshot ) {
        finish( Status::ResourceLimit );
        return {};
    }
    marks_.push_back( { &owner, lifetime, *snapshot, *snapshot, {}, {} } );
    return &marks_.back();
}

ReviewTransaction::Status ReviewTransaction::stageMarks(
    klogg::ReviewMarksOwner& owner, const MarkEdit& edit, bool privateOwner )
{
    if ( status_ != Status::Open ) return status_;
    if ( privateOwner && !participant_ ) return finish( Status::InvalidArgument );
    using Kind = MarkEdit::Kind;
    switch ( edit.kind ) {
    case Kind::Add: case Kind::Remove:
        if ( edit.rows.empty() ) return finish( Status::InvalidArgument );
        break;
    case Kind::Clear:
        if ( !edit.rows.empty() ) return finish( Status::InvalidArgument );
        break;
    case Kind::Replace: break;
    default: return finish( Status::InvalidArgument );
    }
    if ( commandCount_ >= ColorLabelsController::MaxEdits || edit.rows.size() > klogg::ReviewMarksOwner::MaxMarks )
        return finish( Status::ResourceLimit );
    try {
        auto* entry = captureMarks( owner );
        if ( !entry ) return status_;
        if ( entry->privateOwner && *entry->privateOwner != privateOwner ) return finish( Status::InvalidArgument );
        entry->privateOwner = privateOwner;
        std::map<QString, std::set<uint64_t>> groups;
        if ( edit.kind == Kind::Add || edit.kind == Kind::Remove ) {
            for ( const auto& group : entry->candidate.groups )
                for ( const auto row : group.rows ) groups[ group.memberPath ].insert( row.get() );
        }
        std::vector<Marks::Proof> proofs;
        auto proofText = markProofTextUnits_;
        for ( const auto& identity : edit.rows ) {
            const auto location = owner.resolveReviewMark( identity );
            if ( !location ) return finish( Status::Conflict );
            const bool sourceModel = entry->snapshot.scope == klogg::ReviewMarksSnapshot::Scope::SourceModel;
            if ( sourceModel != location->memberPath.isEmpty() ) return finish( Status::InvalidArgument );
            const auto sameIdentity = [&]( const auto& proof ) { return proof.identity == identity; };
            const auto existing = std::find_if( entry->proofs.cbegin(), entry->proofs.cend(), sameIdentity );
            const auto staged = std::find_if( proofs.cbegin(), proofs.cend(), sameIdentity );
            if ( existing != entry->proofs.cend() ) {
                if ( existing->location != *location ) return finish( Status::Conflict );
            }
            else if ( staged != proofs.cend() ) {
                if ( staged->location != *location ) return finish( Status::Conflict );
            }
            else {
                const auto text = static_cast<size_t>( location->memberPath.size() );
                if ( proofs.size() >= klogg::ReviewMarksOwner::MaxMarks - markProofCount_
                     || text > klogg::ReviewMarksOwner::MaxTextUnits - proofText ) return finish( Status::ResourceLimit );
                proofs.push_back( { identity, *location } );
                proofText += text;
            }
            if ( edit.kind == Kind::Remove ) {
                const auto found = groups.find( location->memberPath );
                if ( found != groups.end() ) {
                    found->second.erase( location->row.get() );
                    if ( found->second.empty() ) groups.erase( found );
                }
            }
            else groups[ location->memberPath ].insert( location->row.get() );
        }
        if ( groups.size() > klogg::ReviewMarksOwner::MaxMembers ) return finish( Status::ResourceLimit );
        size_t rows = 0;
        size_t text = 0;
        auto candidate = entry->candidate;
        candidate.groups.clear();
        for ( const auto& group : groups ) {
            if ( group.second.size() > klogg::ReviewMarksOwner::MaxMarks - rows
                 || static_cast<size_t>( group.first.size() ) > klogg::ReviewMarksOwner::MaxTextUnits - text )
                return finish( Status::ResourceLimit );
            rows += group.second.size();
            text += static_cast<size_t>( group.first.size() );
            klogg::ReviewMarksSnapshot::Group value{ group.first, {} };
            for ( const auto row : group.second ) value.rows.append( LineNumber{ row } );
            candidate.groups.push_back( std::move( value ) );
        }
        entry->candidate = std::move( candidate );
        entry->proofs.insert( entry->proofs.end(), proofs.begin(), proofs.end() );
        markProofCount_ += proofs.size();
        markProofTextUnits_ = proofText;
        ++commandCount_;
        return status_;
    }
    catch ( const std::bad_alloc& ) { return finish( Status::ResourceLimit ); }
    catch ( ... ) { return finish( Status::PreparationFailed ); }
}

ReviewTransaction::Highlighters* ReviewTransaction::captureHighlighters()
{
    Q_ASSERT( !QCoreApplication::instance()
              || QThread::currentThread() == QCoreApplication::instance()->thread() );
    if ( status_ != Status::Open ) return {};
    if ( !highlighters_ ) {
        if ( ownerCount() >= MaxOwners ) {
            finish( Status::ResourceLimit );
            return {};
        }
        auto base = HighlighterSetCollection::get().boundedSnapshot();
        if ( !base ) {
            finish( Status::ResourceLimit );
            return {};
        }
        highlighters_ = Highlighters{ *base, *base };
    }
    return &*highlighters_;
}

std::optional<HighlighterSetCollection::Snapshot> ReviewTransaction::highlighters()
{
    const auto* entry = captureHighlighters();
    if ( !entry ) return {};
    return entry->candidate;
}

ReviewTransaction::Status ReviewTransaction::stageHighlighters(
    QList<HighlighterSet> sets, QStringList activeSetIds )
{
    auto* entry = captureHighlighters();
    if ( !entry ) return status_;
    if ( commandCount_ >= ColorLabelsController::MaxEdits ) return finish( Status::ResourceLimit );
    auto candidate = entry->candidate;
    candidate.sets = std::move( sets );
    candidate.activeSetIds = std::move( activeSetIds );
    if ( const auto error = HighlighterSetCollection::validateUpdate( candidate ) )
        return finish( updateFailure( *error ) );
    // Staged arguments may themselves come from live GUI sets rather than a
    // captured snapshot. Detach cache-owning rule objects before any worker.
    for ( auto& set : candidate.sets ) {
        auto rules = set.highlighters();
        rules.detach();
        set = HighlighterSet::fromDescription( set.id(), set.name(), std::move( rules ) );
    }
    entry->candidate = std::move( candidate );
    ++commandCount_;
    return status_;
}

ReviewTransaction::Labels* ReviewTransaction::captureLabels( ColorLabelsController& owner )
{
    Q_ASSERT( QThread::currentThread() == owner.thread() );
    if ( status_ != Status::Open ) return nullptr;
    const auto found = std::find_if( labels_.begin(), labels_.end(),
                                    [ &owner ]( const auto& entry ) { return entry.owner == &owner; } );
    if ( found != labels_.end() ) return &*found;
    if ( ownerCount() >= MaxOwners ) {
        finish( Status::ResourceLimit );
        return nullptr;
    }
    labels_.push_back( { &owner, owner.revision_, owner.manager_ } );
    return &labels_.back();
}

std::optional<ColorLabelsController::Snapshot> ReviewTransaction::labels( ColorLabelsController& owner )
{
    const auto* entry = captureLabels( owner );
    if ( !entry ) return {};
    return ColorLabelsController::Snapshot{ entry->revision, entry->candidate.colorLabels() };
}

ReviewTransaction::Filter* ReviewTransaction::captureFilter( SearchToolbar& owner )
{
    Q_ASSERT( QThread::currentThread() == owner.thread() );
    if ( status_ != Status::Open ) return nullptr;
    const auto found = std::find_if( filters_.begin(), filters_.end(),
                                    [ &owner ]( const auto& entry ) { return entry.owner == &owner; } );
    if ( found != filters_.end() ) return &*found;
    if ( ownerCount() >= MaxOwners ) {
        finish( Status::ResourceLimit );
        return nullptr;
    }
    filters_.push_back( { &owner, owner.filterDraftSnapshot() } );
    return &filters_.back();
}

std::optional<SearchToolbar::FilterDraftSnapshot> ReviewTransaction::filterDraft( SearchToolbar& owner )
{
    const auto* entry = captureFilter( owner );
    if ( !entry ) return {};
    return entry->snapshot;
}

ReviewTransaction::Status ReviewTransaction::stageFilterDraft(
    SearchToolbar& owner, const SearchToolbar::FilterSpec& candidate )
{
    if ( status_ != Status::Open ) return status_;
    if ( !participant_ ) return finish( Status::InvalidArgument );
    if ( commandCount_ >= ColorLabelsController::MaxEdits ) return finish( Status::ResourceLimit );
    auto* entry = captureFilter( owner );
    if ( !entry ) return status_;
    entry->snapshot.filter = candidate; // Keep the captured revision for final validation.
    ++commandCount_;
    return status_;
}

ReviewTransaction::Status ReviewTransaction::stageLabels(
    ColorLabelsController& owner, const std::vector<ColorLabelsController::Edit>& edits )
{
    auto* entry = captureLabels( owner );
    if ( !entry ) return status_;
    if ( edits.size() > ColorLabelsController::MaxEdits - commandCount_ ) return finish( Status::ResourceLimit );
    auto nextTextUnits = textUnits_;
    auto nextTextCount = textCount_;
    if ( const auto error = ColorLabelsController::validateEdits(
             edits, entry->candidate.colorLabels().size(), nextTextUnits, nextTextCount ) ) {
        return finish( *error == ColorLabelsController::EditResult::ResourceLimit
                           ? Status::ResourceLimit : Status::InvalidArgument );
    }
    auto candidate = entry->candidate;
    for ( const auto& edit : edits ) ColorLabelsController::applyEdit( candidate, edit );
    entry->candidate = std::move( candidate );
    commandCount_ += edits.size();
    textUnits_ = nextTextUnits;
    textCount_ = nextTextCount;
    return status_;
}

ReviewTransaction::Status ReviewTransaction::commit( const std::function<void()>& published )
{
    if ( asyncStagePending_ || publishing_ || ( status_ != Status::Open && status_ != Status::Preparing ) ) return status_;
    // Validate before starting costly work and again after it completes. No
    // event-loop turn or callback occurs between final validation and install.
    size_t changedMarks = 0;
    try {
        for ( const auto& entry : marks_ ) {
            if ( !entry.lifetime ) return finish( Status::TargetClosed );
            Q_ASSERT( QThread::currentThread() == entry.lifetime->thread() );
            if ( entry.owner->reviewMarksRevision() != entry.snapshot.revision ) return finish( Status::Conflict );
            for ( const auto& proof : entry.proofs ) {
                const auto current = entry.owner->resolveReviewMark( proof.identity );
                if ( !current || *current != proof.location ) return finish( Status::Conflict );
            }
            if ( entry.candidate.groups != entry.snapshot.groups ) ++changedMarks;
        }
    }
    catch ( const std::bad_alloc& ) { return finish( Status::ResourceLimit ); }
    catch ( ... ) { return finish( Status::PreparationFailed ); }
    for ( const auto& entry : filters_ ) {
        if ( !entry.owner ) return finish( Status::TargetClosed );
        Q_ASSERT( QThread::currentThread() == entry.owner->thread() );
        if ( entry.owner->filterDraftSnapshot().revision != entry.snapshot.revision ) return finish( Status::Conflict );
    }
    for ( const auto& entry : labels_ ) {
        if ( !entry.owner ) return finish( Status::TargetClosed );
        Q_ASSERT( QThread::currentThread() == entry.owner->thread() );
        if ( entry.owner->revision_ != entry.revision ) return finish( Status::Conflict );
        if ( entry.revision == std::numeric_limits<uint64_t>::max()
             && !entry.owner->manager_.hasSameState( entry.candidate ) ) return finish( Status::ResourceLimit );
    }

    bool changedHighlighters = false;
    if ( highlighters_ ) {
        const auto& global = HighlighterSetCollection::get();
        if ( global.revision() != highlighters_->base.revision ) return finish( Status::Conflict );
        changedHighlighters = highlighters_->candidate.sets != highlighters_->base.sets
            || highlighters_->candidate.activeSetIds != highlighters_->base.activeSetIds;
        if ( changedHighlighters && global.revision() == std::numeric_limits<uint64_t>::max() )
            return finish( Status::ResourceLimit );
    }
    // Keep admission until local projections and notification records retire,
    // including reentrant requests made by publication observers.
    klogg::AsyncWorkBudget::Lease publicationPermit;
    std::shared_ptr<HighlighterSetCollection::PreparedUpdate> highlighterUpdate;
    std::vector<std::shared_ptr<klogg::PreparedReviewMarks>> markUpdates;
    if ( changedHighlighters || changedMarks != 0 ) {
        if ( status_ == Status::Open ) {
            try {
                preparationPermit_ = acquirePreparationPermit();
                if ( !preparationPermit_ ) return finish( Status::ResourceLimit );
                preparationCancelled_ = std::make_shared<std::atomic<bool>>( false );
                PreparationInput input;
                input.cancelled = preparationCancelled_;
                input.permit = preparationPermit_;
                if ( changedHighlighters ) {
                    preparationEngine_ = Configuration::get().regexpEngine();
                    input.highlighters = highlighters_->candidate;
                    input.engine = *preparationEngine_;
                }
                klogg::ReviewMarksBudget budget;
                for ( const auto& entry : marks_ ) {
                    if ( entry.candidate.groups == entry.snapshot.groups ) continue;
                    auto prepare = entry.owner->prepareReviewMarks( entry.candidate, budget );
                    if ( !prepare ) return finish( Status::ResourceLimit );
                    input.marks.push_back( std::move( prepare ) );
                }
                preparation_ = preparer_( std::move( input ) );
            }
            catch ( const std::bad_alloc& ) { return finish( Status::ResourceLimit ); }
            catch ( ... ) { return finish( Status::PreparationFailed ); }
            if ( !preparation_ ) return finish( Status::ResourceLimit );
            status_ = Status::Preparing;
            return status_;
        }
        if ( changedHighlighters && Configuration::get().regexpEngine() != *preparationEngine_ )
            return finish( Status::Conflict );
        if ( !preparation_->finished.isFinished() ) return Status::Preparing;
        publicationPermit = preparationPermit_;
        try {
            auto& result = preparation_->result;
            if ( !result.valid() || result.wait_for( std::chrono::seconds::zero() ) != std::future_status::ready )
                return finish( Status::PreparationFailed );
            auto outcome = result.get();
            if ( outcome.failure != Status::Open ) {
                switch ( outcome.failure ) {
                case Status::InvalidArgument:
                case Status::ResourceLimit:
                case Status::Aborted: return finish( outcome.failure );
                default: return finish( Status::PreparationFailed );
                }
            }
            if ( static_cast<bool>( outcome.update ) != changedHighlighters || outcome.marks.size() != changedMarks
                 || std::any_of( outcome.marks.cbegin(), outcome.marks.cend(), []( const auto& update ) { return !update; } ) )
                return finish( Status::PreparationFailed );
            highlighterUpdate = std::move( outcome.update );
            markUpdates = std::move( outcome.marks );
        }
        catch ( const std::bad_alloc& ) { return finish( Status::ResourceLimit ); }
        catch ( ... ) { return finish( Status::PreparationFailed ); }
        if ( highlighterUpdate ) {
            if ( const auto error = highlighterUpdate->error() ) return finish( updateFailure( *error ) );
        }
    }

    using Prepared = ColorLabelsController::PreparedPublication;
    std::vector<std::pair<QPointer<ColorLabelsController>, Prepared>> prepared;
    std::vector<QPointer<ColorLabelsController>> notifications;
    struct MarkPublication {
        std::unique_ptr<klogg::ReviewMarksPublication> guard;
        bool privateOwner;
    };
    std::vector<MarkPublication> markPublications;
    // On failure destroy model guards before releasing the participant: the
    // latter may be the final owner of a disposable document and its models.
    const auto fail = [&]( Status reason ) {
        markPublications.clear();
        return finish( reason );
    };
    HighlighterSetCollection* global = nullptr;
    UpdateResult approval = UpdateResult::Unchanged;
    publishing_ = true;
    status_ = Status::Preparing;
    try {
        prepared.reserve( labels_.size() );
        notifications.reserve( labels_.size() );
        for ( const auto& entry : labels_ ) {
            auto publication = entry.owner->preparePublication( entry.candidate );
            if ( publication.changed ) notifications.push_back( entry.owner );
            prepared.emplace_back( entry.owner, std::move( publication ) );
        }
        if ( highlighterUpdate ) {
            if ( !highlighters_ ) return fail( Status::PreparationFailed );
            global = &HighlighterSetCollection::get();
            approval = global->preflightPrepared( *highlighterUpdate, highlighters_->base.revision );
            if ( approval != UpdateResult::Applied && approval != UpdateResult::Unchanged )
                return fail( updateFailure( approval ) );
        }
        if ( participant_ ) {
            const auto validation = participant_->validate();
            if ( validation != Status::Open ) return fail( stageFailure( validation ) );
        }
        markPublications.reserve( markUpdates.size() );
        for ( const auto& entry : marks_ ) {
            if ( entry.candidate.groups == entry.snapshot.groups ) continue;
            if ( !entry.privateOwner ) return fail( Status::PreparationFailed );
            auto guard = markUpdates[ markPublications.size() ]->acquire();
            if ( !guard ) return fail( Status::Conflict );
            markPublications.push_back( { std::move( guard ), *entry.privateOwner } );
        }
        // Only disposable, unexposed models may change before fallible adoption.
        // Inserting a tab can read its projection synchronously, so unlock first.
        for ( const auto& publication : markPublications ) {
            if ( publication.privateOwner ) {
                publication.guard->install();
                publication.guard->release();
            }
        }
        if ( participant_ && !participant_->install() ) return fail( Status::Conflict );
    }
    catch ( const std::bad_alloc& ) { return fail( Status::ResourceLimit ); }
    catch ( ... ) { return fail( Status::PreparationFailed ); }
    // All fallible decisions precede the first visible publication. The rest is
    // installation of preapproved state, then guard release and notifications.
    if ( highlighterUpdate ) {
        std::move( *highlighterUpdate ).installApproved( *global, approval );
        highlightersCommitted_ = approval == UpdateResult::Applied;
    }
    for ( auto& entry : prepared ) entry.first->install( std::move( entry.second ) );
    for ( const auto& publication : markPublications ) {
        if ( !publication.privateOwner ) publication.guard->install();
    }
    for ( const auto& publication : markPublications ) {
        if ( !publication.privateOwner ) publication.guard->release();
    }
    // Retain the participant's private payload and admission through observers,
    // just like publicationPermit above. Release native guards, not ownership.
    auto participant = std::exchange( participant_, {} );
    finish( Status::Committed );
    if ( participant ) participant->release();
    if ( published ) published(); // Complete the operation before native observers can reenter.
    if ( participant ) participant->notify();
    for ( const auto& owner : notifications ) {
        if ( owner ) Q_EMIT owner->labelsChanged();
    }
    for ( const auto& publication : markPublications ) publication.guard->notify();
    return Status::Committed;
}

ReviewTransaction::Status ReviewTransaction::finish( Status status )
{
    status_ = status;
    publishing_ = false;
    asyncStagePending_ = false;
    auto cancel = std::exchange( cancelAsyncStage_, {} );
    if ( cancel ) {
        try { cancel(); }
        catch ( ... ) { qWarning( "Review stage cancellation callback failed" ); }
    }
    if ( status != Status::Committed && preparationCancelled_ ) *preparationCancelled_ = true;
    preparation_.reset();
    preparationCancelled_.reset();
    preparationEngine_.reset();
    preparationPermit_.reset();
    labels_.clear();
    filters_.clear();
    marks_.clear();
    highlighters_.reset();
    if ( participant_ ) {
        try { participant_->release(); }
        catch ( ... ) { qWarning( "Review participant cleanup callback failed" ); }
        participant_.reset();
    }
    return status_;
}

ReviewTransaction::Status ReviewTransaction::abort()
{
    if ( publishing_ ) return status_;
    return status_ == Status::Open || status_ == Status::Preparing ? finish( Status::Aborted ) : status_;
}
