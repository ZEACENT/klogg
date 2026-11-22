#include "crawlerwidget.h"

#include <QPointer>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QThread>
#include <utility>

#include "infoline.h"
#include "configuration.h"
#include "emptyfilterpolicy.h"

struct CrawlerWidget::FilterUpdateCapture {
    QString tabId;
    QString mainPaneId;
    QString resultsPaneId;
    SearchToolbar::FilterDraftSnapshot draft;
    SearchToolbar::FilterSpec candidate;
    RegularExpressionPattern appliedPattern;
    SearchState::State searchState = SearchState::NoSearch;
    LineNumber startLine = 0_lnum;
    LineNumber endLine = 0_lnum;
    quint64 mainSelectionRevision = 0;
    quint64 resultsSelectionRevision = 0;
    bool revealMatches = false;
    bool showAllWhenEmpty = false;
    bool newPane = false;
    QString resultsOwnerId;
    quint64 oldQueryGeneration = 0;
    LogFilteredData::Visibility oldVisibility;
    int contextMode = 0;
    int contextValue = 0;
    std::shared_ptr<const LogFilteredData::SearchUpdateCapture> model;
};
struct CrawlerWidget::PreparedFilterUpdate {
    std::shared_ptr<const FilterUpdateCapture> captured;
    std::shared_ptr<LogFilteredData::PreparedSearchUpdate> model;
    bool consumed = false;
};
struct CrawlerWidget::FilterUpdateStaging::State {
    QThread* const thread = QThread::currentThread();
    QPointer<CrawlerWidget> owner;
    std::shared_ptr<const FilterUpdateCapture> captured;
    // Destruction order: stop/delete the private view before model, pattern and
    // source. These resources stay entirely on the GUI delivery side.
    std::shared_ptr<SearchableLogData> source;
    std::shared_ptr<QuickFindPattern> quickFind;
    std::shared_ptr<LogFilteredData> destination;
    std::unique_ptr<FilteredView> view;
    decltype( CrawlerWidget::filteredViewsData_ )::node_type registration;
    ~State()
    {
        Q_ASSERT( QThread::currentThread() == thread );
        if ( view ) view->stopSearchAndWait();
    }
};
struct CrawlerWidget::FilterUpdatePublication::State {
    QPointer<CrawlerWidget> owner;
    std::shared_ptr<PreparedFilterUpdate> prepared;
    std::unique_ptr<LogFilteredData::SearchUpdatePublication> model;
    QPointer<LogFilteredData> installedModel;
    std::shared_ptr<FilterUpdateStaging::State> staging;
    QString destinationPaneId;
    size_t registeredModels = 0;
    int registeredTabs = 0;
    bool pendingLinesChanged = false;
    bool installed = false;
    bool released = false;
    bool notified = false;
};

namespace {
LogFilteredData::Visibility resultVisibility( const CrawlerWidget::FilterUpdateCapture& captured )
{
    using Flags = LogFilteredData::VisibilityFlags;
    return captured.revealMatches ? Flags::Matches | Flags::Marks : captured.oldVisibility;
}

std::shared_ptr<const LogFilteredData::SearchUpdateCapture> captureModelUpdate(
    const LogFilteredData& model, const CrawlerWidget::FilterUpdateCapture& captured )
{
    const auto visibility = resultVisibility( captured );
    if ( captured.candidate.text.isEmpty() ) {
        const bool allVisible = klogg::emptyLensFilterPolicy( true, captured.showAllWhenEmpty )
            == klogg::EmptyFilterPolicy::MirrorAllLines;
        return model.captureSearchReset( allVisible, visibility );
    }
    return model.captureSearchUpdate( captured.candidate.regularExpressionPattern(),
        visibility, captured.startLine, captured.endLine );
}
}

CrawlerWidget::FilterUpdateStaging::FilterUpdateStaging( std::shared_ptr<State> state )
    : state_( std::move( state ) ) {}
CrawlerWidget::FilterUpdateStaging::~FilterUpdateStaging() = default;

std::shared_ptr<const CrawlerWidget::FilterUpdateCapture>
CrawlerWidget::FilterUpdateStaging::capture() const
{
    Q_ASSERT( QThread::currentThread() == state_->thread );
    return state_->captured;
}

SearchToolbar::FilterSpec CrawlerWidget::FilterUpdateStaging::effectiveFilter() const
{
    return capture()->candidate;
}

