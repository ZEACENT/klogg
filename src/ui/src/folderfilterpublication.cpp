#include "foldercrawlerwidget.h"

#include <QApplication>
#include <QComboBox>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QThread>
#include <QToolButton>
#include <algorithm>
#include <stdexcept>

#include "atomicflag.h"
#include "folderfilteredview.h"
#include "logmainview.h"
#include "quickfindpattern.h"

// Only immutable request values and model captures cross the worker boundary.
struct FolderCrawlerWidget::FilterUpdateCapture {
    QString folder;
    QStringList filePaths;
    SearchToolbar::FilterSpec candidate;
    klogg::folder::ContextOptions context;
    FolderSearchEngine::ScanSettings settings;
    std::shared_ptr<const FolderSearchResults::SearchReplacementCapture> replacement;
    std::shared_ptr<const FolderSearchResults::MarksLayoutCapture> revealOldPane;
};

struct FolderCrawlerWidget::PreparedFilterUpdate {
    std::shared_ptr<const FilterUpdateCapture> captured;
    std::shared_ptr<FolderSearchResults::PreparedSearchReplacement> replacement;
    std::shared_ptr<FolderSearchResults::PreparedMarksLayout> revealOldPane;
    QStringList filePaths;
    quint64 matches = 0;
};

struct FolderCrawlerWidget::FilterUpdateStaging::State {
    QThread* const thread = QThread::currentThread();
    QPointer<FolderCrawlerWidget> owner;
    std::shared_ptr<const FilterUpdateCapture> captured;
    SearchToolbar::FilterDraftSnapshot draft;
    quint64 revision = 0;
    quint64 generation = 0;
    quint64 marksRevision = 0;
    quint64 mainSelection = 0;
    quint64 resultSelection = 0;
    quint64 openRevision = 0;
    QString mainFile;
    bool searchActive = false;
    int visibility = 0;
    int contextMode = 0;
    int contextValue = 0;
    QPointer<FolderSearchResults> searchTarget;
    std::shared_ptr<FolderSearchResults> original;
    std::shared_ptr<FolderSearchResults> destination;
    std::shared_ptr<QuickFindPattern> quickFind;
    std::unique_ptr<ResultPane> newPane;
    bool consumed = false;
    ~State()
    {
        Q_ASSERT( QThread::currentThread() == thread );
        // An unpublished view is not in the widget's Qt child tree.
        if ( newPane ) delete newPane->view;
    }
};

struct FolderCrawlerWidget::FilterUpdatePublication::State {
    std::shared_ptr<FilterUpdateStaging::State> staging;
    std::shared_ptr<PreparedFilterUpdate> prepared;
    std::unique_ptr<FolderSearchResults::SearchReplacementPublication> replacement;
    std::unique_ptr<FolderSearchResults::MarksLayoutPublication> revealOldPane;
    QString paneId;
    quint64 installedGeneration = 0;
    bool installed = false;
    bool released = false;
    bool notified = false;
};

FolderCrawlerWidget::FilterUpdateStaging::FilterUpdateStaging( std::shared_ptr<State> state )
    : state_( std::move( state ) ) {}
FolderCrawlerWidget::FilterUpdateStaging::~FilterUpdateStaging() = default;

std::shared_ptr<const FolderCrawlerWidget::FilterUpdateCapture>
FolderCrawlerWidget::FilterUpdateStaging::capture() const
{
    Q_ASSERT( QThread::currentThread() == state_->thread );
    return state_->captured;
}

