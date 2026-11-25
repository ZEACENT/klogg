#include "logfiltereddata.h"

#include <QPointer>
#include <QThread>
#include <type_traits>
#include <utility>

struct LogFilteredData::SearchUpdateCapture {
    QString ownerId;
    quint64 generation = 0;
    uint64_t marksRevision = 0;
    SearchResultArray marks;
    Visibility visibility;
    Visibility resultVisibility;
    bool allVisible = false;
    bool reset = false;
    bool resultAllVisible = false;
    int before = 0;
    int after = 0;
    FullSearchPreparationRequest request;
};
struct LogFilteredData::PreparedSearchUpdate {
    std::shared_ptr<const SearchUpdateCapture> captured;
    LogFilteredDataWorker::PreparedResult result;
    RegularExpressionPattern pattern;
    SearchResultArray matches;
    SearchResultArray combined;
    klogg::vector<LineNumber> context;
    bool contextValid = false;
    bool consumed = false;
};
struct LogFilteredData::SearchUpdatePublication::State {
    QPointer<LogFilteredData> owner;
    std::shared_ptr<PreparedSearchUpdate> prepared;
    CompletedSearchResult::Ptr installedResult;
    quint64 generation = 0;
    quint64 requestTicket = 0;
    bool installed = false;
    bool released = false;
    bool notified = false;
};

std::shared_ptr<const LogFilteredData::SearchUpdateCapture> LogFilteredData::captureSearchUpdate(
    const RegularExpressionPattern& pattern, LineNumber startLine,
    std::optional<LineNumber> endLine ) const
{
    return captureSearchUpdate( pattern, visibility_, startLine, endLine );
}

std::shared_ptr<LogFilteredData::SearchUpdateCapture> LogFilteredData::captureSearchUpdateState() const
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( shuttingDown_ || marksPublicationLocked_ || !sourceLogData_
         || marks_.getSizeInBytes() > MaxPreparedBitmapBytes ) return {};
    auto captured = std::make_shared<SearchUpdateCapture>();
    captured->ownerId = reviewMarksId();
    captured->generation = currentSearchGeneration();
    captured->marksRevision = marksRevision_;
    captured->marks = marks_;
    captured->visibility = visibility_;
    captured->resultVisibility = visibility_;
    captured->allVisible = allLinesVisible_;
    captured->before = contextLinesBefore_;
    captured->after = contextLinesAfter_;
    captured->request.source = sourceLogData_->searchableSnapshot();
    captured->request.settings = SearchSettings::capture();
    if ( !captured->request.source ) return {};
    return captured;
}

std::shared_ptr<const LogFilteredData::SearchUpdateCapture> LogFilteredData::captureSearchUpdate(
    const RegularExpressionPattern& pattern, Visibility resultVisibility, LineNumber startLine,
    std::optional<LineNumber> endLine ) const
{
    auto captured = captureSearchUpdateState();
    if ( !captured ) return {};
    captured->request.pattern = pattern;
    captured->request.startLine = startLine;
    captured->request.endLine = endLine;
    captured->resultVisibility = resultVisibility;
    return captured;
}

std::shared_ptr<const LogFilteredData::SearchUpdateCapture> LogFilteredData::captureSearchReset(
    bool allLinesVisible, Visibility resultVisibility ) const
{
    auto captured = captureSearchUpdateState();
    if ( !captured ) return {};
    captured->reset = true;
    captured->resultAllVisible = allLinesVisible;
    captured->resultVisibility = resultVisibility;
    return captured;
}