std::unique_ptr<CrawlerWidget::FilterUpdatePublication> CrawlerWidget::FilterUpdateStaging::lock(
    const std::shared_ptr<PreparedFilterUpdate>& prepared )
{
    Q_ASSERT( QThread::currentThread() == state_->thread );
    if ( !state_->owner || !prepared || prepared->captured != state_->captured ) return {};
    auto& owner = *state_->owner;
    if ( !state_->destination ) return owner.lockFilterUpdate( prepared );
    if ( !owner.isFilterUpdateCurrent( *prepared ) ) return {};
    auto publication = std::make_unique<FilterUpdatePublication::State>();
    publication->owner = &owner;
    publication->prepared = prepared;
    publication->staging = state_;
    publication->model = state_->destination->lockSearchUpdate( prepared->model );
    if ( !publication->model ) return {};
    if ( !state_->view ) {
        state_->view = std::make_unique<FilteredView>( state_->destination.get(), state_->quickFind.get() );
        owner.configureFilteredView( state_->view.get() );
        state_->view->registerShortcuts();
        owner.connectAllFilteredViewSlots( state_->view.get() );
        owner.connectModelPublication( state_->destination.get() );
        decltype( owner.filteredViewsData_ ) registration;
        registration.emplace( state_->view.get(), state_->destination );
        state_->registration = registration.extract( state_->view.get() );
    }
    owner.filteredViewsData_.reserve( owner.filteredViewsData_.size() + 1 );
    publication->registeredModels = owner.filteredViewsData_.size();
    publication->registeredTabs = owner.tabbedFilteredView_->count();
    publication->destinationPaneId = state_->view->runtimePaneId();
    return std::unique_ptr<FilterUpdatePublication>( new FilterUpdatePublication( std::move( publication ) ) );
}

std::unique_ptr<CrawlerWidget::FilterUpdateStaging> CrawlerWidget::stageFilterUpdate(
    const SearchToolbar::FilterSpec& candidate )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    auto effective = candidate;
    effective.keepResults = false; // Native Keep is a consumed one-shot action.
    auto captured = captureFilterUpdateState( effective );
    if ( !captured ) return {};
    auto state = std::make_shared<FilterUpdateStaging::State>();
    state->owner = this;
    if ( candidate.keepResults ) {
        state->source = logData_;
        state->quickFind = quickFindPattern_;
        state->destination = logData_->getNewFilteredData();
        captured->newPane = true;
        state->destination->setVisibility( resultVisibility( *captured ) );
        const auto mode = captured->contextMode;
        const auto context = captured->contextValue;
        state->destination->setContextLines( mode == 1 || mode == 3 ? context : 0,
                                            mode == 2 || mode == 3 ? context : 0 );
    }
    const auto& model = state->destination ? *state->destination : *logFilteredData_;
    captured->model = captureModelUpdate( model, *captured );
    if ( !captured->model ) return {};
    state->captured = std::move( captured );
    return std::unique_ptr<FilterUpdateStaging>( new FilterUpdateStaging( std::move( state ) ) );
}

std::shared_ptr<const CrawlerWidget::FilterUpdateCapture> CrawlerWidget::captureFilterUpdate(
    const SearchToolbar::FilterSpec& candidate ) const
{
    auto captured = captureFilterUpdateState( candidate );
    if ( !captured ) return {};
    captured->model = captureModelUpdate( *logFilteredData_, *captured );
    return captured->model ? captured : nullptr;
}

std::shared_ptr<CrawlerWidget::FilterUpdateCapture> CrawlerWidget::captureFilterUpdateState(
    const SearchToolbar::FilterSpec& candidate ) const
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( resourcesRetiring_ || !firstLoadDone_ || !searchToolbar_ || !logFilteredData_
         || !logMainView_ || !filteredView_ || candidate.keepResults
         || logMainView_->matchSelectionPending() || filteredView_->matchSelectionPending() ) return {};
    auto captured = std::make_shared<FilterUpdateCapture>();
    captured->tabId = runtimeTabId();
    captured->mainPaneId = logMainView_->runtimePaneId();
    captured->resultsPaneId = filteredView_->runtimePaneId();
    captured->resultsOwnerId = logFilteredData_->reviewMarksId();
    captured->oldQueryGeneration = logFilteredData_->currentSearchGeneration();
    captured->oldVisibility = logFilteredData_->visibility();
    captured->contextMode = contextLinesMode_;
    captured->contextValue = contextLinesSpinBox_->value();
    captured->draft = searchToolbar_->filterDraftSnapshot();
    if ( !searchToolbar_->canInstallFilterDraft( captured->draft, candidate ) ) return {};
    captured->candidate = candidate;
    captured->appliedPattern = currentSearchPattern_;
    captured->searchState = searchState_.getState();
    captured->startLine = searchStartLine_;
    captured->endLine = searchEndLine_;
    captured->mainSelectionRevision = logMainView_->matchSelectionRevision();
    captured->resultsSelectionRevision = filteredView_->matchSelectionRevision();
    captured->revealMatches = !captured->oldVisibility.testFlag( LogFilteredData::VisibilityFlags::Matches );
    if ( candidate.text.isEmpty() ) {
        captured->showAllWhenEmpty = Configuration::get().showAllInFilteredViewWhenSearchEmpty();
    }
    return captured;
}