std::unique_ptr<FolderCrawlerWidget::FilterUpdateStaging> FolderCrawlerWidget::stageFilterUpdate(
    const SearchToolbar::FilterSpec& candidate )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( resourcesRetiring_ || folderPath_.isEmpty() || !activeResults()
         || resultsTabs_->currentWidget() != activeFilteredView()
         || mainView_->matchSelectionPending() || activeFilteredView()->matchSelectionPending() ) return {};

    auto captured = std::make_shared<FilterUpdateCapture>();
    captured->folder = folderPath_;
    captured->filePaths = filePaths_;
    captured->candidate = candidate;
    if ( !candidate.text.isEmpty() ) captured->candidate.keepResults = false;
    captured->context = { contextBefore_, contextAfter_ };
    captured->settings = FolderSearchEngine::ScanSettings::capture();

    auto state = std::make_shared<FilterUpdateStaging::State>();
    state->owner = this;
    state->draft = searchToolbar_->filterDraftSnapshot();
    if ( !searchToolbar_->canInstallFilterDraft( state->draft, captured->candidate ) ) return {};
    state->revision = filterStateRevision_;
    state->generation = currentSearchGeneration_;
    state->marksRevision = marksRevision_;
    state->mainSelection = mainView_->matchSelectionRevision();
    state->resultSelection = activeFilteredView()->matchSelectionRevision();
    state->openRevision = mainViewOpenRequestRevision_;
    state->mainFile = currentMainFilePath_;
    state->searchActive = searchActive_;
    state->searchTarget = searchTargetResults_;
    state->visibility = visibilityBox_->currentIndex();
    state->contextMode = contextLinesComboBox_->currentIndex();
    state->contextValue = contextLinesSpinBox_->value();
    state->original = panes_[ static_cast<size_t>( activePaneIndex_ ) ]->results;
    state->destination = state->original;

    using V = FolderSearchResults::Visibility;
    const bool reveal = state->visibility == 1;
    const auto visibility = reveal ? V::MarksAndMatches : state->original->visibility();
    // Every real match group has a header after revealing matches. Marks-only
    // groups already visible remain visible, including when a group is collapsed.
    if ( !candidate.text.isEmpty() && candidate.keepResults
         && ( state->original->getNbLine() != 0_lcount || ( reveal && state->original->groupCount() != 0 ) ) ) {
        state->destination = std::make_shared<FolderSearchResults>();
        // Bind without building a throwaway marks projection on the GUI thread.
        state->destination->setVisibility( V::Matches );
        state->destination->setMarksStore( &folderMarks_ );
        if ( reveal ) {
            captured->revealOldPane = state->original->captureMarksLayout( folderMarks_, nullptr, visibility );
            if ( !captured->revealOldPane ) return {};
        }
    }
    captured->replacement = state->destination->captureSearchReplacement( visibility );
    if ( !captured->replacement ) return {};
    state->captured = std::move( captured );
    return std::unique_ptr<FilterUpdateStaging>( new FilterUpdateStaging( std::move( state ) ) );
}

FolderCrawlerWidget::FilterUpdatePreparation FolderCrawlerWidget::prepareFilterUpdate(
    const std::shared_ptr<const FilterUpdateCapture>& captured, const AtomicFlag& cancelled )
{
    using Status = SearchPreparation::Status;
    using Completion = FolderSearchEngine::CompletionStatus;
    if ( !captured ) return {};
    try {
        if ( cancelled ) return { Status::Cancelled, {} };
        auto prepared = std::make_shared<PreparedFilterUpdate>();
        prepared->captured = captured;
        if ( captured->candidate.text.isEmpty() ) {
            prepared->filePaths = captured->filePaths;
            prepared->replacement = FolderSearchResults::prepareSearchReset( captured->replacement, prepared->filePaths );
        } else {
            auto scan = FolderSearchEngine::prepareFolderSearch( captured->folder,
                captured->candidate.regularExpressionPattern(), captured->context, captured->settings,
                []( const QString& root, const FolderSearchEngine::StopPredicate& stop ) {
                    auto paths = enumerateSearchFolder( root, stop );
                    if ( paths.isEmpty() && !stop() ) {
                        // QDirIterator returns an empty range for a vanished root.
                        // Only a readable directory can publish an empty result.
                        const QFileInfo info{ root };
                        if ( !info.isDir() || !info.isReadable() ) throw std::runtime_error( "Folder unavailable" );
                    }
                    return paths;
                }, [&cancelled] { return static_cast<bool>( cancelled ); } );
            if ( scan.status == Completion::Cancelled ) return { Status::Cancelled, {} };
            if ( scan.status == Completion::CompileFailure ) return { Status::CompileFailure, {} };
            if ( scan.status != Completion::Ready ) return { Status::Failed, {} };
            prepared->filePaths = scan.filePaths;
            for ( const auto& group : scan.groups ) {
                if ( cancelled ) return { Status::Cancelled, {} };
                prepared->matches += static_cast<quint64>( std::count_if( group.matches.begin(), group.matches.end(),
                    []( const auto& record ) { return record.role == klogg::folder::RecordRole::Match; } ) );
            }
            prepared->replacement = FolderSearchResults::prepareSearchReplacement( captured->replacement, std::move( scan ) );
        }
        if ( !prepared->replacement ) return { Status::Failed, {} };
        if ( captured->revealOldPane ) {
            prepared->revealOldPane = FolderSearchResults::prepareMarksLayout( captured->revealOldPane );
            if ( !prepared->revealOldPane ) return { Status::Failed, {} };
        }
        if ( cancelled ) return { Status::Cancelled, {} };
        return { Status::Ready, std::move( prepared ) };
    } catch ( ... ) {
        return { Status::Failed, {} };
    }
}