LogFilteredData::SearchUpdatePreparation LogFilteredData::prepareSearchUpdate(
    const std::shared_ptr<const SearchUpdateCapture>& captured, AtomicFlag& cancelled )
{
    using Status = SearchPreparation::Status;
    if ( !captured ) return {};
    if ( cancelled ) return { Status::Cancelled, {} };
    const auto searched = captured->reset ? SearchPreparation{ Status::Ready, {} }
                                         : prepareFullSearch( captured->request, cancelled );
    if ( searched.status != Status::Ready ) return { searched.status, {} };
    try {
        const SearchResultArray emptyMatches;
        const auto& matches = captured->reset ? emptyMatches : searched.result->matches;
        const auto markBytes = captured->marks.getSizeInBytes();
        if ( matches.getSizeInBytes() > MaxPreparedBitmapBytes - markBytes ) return { Status::Failed, {} };
        const bool context = ( captured->before != 0 || captured->after != 0 )
            && !( captured->resultAllVisible && captured->resultVisibility.testFlag( VisibilityFlags::Matches ) );
        if ( context ) {
            const auto expansion = uint64_t{ 1 } + static_cast<uint64_t>( captured->before )
                + static_cast<uint64_t>( captured->after );
            auto availableSeeds = MaxPreparedContextRows / expansion;
            for ( const auto flag : { VisibilityFlags::Matches, VisibilityFlags::Marks } ) {
                if ( !captured->resultVisibility.testFlag( flag ) ) continue;
                const auto& seeds = flag == VisibilityFlags::Matches ? matches : captured->marks;
                if ( seeds.cardinality() > availableSeeds ) return { Status::Failed, {} };
                availableSeeds -= seeds.cardinality();
            }
        }
        auto prepared = std::make_shared<PreparedSearchUpdate>();
        prepared->captured = captured;
        // Stamp a private copy only at publication; retained preparation results
        // and their source/compiled query remain immutable to other observers.
        if ( !captured->reset ) prepared->result = LogFilteredDataWorker::PreparedResult{ *searched.result };
        prepared->contextValid = context;
        prepared->pattern = captured->request.pattern;
        prepared->matches = matches;
        prepared->combined = matches | captured->marks;
        if ( context ) {
            prepared->context = buildContextLines( matches, captured->marks,
                captured->resultVisibility, captured->before, captured->after,
                captured->request.source->lineCount() );
        }
        if ( cancelled ) return { Status::Cancelled, {} };
        return { Status::Ready, std::move( prepared ) };
    } catch ( ... ) {
        return { Status::Failed, {} };
    }
}

bool LogFilteredData::isSearchUpdateCurrent( const PreparedSearchUpdate& prepared ) const
{
    if ( shuttingDown_ || prepared.consumed || !sourceLogData_
         || ( !prepared.captured->reset && !prepared.result ) ) return false;
    const auto& captured = *prepared.captured;
    if ( captured.ownerId != reviewMarksId() || captured.generation != currentSearchGeneration()
         || captured.marksRevision != marksRevision_ || captured.visibility != visibility_
         || captured.allVisible != allLinesVisible_ || captured.before != contextLinesBefore_
         || captured.after != contextLinesAfter_ ) return false;
    const auto source = sourceLogData_->searchableSnapshot();
    if ( !source ) return false;
    const auto key = source->cacheKey();
    // A prepared full query belongs to an exact cutoff. Unlike a mark edit,
    // append is relevant and must be retried rather than silently omitted.
    return source == captured.request.source
        || ( key && key == captured.request.source->cacheKey() );
}

std::unique_ptr<LogFilteredData::SearchUpdatePublication> LogFilteredData::lockSearchUpdate(
    const std::shared_ptr<PreparedSearchUpdate>& prepared )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( !prepared || marksPublicationLocked_ || !isSearchUpdateCurrent( *prepared ) ) return {};
    auto state = std::make_unique<SearchUpdatePublication::State>();
    state->owner = this;
    state->prepared = prepared;
    auto publication = std::unique_ptr<SearchUpdatePublication>(
        new SearchUpdatePublication( std::move( state ) ) );
    marksPublicationLocked_ = true;
    return publication;
}

LogFilteredData::SearchUpdatePublication::SearchUpdatePublication( std::unique_ptr<State> state )
    : state_( std::move( state ) ) {}
LogFilteredData::SearchUpdatePublication::~SearchUpdatePublication() { release(); }