CrawlerWidget::FilterUpdatePreparation CrawlerWidget::prepareFilterUpdate(
    const std::shared_ptr<const FilterUpdateCapture>& captured, AtomicFlag& cancelled )
{
    if ( !captured ) return {};
    try {
        auto model = LogFilteredData::prepareSearchUpdate( captured->model, cancelled );
        if ( model.status != SearchPreparation::Status::Ready ) return { model.status, {} };
        auto prepared = std::make_shared<PreparedFilterUpdate>();
        prepared->captured = captured;
        prepared->model = std::move( model.update );
        return { SearchPreparation::Status::Ready, std::move( prepared ) };
    } catch ( ... ) {
        return { SearchPreparation::Status::Failed, {} };
    }
}

bool CrawlerWidget::isFilterUpdateCurrent( const PreparedFilterUpdate& prepared ) const
{
    if ( resourcesRetiring_ || prepared.consumed || !searchToolbar_ || !logFilteredData_
         || !logMainView_ || !filteredView_ || !tabbedFilteredView_
         || tabbedFilteredView_->currentWidget() != filteredView_ ) return false;
    const auto& captured = *prepared.captured;
    if ( captured.candidate.text.isEmpty()
         && captured.showAllWhenEmpty != Configuration::get().showAllInFilteredViewWhenSearchEmpty() ) return false;
    return captured.tabId == runtimeTabId()
        && captured.mainPaneId == logMainView_->runtimePaneId()
        && captured.resultsPaneId == filteredView_->runtimePaneId()
        && captured.resultsOwnerId == logFilteredData_->reviewMarksId()
        && captured.oldQueryGeneration == logFilteredData_->currentSearchGeneration()
        && captured.oldVisibility == logFilteredData_->visibility()
        && captured.contextMode == contextLinesMode_
        && captured.contextValue == contextLinesSpinBox_->value()
        && captured.appliedPattern == currentSearchPattern_
        && captured.searchState == searchState_.getState()
        && captured.startLine == searchStartLine_ && captured.endLine == searchEndLine_
        && !logMainView_->matchSelectionPending() && !filteredView_->matchSelectionPending()
        && captured.mainSelectionRevision == logMainView_->matchSelectionRevision()
        && captured.resultsSelectionRevision == filteredView_->matchSelectionRevision()
        && searchToolbar_->canInstallFilterDraft( captured.draft, captured.candidate );
}

std::unique_ptr<CrawlerWidget::FilterUpdatePublication> CrawlerWidget::lockFilterUpdate(
    const std::shared_ptr<PreparedFilterUpdate>& prepared )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( !prepared || prepared->captured->newPane || !isFilterUpdateCurrent( *prepared ) ) return {};
    auto state = std::make_unique<FilterUpdatePublication::State>();
    state->owner = this;
    state->prepared = prepared;
    state->destinationPaneId = prepared->captured->resultsPaneId;
    state->model = logFilteredData_->lockSearchUpdate( prepared->model );
    if ( !state->model ) return {};
    return std::unique_ptr<FilterUpdatePublication>( new FilterUpdatePublication( std::move( state ) ) );
}

CrawlerWidget::FilterUpdatePublication::FilterUpdatePublication( std::unique_ptr<State> state )
    : state_( std::move( state ) ) {}
CrawlerWidget::FilterUpdatePublication::~FilterUpdatePublication() { release(); }