bool FolderCrawlerWidget::filterUpdateCurrent( const FilterUpdateStaging::State& s ) const
{
    // No model-locking getters here: install also rechecks GUI admission while
    // holding the destination model's write guard.
    const auto& c = *s.captured;
    if ( resourcesRetiring_ || s.consumed || filterStateRevision_ != s.revision
         || activeResults() != s.original.get() || resultsTabs_->currentWidget() != activeFilteredView()
         || currentSearchGeneration_ != s.generation || searchActive_ != s.searchActive
         || searchTargetResults_ != s.searchTarget || marksRevision_ != s.marksRevision
         || mainViewOpenRequestRevision_ != s.openRevision || currentMainFilePath_ != s.mainFile
         || mainView_->matchSelectionPending() || activeFilteredView()->matchSelectionPending()
         || mainView_->matchSelectionRevision() != s.mainSelection
         || activeFilteredView()->matchSelectionRevision() != s.resultSelection
         || contextLinesComboBox_->currentIndex() != s.contextMode || contextLinesSpinBox_->value() != s.contextValue
         || visibilityBox_->currentIndex() != s.visibility || folderPath_ != c.folder || filePaths_ != c.filePaths
         || ( s.newPane && s.quickFind != quickFindPattern_ )
         || !searchToolbar_->canInstallFilterDraft( s.draft, c.candidate ) ) return false;
    if ( !c.candidate.text.isEmpty() ) {
        const auto current = FolderSearchEngine::ScanSettings::capture();
        if ( c.settings.search.backend != current.search.backend
             || c.settings.defaultEncodingMib != current.defaultEncodingMib ) return false;
    }
    // Hiding the focused Stop button or switching away from a focused results
    // pane would transfer keyboard focus. Do not steal it, even transiently.
    const auto* focus = QApplication::focusWidget();
    if ( focus == searchToolbar_->stopButton() ) return false;
    return s.destination == s.original || !focus
        || ( focus != activeFilteredView() && !activeFilteredView()->isAncestorOf( focus ) );
}

std::unique_ptr<FolderCrawlerWidget::FilterUpdatePublication> FolderCrawlerWidget::FilterUpdateStaging::lock(
    const std::shared_ptr<PreparedFilterUpdate>& prepared )
{
    Q_ASSERT( QThread::currentThread() == state_->thread );
    auto* owner = state_->owner.data();
    if ( !owner || !prepared || prepared->captured != state_->captured || !owner->filterUpdateCurrent( *state_ ) ) return {};
    auto guard = std::make_unique<FilterUpdatePublication::State>();
    guard->staging = state_;
    guard->prepared = prepared;
    if ( state_->destination != state_->original ) {
        if ( !state_->newPane ) {
            state_->quickFind = owner->quickFindPattern_;
            state_->newPane = owner->makePane( state_->destination );
        }
        owner->panes_.reserve( owner->panes_.size() + 1 );
        guard->paneId = state_->newPane->view->runtimePaneId();
    } else {
        guard->paneId = owner->activeFilteredView()->runtimePaneId();
    }
    if ( prepared->revealOldPane ) {
        guard->revealOldPane = state_->original->lockMarksLayout( prepared->revealOldPane );
        if ( !guard->revealOldPane ) return {};
    }
    guard->replacement = state_->destination->lockSearchReplacement( prepared->replacement );
    if ( !guard->replacement ) return {};
    return std::unique_ptr<FilterUpdatePublication>( new FilterUpdatePublication( std::move( guard ) ) );
}

FolderCrawlerWidget::FilterUpdatePublication::FilterUpdatePublication( std::unique_ptr<State> state )
    : state_( std::move( state ) ) {}
FolderCrawlerWidget::FilterUpdatePublication::~FilterUpdatePublication() = default;
QString FolderCrawlerWidget::FilterUpdatePublication::paneId() const { return state_->paneId; }