bool LogFilteredData::SearchUpdatePublication::install()
{
    static_assert( std::is_nothrow_move_assignable_v<SearchResultArray>,
                   "Search publication requires allocation-free bitmap moves" );
    static_assert( std::is_nothrow_move_assignable_v<klogg::vector<LineNumber>>,
                   "Search publication requires allocation-free context moves" );
    if ( !state_->owner || state_->released || state_->installed ) return false;
    auto& owner = *state_->owner;
    auto& prepared = *state_->prepared;
    Q_ASSERT( QThread::currentThread() == owner.thread() );
    if ( !owner.isSearchUpdateCurrent( prepared ) ) return false;
    const auto& captured = *prepared.captured;
    if ( captured.reset ) owner.workerThread_.bumpGeneration();
    else if ( !owner.workerThread_.adoptPreparedResult( std::move( prepared.result ) ) ) return false;
    owner.cancelPendingPublications();
    owner.completedResult_ = owner.workerThread_.completedSearchResult();
    owner.presentedSource_ = captured.request.source;
    owner.completedSourceInvalid_ = false;
    owner.currentRegExp_ = std::move( prepared.pattern );
    const auto& request = prepared.captured->request;
    owner.currentSearchKey_ = captured.reset ? SearchCacheKey{}
        : owner.makeCacheKey( owner.currentRegExp_, request.startLine,
            request.endLine.value_or( LineNumber{ request.source->lineCount().get() } ) );
    owner.matching_lines_ = std::move( prepared.matches );
    owner.marks_and_matches_ = std::move( prepared.combined );
    owner.allLinesVisible_ = captured.resultAllVisible;
    owner.visibility_ = captured.resultVisibility;
    owner.maxLength_ = owner.completedResult_ ? owner.completedResult_->maxLength : 0_length;
    owner.nbLinesProcessed_ = owner.completedResult_ ? owner.completedResult_->processedLines : 0_lcount;
    owner.contextLinesList_ = std::move( prepared.context );
    owner.contextLinesListValid_ = prepared.contextValid;
    owner.maxLengthContext_.reset();
    owner.maxLengthMarks_ = owner.marks_.isEmpty() ? std::optional<LineLength>{ 0_length } : std::nullopt;
    owner.longestMark_.reset();
    owner.unmeasuredMarks_ = SearchResultArray{};
    state_->installedResult = owner.completedResult_;
    state_->generation = owner.currentSearchGeneration();
    state_->requestTicket = owner.workerThread_.currentRequestTicket();
    prepared.consumed = true;
    state_->installed = true;
    return true;
}

bool LogFilteredData::SearchUpdatePublication::isCurrent() const
{
    return state_->installed && state_->owner && !state_->owner->shuttingDown_
        && state_->owner->currentSearchGeneration() == state_->generation
        && state_->owner->workerThread_.currentRequestTicket() == state_->requestTicket
        && state_->owner->completedSearchResult() == state_->installedResult;
}

void LogFilteredData::SearchUpdatePublication::release()
{
    if ( state_->released ) return;
    state_->released = true;
    if ( state_->owner ) state_->owner->marksPublicationLocked_ = false;
}

void LogFilteredData::SearchUpdatePublication::notify()
{
    if ( !state_->installed || !state_->released || state_->notified || !state_->owner ) return;
    state_->notified = true;
    const auto owner = state_->owner;
    const auto result = state_->installedResult;
    // Another owner's observer may already have requested a native update. Its
    // query generation is unchanged, but this completed cutoff is superseded.
    if ( !isCurrent() ) return;
    const auto generation = state_->generation;
    const auto startLine = state_->prepared->captured->request.startLine;
    // No publication state access after observers can destroy this guard.
    if ( state_->prepared->captured->reset ) {
        Q_EMIT owner->searchResultsChanged( 0_lcount, startLine, false, generation );
    } else {
        owner->publishTerminal( LinesCount{ result->matches.cardinality() }, startLine, generation );
    }
}