bool CrawlerWidget::FilterUpdatePublication::install()
{
    if ( !state_->owner || state_->installed || state_->released ) return false;
    auto& owner = *state_->owner;
    auto& prepared = *state_->prepared;
    if ( state_->staging && ( owner.filteredViewsData_.size() != state_->registeredModels
         || owner.tabbedFilteredView_->count() != state_->registeredTabs ) ) return false;
    if ( !owner.isFilterUpdateCurrent( prepared ) || !state_->model->install() ) return false;
    const auto& captured = *prepared.captured;
    if ( state_->staging ) {
        auto& staging = *state_->staging;
        const QSignalBlocker tabSignals{ owner.tabbedFilteredView_ };
        owner.logFilteredData_->interruptSearch();
        const auto inserted = owner.filteredViewsData_.insert( std::move( staging.registration ) );
        Q_ASSERT( inserted.inserted );
        Q_UNUSED( inserted )
        owner.logFilteredData_ = staging.destination;
        owner.filteredView_ = staging.view.release();
        if ( owner.qfSavedFocus_ != owner.logMainView_ && owner.qfSavedFocus_ != owner.filteredView_ ) {
            owner.qfSavedFocus_.clear();
        }
        owner.logMainView_->useNewFiltering( owner.logFilteredData_.get() );
        const auto index = owner.tabbedFilteredView_->addTab( owner.filteredView_, QString{} );
        owner.tabbedFilteredView_->setTabsClosable( true );
        owner.tabbedFilteredView_->setCurrentIndex( index );
    }
    // No event-loop turn or external notification occurs between preflight and
    // installation. A failed source admission above leaves the toolbar untouched.
    const auto draftInstalled = owner.searchToolbar_->installFilterDraft( captured.draft, captured.candidate );
    Q_ASSERT( draftInstalled );
    Q_UNUSED( draftInstalled )
    owner.searchUpdateThrottleTimer_.stop();
    owner.searchUpdatePending_ = false;
    owner.presentationSelectionRestorePending_ = false;
    state_->pendingLinesChanged = owner.searchPendingLines_ != 0;
    owner.searchPendingLines_ = 0;
    owner.searchInfoLine_->hideGauge();
    owner.searchToolbar_->setSearchInProgress( false );
    owner.searchState_.setAutorefresh( captured.candidate.autoRefresh );
    if ( captured.candidate.text.isEmpty() ) owner.searchState_.resetState();
    else owner.searchState_.startSearch();
    owner.lastPublishedMatchCount_ = 0_lcount;
    owner.nbMatches_ = owner.logFilteredData_->getNbMatches();
    if ( captured.revealMatches ) {
        const QSignalBlocker visibilitySignals{ owner.visibilityBox_ };
        owner.visibilityBox_->setCurrentIndex( 0 );
    }
    owner.setAppliedSearchPattern( captured.candidate.text.isEmpty() ? RegularExpressionPattern{}
        : captured.candidate.regularExpressionPattern(), true );
    owner.tabbedFilteredView_->setTabText( owner.tabbedFilteredView_->indexOf( owner.filteredView_ ),
        QStringLiteral( "Find \"%1\"" ).arg( captured.candidate.text ) );
    owner.printSearchInfoMessage( owner.nbMatches_ );
    state_->installedModel = owner.logFilteredData_.get();
    prepared.consumed = true;
    state_->installed = true;
    return true;
}

QString CrawlerWidget::FilterUpdatePublication::paneId() const
{
    return state_->destinationPaneId;
}

void CrawlerWidget::FilterUpdatePublication::release()
{
    if ( state_->released ) return;
    state_->released = true;
    state_->model->release();
}

void CrawlerWidget::FilterUpdatePublication::notify()
{
    if ( !state_->installed || !state_->released || state_->notified ) return;
    state_->notified = true;
    const auto owner = state_->owner;
    const auto installedModel = state_->installedModel;
    const auto captured = state_->prepared->captured;
    const bool pendingChanged = state_->pendingLinesChanged;
    auto model = std::move( state_->model );
    const auto current = [&] {
        return owner && installedModel && owner->logFilteredData_.get() == installedModel.data()
            && model->isCurrent();
    };
    if ( !current() ) return;
    // Record the accepted query before observers can submit another one. A prior
    // owner's observer may already have edited this owner's draft without applying it.
    owner->searchToolbar_->recordSearch( captured->candidate.text );
    if ( current() && captured->newPane ) Q_EMIT owner->filteredViewChanged();
    if ( current() && pendingChanged ) Q_EMIT owner->searchPendingLinesChanged();
    if ( current() && captured->draft.filter.matchCase != captured->candidate.matchCase
         && owner->searchToolbar_->isMatchCase() == captured->candidate.matchCase ) {
        Q_EMIT owner->matchCaseChanged( captured->candidate.matchCase );
    }
    if ( current() && captured->draft.filter.autoRefresh != captured->candidate.autoRefresh
         && owner->searchToolbar_->isAutoRefresh() == captured->candidate.autoRefresh ) {
        Q_EMIT owner->searchRefreshChanged( captured->candidate.autoRefresh );
    }
    if ( current() ) model->notify();
}