bool FolderCrawlerWidget::FilterUpdatePublication::install()
{
    auto& s = *state_->staging;
    auto* owner = s.owner.data();
    if ( state_->released || !owner || !owner->filterUpdateCurrent( s ) ) return false;
    // Both guards have exclusive, consume-once admission. No callbacks between
    // model swaps. Drop writer locks before Qt can synchronously resize/show a
    // newly attached view: geometry queries read the installed model.
    if ( !state_->replacement->install() ) return false;
    if ( state_->revealOldPane ) {
        const bool installed = state_->revealOldPane->install();
        Q_ASSERT( installed );
        Q_UNUSED( installed )
    }
    state_->revealOldPane.reset();
    state_->replacement.reset();
    s.consumed = true;
    const auto& candidate = s.captured->candidate;
    const bool empty = candidate.text.isEmpty();
    const auto pattern = empty ? RegularExpressionPattern{} : candidate.regularExpressionPattern();
    const bool draftInstalled = owner->searchToolbar_->installFilterDraft( s.draft, candidate );
    Q_ASSERT( draftInstalled );
    Q_UNUSED( draftInstalled )
    owner->supersedeSearch();
    owner->currentSearchGeneration_ = owner->engine_->bumpGeneration();
    state_->installedGeneration = owner->currentSearchGeneration_;
    owner->searchActive_ = false;
    owner->searchToolbar_->setSearchInProgress( false );
    owner->filePaths_ = state_->prepared->filePaths;
    if ( empty ) {
        owner->lastResultStatusText_.clear();
        owner->updateReadyStatus();
    } else {
        owner->setFinishedStatus( state_->prepared->matches );
    }
    if ( s.visibility == 1 ) {
        const QSignalBlocker blocker{ owner->visibilityBox_ };
        owner->visibilityBox_->setCurrentIndex( 0 );
    }
    const auto title = QStringLiteral( "Find \"%1\"" ).arg( candidate.text );
    const QSignalBlocker tabSignals{ owner->resultsTabs_ };
    if ( s.newPane ) {
        if ( !owner->currentSearchPattern_.pattern.isEmpty() ) {
            owner->resultsTabs_->setTabText( owner->activePaneIndex_,
                QStringLiteral( "Find \"%1\"" ).arg( owner->currentSearchPattern_.pattern ) );
        }
        s.newPane->appliedPattern = pattern;
        s.newPane->searchComplete = !empty;
        owner->appendPane( std::move( s.newPane ), title );
        if ( owner->qfSavedFocus_ != owner->mainView_ ) owner->qfSavedFocus_.clear();
    } else {
        auto& pane = *owner->panes_[ static_cast<size_t>( owner->activePaneIndex_ ) ];
        pane.appliedPattern = pattern;
        pane.searchComplete = !empty;
        if ( !empty ) owner->resultsTabs_->setTabText( owner->activePaneIndex_, title );
    }
    // Query attribution above is authoritative immediately, even when the
    // inactive tab defers repainting its views until activation. The old
    // selection belongs to the superseded query, not to the new row layout.
    owner->activeFilteredView()->invalidateMatchSelection();
    owner->setCurrentSearchPattern( pattern );
    owner->scheduleMainViewPresentationRefresh( true );
    state_->installed = true;
    return true;
}

void FolderCrawlerWidget::FilterUpdatePublication::release()
{
    state_->replacement.reset();
    state_->revealOldPane.reset();
    state_->released = true;
}

void FolderCrawlerWidget::FilterUpdatePublication::notify()
{
    if ( !state_->released || !state_->installed || state_->notified ) return;
    state_->notified = true;
    // Observers may start another search, close the tab, or delete this guard.
    const auto staging = state_->staging;
    const auto owner = staging->owner;
    const auto generation = state_->installedGeneration;
    const auto current = [&] { return owner && !owner->resourcesRetiring_
        && owner->currentSearchGeneration_ == generation && owner->activeResults() == staging->destination.get(); };
    if ( !current() ) return;
    const auto& candidate = staging->captured->candidate;
    if ( !candidate.text.isEmpty() ) owner->searchToolbar_->recordSearch( candidate.text );
    if ( current() && staging->captured->revealOldPane ) Q_EMIT staging->original->layoutChanged();
    if ( current() && staging->original != staging->destination ) Q_EMIT owner->searchablesChanged();
    if ( current() ) Q_EMIT staging->destination->layoutChanged();
    if ( current() ) owner->flushFolderPresentation();
}
