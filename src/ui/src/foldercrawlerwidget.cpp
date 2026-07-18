/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "foldercrawlerwidget.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QFontMetrics>
#include <QInputDialog>
#include <QMessageBox>
#include <QSpinBox>
#include <QStringListModel>
#include <QTextCodec>
#include <QFont>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QShortcut>
#include <QSplitter>
#include <QToolButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "abstractlogview.h"
#include "configuration.h"
#include "crawlershortcuts.h"
#include "filterdiffdialog.h"
#include "folderfilteredview.h"
#include "foldersearchengine.h"
#include "foldersearchresults.h"
#include "loadingstatus.h"
#include "log.h"
#include "logdata.h"
#include "logmainview.h"
#include "overviewwidget.h"
#include "predefinedfilters.h"
#include "predefinedfilterscombobox.h"
#include "savefavoritedialog.h"
#include "quickfindpattern.h"
#include "regularexpression.h"
#include "regularexpressionpattern.h"
#include "savedsearches.h"
#include "searchablelogdata.h"
#include "searchtoolbar.h"

// Implementation of the view context for FolderCrawlerWidget (mirrors
// CrawlerWidgetContext in crawlerwidget.cpp:96-170). Serializes the search
// pattern text + option toggles (+ optional splitter sizes) so folder tabs can
// be persisted and restored across sessions. doSetViewContext restores the
// pattern + toggles WITHOUT auto-running a search (one Enter re-runs), matching
// the requirement that restoring a session does not kick off heavy work.
class FolderCrawlerContext : public ViewContextInterface {
  public:
    // Construct from the stored JSON string (forwarded by setViewContext).
    explicit FolderCrawlerContext( const QString& json )
    {
        loadFromJson( json );
    }

    // Construct from the live widget state (used by doGetViewContext).
    FolderCrawlerContext( QString pattern, bool ignoreCase, bool useRegexp, bool inverse,
                          bool boolean, QList<int> sizes )
        : pattern_{ std::move( pattern ) }
        , ignoreCase_{ ignoreCase }
        , useRegexp_{ useRegexp }
        , inverse_{ inverse }
        , boolean_{ boolean }
        , sizes_{ std::move( sizes ) }
    {
    }

    QString toString() const override
    {
        QVariantMap properties;
        properties[ "P" ] = pattern_;
        properties[ "IC" ] = ignoreCase_;
        properties[ "RE" ] = useRegexp_;
        properties[ "IR" ] = inverse_;
        properties[ "BC" ] = boolean_;

        QVariantList sizeVariants;
        for ( const auto s : sizes_ ) {
            sizeVariants.append( s );
        }
        properties[ "S" ] = sizeVariants;

        return QJsonDocument::fromVariant( properties ).toJson( QJsonDocument::Compact );
    }

    const QString& pattern() const
    {
        return pattern_;
    }
    bool ignoreCase() const
    {
        return ignoreCase_;
    }
    bool useRegexp() const
    {
        return useRegexp_;
    }
    bool inverse() const
    {
        return inverse_;
    }
    bool boolean() const
    {
        return boolean_;
    }
    QList<int> sizes() const
    {
        return sizes_;
    }

  private:
    void loadFromJson( const QString& json )
    {
        const auto properties = QJsonDocument::fromJson( json.toLatin1() ).toVariant().toMap();

        pattern_ = properties.value( "P" ).toString();
        ignoreCase_ = properties.value( "IC" ).toBool();
        useRegexp_ = properties.value( "RE" ).toBool();
        inverse_ = properties.value( "IR" ).toBool();
        boolean_ = properties.value( "BC" ).toBool();

        if ( properties.contains( "S" ) ) {
            const auto sizes = properties.value( "S" ).toList();
            for ( const auto& s : sizes ) {
                sizes_.append( s.toInt() );
            }
        }
    }

    QString pattern_;
    bool ignoreCase_ = false;
    bool useRegexp_ = false;
    bool inverse_ = false;
    bool boolean_ = false;
    QList<int> sizes_;
};

// Defined out-of-line so the unique_ptr<FolderFilteredMarkProvider> member
// (incomplete in the header) destroys where the provider type is complete.
FolderCrawlerWidget::ResultPane::ResultPane() = default;
FolderCrawlerWidget::ResultPane::~ResultPane() = default;

FolderCrawlerWidget::FolderCrawlerWidget( QWidget* parent )
    : QWidget( parent )
    , colorLabelsController_( this, [ this ]() { return activeView(); } )
{
    placeholderData_ = std::make_shared<LogData>();
    currentMainData_ = placeholderData_;
    quickFindPattern_ = std::make_shared<QuickFindPattern>();
    engine_ = new FolderSearchEngine( this ); // QObject-owned

    // --- toolbar ---
    // SearchToolbar (shared with CrawlerWidget) owns the search input + option
    // toggles + Search/Stop buttons. Folder mode passes null SavedSearches
    // (no history). Collapse/expand + status label sit alongside it.
    searchToolbar_ = new SearchToolbar( this, nullptr );

    // Shared view-signal wiring (scratchpad / search composition / splitter
    // save / font zoom / exitView / highlightersChange / hover-highlight) --
    // the same component CrawlerWidget uses, so folder views cannot drift from
    // single-file behavior again. Hooks adapt the host-specific parts.
    {
        ViewSignalWiring::Hooks hooks;
        hooks.sendToScratchpad
            = [ this ]( const QString& text ) { Q_EMIT sendToScratchpad( text ); };
        hooks.replaceScratchpad
            = [ this ]( const QString& text ) { Q_EMIT replaceDataInScratchpad( text ); };
        hooks.saveSplitterSizes = [ this ]() {
            auto& splitterConfig = Configuration::get();
            splitterConfig.setSplitterSizes( splitter_->sizes() );
            splitterConfig.save();
        };
        hooks.exitView = [ this ]( AbstractLogView* fromView ) {
            if ( fromView == mainView_ ) {
                if ( auto* const fv = activeFilteredView() ) {
                    fv->setFocus();
                }
            }
            else {
                mainView_->setFocus();
            }
        };
        hooks.applyConfiguration = [ this ]() { applyConfiguration(); };
        hooks.hoveredOverLine
            = [ this ]( AbstractLogView* view, LineNumber row ) { highlightOverviewForRow( view, row ); };
        hooks.leftHoveringZone = [ this ]() { overviewWidget_->removeHighlight(); };
        viewSignalWiring_
            = std::make_unique<ViewSignalWiring>( this, searchToolbar_, std::move( hooks ) );
    }
    // Folder search is a one-shot grep (no file watching) with no notion of
    // predefined filters, favorites, keep-results, or auto-refresh -- hide
    // those file-search-only controls so the toolbar is not littered with
    // inert buttons. The toggles that matter for grep (case / regex / inverse
    // / boolean) and the search-history dropdown stay.
    searchToolbar_->searchRefreshButton()->hide();
    // keepSearchResultsButton stays VISIBLE: folder search supports Keep results
    // (snapshot the current pane, start the next search in a new tab).
    // Predefined filters + favorites are shared global pattern stores; they
    // apply to folder search the same way as single-file (wired below).
    collapseAllButton_ = new QToolButton( this );
    collapseAllButton_->setText( tr( "Collapse all" ) );
    expandAllButton_ = new QToolButton( this );
    expandAllButton_->setText( tr( "Expand all" ) );
    statusLabel_ = new QLabel( this );
    statusLabelDefaultPalette_ = statusLabel_->palette();
    // Results-view visibility filter (parity with CrawlerWidget). Index order
    // maps to FolderSearchResults::Visibility in changeFilteredViewVisibility.
    visibilityBox_ = new QComboBox( this );
    visibilityBox_->addItem( tr( "Marks and matches" ) );
    visibilityBox_->addItem( tr( "Marks" ) );
    visibilityBox_->addItem( tr( "Matches" ) );
    visibilityBox_->setCurrentIndex( 0 );
    // grep -A/-B/-C context controls (mirrors CrawlerWidget's
    // contextLinesSpinBox_ + contextLinesComboBox_). Combo itemData encodes the
    // mode: 0=None, 1=Before(-B), 2=After(-A), 3=Around(-C).
    contextLinesSpinBox_ = new QSpinBox( this );
    contextLinesSpinBox_->setMinimum( 0 );
    contextLinesSpinBox_->setMaximum( 1000 );
    contextLinesSpinBox_->setValue( 0 );
    contextLinesSpinBox_->setToolTip( tr( "Number of context lines" ) );
    contextLinesSpinBox_->setSizePolicy( QSizePolicy::Minimum, QSizePolicy::Minimum );
    contextLinesSpinBox_->setMinimumWidth(
        QFontMetrics( contextLinesSpinBox_->font() ).horizontalAdvance( QStringLiteral( "0000" ) )
        + 32 );
    contextLinesComboBox_ = new QComboBox( this );
    contextLinesComboBox_->setToolTip( tr( "Context lines mode" ) );
    contextLinesComboBox_->setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Fixed );
    contextLinesComboBox_->setContentsMargins( 2, 2, 2, 2 );
    contextLinesComboBox_->addItem( tr( "None" ), 0 );
    contextLinesComboBox_->addItem( tr( "Before (-B)" ), 1 );
    contextLinesComboBox_->addItem( tr( "After (-A)" ), 2 );
    contextLinesComboBox_->addItem( tr( "Around (-C)" ), 3 );
    contextLinesComboBox_->setCurrentIndex( 0 );

    auto* toolbar = new QHBoxLayout;
    // visibilityBox_ ("Marks and matches") is added FIRST, matching single-file
    // CrawlerWidget (crawlerwidget.cpp:1287) where it sits at the far left of the
    // results toolbar, ahead of the search bar. The folder-only collapse/expand
    // buttons follow the search bar so they no longer displace the shared combo.
    toolbar->addWidget( visibilityBox_ );
    toolbar->addWidget( searchToolbar_, 1 );
    toolbar->addSpacing( 12 );
    toolbar->addWidget( collapseAllButton_ );
    toolbar->addWidget( expandAllButton_ );
    toolbar->addWidget( contextLinesSpinBox_ );
    toolbar->addWidget( contextLinesComboBox_ );
    toolbar->addSpacing( 12 );
    toolbar->addWidget( statusLabel_ );

    // --- views ---
    // Overview pair is constructed up-front because LogMainView's ctor needs
    // non-null Overview + OverviewWidget (refreshOverview derefs them). The
    // widget is re-parented to mainView_ below so AbstractLogView owns its
    // placement (geometry set in resizeEvent), exactly like CrawlerWidget.
    overviewWidget_ = new OverviewWidget( this );
    overviewWidget_->setOverview( &overview_ );
    mainView_ = new LogMainView( placeholderData_.get(), quickFindPattern_.get(), &overview_,
                                 overviewWidget_, this );
    overviewWidget_->setParent( mainView_ );
    overview_.setVisible( Configuration::get().isOverviewVisible() );
    mainView_->refreshOverview();

    // Color labels: route the view's context-menu signals to the shared
    // controller so labels highlight in every view (single-file parity).
    colorLabelsController_.watchView( mainView_ );

    // Shared view-signal wiring (scratchpad / search composition / splitter /
    // font zoom / exitView / highlightersChange). Folder search cannot honor
    // line-range limits, so those menu entries are hidden instead of
    // misleadingly graying the view.
    viewSignalWiring_->wireView( mainView_ );
    mainView_->setControlsSearchLimits( false );

    // Folder-mode marks: inject a per-file MarkProvider so the main view's mark
    // bullet + Next/Prev-mark navigation work without LogFilteredData. The
    // provider reads folderMarks_ + currentMainFilePath_ by pointer, so it stays
    // correct as files are swapped.
    mainViewMarkProvider_.marks = &folderMarks_;
    mainViewMarkProvider_.currentFile = &currentMainFilePath_;
    mainView_->setMarkProvider( &mainViewMarkProvider_ );

    // Seed view config + search toggles from Configuration, mirroring CrawlerWidget.
    const auto& config = Configuration::get();
    mainView_->setLineNumbersVisible( config.mainLineNumbersVisible() );
    searchToolbar_->setAutoRefresh( config.isSearchAutoRefreshDefault() );
    searchToolbar_->setMatchCase( !config.isSearchIgnoreCaseDefault() );
    searchToolbar_->setUseRegexp( config.mainRegexpType() == SearchRegexpType::ExtendedRegexp );
    searchToolbar_->setBoolean( config.isSearchLogicalCombiningDefault() );

    // Results are shown in a tabbed set of panes (Keep results in a new window).
    // Each pane owns its own FolderSearchResults + FolderFilteredView + mark
    // provider; the main view is shared. createPane wires per-pane signals.
    resultsTabs_ = new QTabWidget( this );
    resultsTabs_->setDocumentMode( true );
    resultsTabs_->setTabsClosable( true );
    resultsTabs_->setTabBarAutoHide( true );
    connect( resultsTabs_, &QTabWidget::currentChanged, this,
             &FolderCrawlerWidget::onActivePaneChanged );
    connect( resultsTabs_, &QTabWidget::tabCloseRequested, this,
             &FolderCrawlerWidget::onClosePane );

    // Composite "bottom window" mirroring CrawlerWidget: pane 1 is the main
    // view, pane 2 packs the search-toolbar row above the results tabs, so the
    // toolbar renders BETWEEN the two views (as in single-file tabs) rather than
    // pinned above the splitter. bottomWindow->setLayout reparents the toolbar
    // widgets into the bottom pane, exactly like CrawlerWidget.
    auto* bottomWindow = new QWidget;
    bottomWindow->setContentsMargins( 2, 0, 2, 0 );
    auto* bottomLayout = new QVBoxLayout;
    bottomLayout->setContentsMargins( 2, 2, 2, 2 );
    bottomLayout->addLayout( toolbar );
    bottomLayout->addWidget( resultsTabs_ );
    bottomWindow->setLayout( bottomLayout );

    splitter_ = new QSplitter( Qt::Vertical, this );
    splitter_->addWidget( mainView_ );
    splitter_->addWidget( bottomWindow );
    // Watched for its first Resize to seed the default proportions (see
    // eventFilter): any earlier setSizes is discarded by the initial layout.
    splitter_->installEventFilter( this );

    // Seed the first pane AFTER resultsTabs_ is laid out + mainView_ exists.
    // Block currentChanged so the seed addTab does not fire onActivePaneChanged
    // while panes_ is empty.
    {
        QSignalBlocker blocker( resultsTabs_ );
        createPane( QString() );
    }

    auto* root = new QVBoxLayout( this );
    root->addWidget( splitter_, 1 );

    // --- signals ---
    connect( searchToolbar_, &SearchToolbar::searchRequested, this,
             &FolderCrawlerWidget::startSearch );
    connect( searchToolbar_, &SearchToolbar::stopRequested, this,
             &FolderCrawlerWidget::stopSearch );
    connect( searchToolbar_, &SearchToolbar::optionsChanged, this, [ this ]() {
        // An option that affects the search changed (case/regex/inverse/
        // boolean); the existing result is stale until the user re-runs. Surface
        // it so the toggle is not silently ignored (the search only re-runs on
        // Enter / Search) -- but keep the last match count visible alongside
        // the hint instead of wiping it. Also clears the invalid-pattern error
        // styling: the hint replaces the error text.
        if ( statusLabel_ != nullptr ) {
            statusErrorActive_ = false;
            statusLabel_->setPalette( statusLabelDefaultPalette_ );
            statusLabel_->setAutoFillBackground( false );
            if ( !lastResultStatusText_.isEmpty() ) {
                statusLabel_->setText(
                    tr( "%1 - options changed, press Enter to re-run" )
                        .arg( lastResultStatusText_ ) );
            }
            else {
                statusLabel_->setText( tr( "Options changed - press Enter to re-run" ) );
            }
        }
    } );
    // Filter favorites + predefined filters mirror CrawlerWidget: the host owns
    // the dialogs/persistence; selecting a predefined filter applies its pattern
    // (+regex) to the toolbar so the next search uses it.
    connect( searchToolbar_, &SearchToolbar::saveFavoriteRequested, this,
             &FolderCrawlerWidget::saveAsFavorite );
    // Predefined-filter dropdown selection: update the predefined-filters state
    // only (must NOT toggle auto-refresh tracking). Mirrors
    // crawlerwidget.cpp:1348-1349; without this the combo's currentIndex is not
    // reset after a selection.
    connect( searchToolbar_, &SearchToolbar::predefinedFilterActivated, this,
             [ this ]( QString ) { updatePredefinedFiltersWidget(); } );
    // Search-history context-menu actions: the host owns the shared
    // SavedSearches + dialogs (parity with CrawlerWidget). Without these the
    // toolbar's clear/edit-history actions are silent no-ops in folder mode.
    connect( searchToolbar_, &SearchToolbar::clearHistoryRequested, this,
             &FolderCrawlerWidget::clearSearchHistory );
    connect( searchToolbar_, &SearchToolbar::editHistoryRequested, this,
             &FolderCrawlerWidget::editSearchHistory );
    connect( searchToolbar_->predefinedFilters(), &PredefinedFiltersComboBox::filterChanged, this,
             [ this ]( const QList<PredefinedFilter>& filters ) {
                 if ( filters.isEmpty() ) {
                     return;
                 }
                 const auto& filter = filters.front();
                 if ( searchToolbar_->isUseRegexp() != filter.useRegex ) {
                     searchToolbar_->setUseRegexp( filter.useRegex );
                 }
                 searchToolbar_->setSearchPattern(
                     searchToolbar_->escapeSearchPattern( filter.pattern, filter.useRegex ) );
             } );
    connect( searchToolbar_, &SearchToolbar::searchTextChanged, this,
             [ this ]( QString ) { updatePredefinedFiltersWidget(); } );
    connect( collapseAllButton_, &QToolButton::clicked, this, &FolderCrawlerWidget::collapseAll );
    connect( expandAllButton_, &QToolButton::clicked, this, &FolderCrawlerWidget::expandAll );
    connect( visibilityBox_, qOverload<int>( &QComboBox::currentIndexChanged ), this,
             &FolderCrawlerWidget::changeFilteredViewVisibility );
    connect( contextLinesComboBox_, qOverload<int>( &QComboBox::currentIndexChanged ), this,
             &FolderCrawlerWidget::onContextControlsChanged );
    connect( contextLinesSpinBox_, qOverload<int>( &QSpinBox::valueChanged ), this,
             &FolderCrawlerWidget::onContextControlsChanged );

    connect( engine_, &FolderSearchEngine::searchStarted, this, &FolderCrawlerWidget::onSearchStarted );
    connect( engine_, &FolderSearchEngine::searchProgressed, this,
             &FolderCrawlerWidget::onSearchProgressed );
    connect( engine_, &FolderSearchEngine::searchFinished, this,
             &FolderCrawlerWidget::onSearchFinished );
    connect( engine_, &FolderSearchEngine::fileGroupReady, this,
             &FolderCrawlerWidget::onFileGroupReady );

    // Per-pane view/result connections (newSelection/headerClicked/markLines/
    // deleteMarkLines + the pane's own results::layoutChanged) are wired in
    // createPane, so every pane -- including Keep-results snapshots -- is
    // self-contained.

    // Mark toggles from the main view (M shortcut / left-margin click): record
    // the line against the file currently shown, then fan out a refresh to every
    // pane (marks live in the shared per-file store, so a main-view mark must
    // repaint in all result panes, frozen ones too).
    connect( mainView_, &AbstractLogView::markLines, this,
             [ this ]( const klogg::vector<LineNumber>& lines ) {
                 for ( const auto& l : lines ) {
                     addMark( currentMainFilePath_, l );
                 }
                 refreshAllPanesForMarks();
             } );
    connect( mainView_, &AbstractLogView::deleteMarkLines, this,
             [ this ]( const klogg::vector<LineNumber>& lines ) {
                 for ( const auto& l : lines ) {
                     removeMark( currentMainFilePath_, l );
                 }
                 refreshAllPanesForMarks();
             } );

    // Apply Configuration (font, line numbers, overview, view shortcuts) now
    // that both views + the toolbar exist. Also re-applied on every
    // MainWindow::optionsChanged (the folder's applyConfiguration is connected
    // to optionsChanged by MainWindow::currentTabChanged).
    applyConfiguration();
    // Populate the predefined-filters combo from the shared collection.
    reloadPredefinedFilters();
}

FolderCrawlerWidget::~FolderCrawlerWidget() = default;

bool FolderCrawlerWidget::eventFilter( QObject* obj, QEvent* event )
{
    // Splitter proportions, mirroring CrawlerWidget::setup: a session-restored
    // per-tab context wins; NEW folder tabs seed from the saved global default
    // ("Save splitter position" in any tab). Applied on the splitter's first
    // Resize -- the first moment it has real geometry; setSizes any earlier
    // (ctor / showEvent / zero-delay timer) is discarded by the splitter's
    // initial layout, which clamps the bottom pane to its minimum size.
    if ( obj == splitter_ && event->type() == QEvent::Resize && !splitterSeedApplied_ ) {
        splitterSeedApplied_ = true;
        splitter_->setSizes( !pendingSplitterSizes_.isEmpty()
                                 ? pendingSplitterSizes_
                                 : Configuration::get().splitterSizes() );
    }
    return QWidget::eventFilter( obj, event );
}

void FolderCrawlerWidget::changeEvent( QEvent* event )
{
    if ( event->type() == QEvent::StyleChange && !statusErrorActive_
         && statusLabel_ != nullptr ) {
        // Track runtime theme/style switches (mirrors CrawlerWidget's
        // changeEvent) so the error-state restore does not re-apply a stale
        // palette. Skipped while the error highlight is up: the label's
        // current palette IS the error palette then.
        statusLabelDefaultPalette_ = statusLabel_->palette();
    }
    QWidget::changeEvent( event );
}

void FolderCrawlerWidget::setFolder( const QString& folderPath, const QStringList& filePaths )
{
    folderPath_ = folderPath;
    filePaths_ = filePaths;
    lastResultStatusText_.clear();
    updateReadyStatus();
}

void FolderCrawlerWidget::updateReadyStatus()
{
    // The folder path itself is shown in MainWindow's info line (status bar);
    // the toolbar status surfaces only the file count.
    statusLabel_->setText( tr( "Ready  (%n file(s))", "", static_cast<int>( filePaths_.size() ) ) );
}

QString FolderCrawlerWidget::statusText() const
{
    return statusLabel_ != nullptr ? statusLabel_->text() : QString();
}

std::optional<AbstractCrawlerWidget::MainViewInfo>
FolderCrawlerWidget::currentMainViewInfo() const
{
    // No real file loaded (still the empty placeholder) -> nullopt so MainWindow
    // falls back to the folder path in the info line.
    if ( currentMainData_ == nullptr || currentMainData_ == placeholderData_ ) {
        return {};
    }
    MainViewInfo info;
    info.path = currentMainFilePath_;
    info.size = static_cast<uint64_t>( currentMainData_->getFileSize() );
    info.lastModified = currentMainData_->getLastModifiedDate();
    info.nbLines = currentMainData_->getNbLine().get();
    if ( auto* codec = currentMainData_->getDisplayEncoding() ) {
        info.encodingText = QString::fromLatin1( codec->name() );
    }
    return info;
}

void FolderCrawlerWidget::addMark( const QString& file, LineNumber line )
{
    if ( file.isEmpty() ) {
        return;
    }
    folderMarks_[ file ].insert( line.get() );
}

void FolderCrawlerWidget::removeMark( const QString& file, LineNumber line )
{
    const auto it = folderMarks_.find( file );
    if ( it == folderMarks_.end() ) {
        return;
    }
    it->erase( line.get() );
    if ( it->empty() ) {
        folderMarks_.erase( it );
    }
}

void FolderCrawlerWidget::onFilteredViewMarkLines( const klogg::vector<LineNumber>& rows )
{
    // Each row is a result-view line; resolve it to (file, localLine) and mark
    // it in the shared per-file store. Skip headers (no source line). Only the
    // active/visible tab can be clicked, so activeResults() is the right model.
    auto* results = activeResults();
    if ( results == nullptr ) {
        return;
    }
    bool changed = false;
    for ( const auto& row : rows ) {
        if ( results->lineKind( row ) != LineKind::Data ) {
            continue;
        }
        const auto src = results->sourceForLine( row );
        addMark( src.filePath, src.localLine );
        changed = true;
    }
    if ( changed ) {
        // Marks are shared: refresh every pane (frozen ones too).
        refreshAllPanesForMarks();
    }
}

void FolderCrawlerWidget::onFilteredViewDeleteMarkLines( const klogg::vector<LineNumber>& rows )
{
    auto* results = activeResults();
    if ( results == nullptr ) {
        return;
    }
    bool changed = false;
    for ( const auto& row : rows ) {
        if ( results->lineKind( row ) != LineKind::Data ) {
            continue;
        }
        const auto src = results->sourceForLine( row );
        removeMark( src.filePath, src.localLine );
        changed = true;
    }
    if ( changed ) {
        refreshAllPanesForMarks();
    }
}

void FolderCrawlerWidget::changeFilteredViewVisibility( int index )
{
    // visibilityBox_ index order: 0 = Marks and matches, 1 = Marks, 2 = Matches.
    using V = FolderSearchResults::Visibility;
    auto* results = activeResults();
    if ( results == nullptr ) {
        return;
    }
    const auto v = [ index ]() -> V {
        switch ( index ) {
            case 1:
                return V::Marks;
            case 2:
                return V::Matches;
            default:
                return V::MarksAndMatches;
        }
    }();
    results->setVisibility( v ); // emits layoutChanged -> active view refreshes
}

void FolderCrawlerWidget::updateContextFromControls()
{
    // Mirrors CrawlerWidget::applyContextLines (crawlerwidget.cpp ~2177): combo
    // itemData encodes the mode (0 None, 1 -B, 2 -A, 3 -C); value 0 clears the
    // window but preserves the user's chosen mode for the next edit.
    const int mode = contextLinesComboBox_ ? contextLinesComboBox_->currentData().toInt() : 0;
    const int n = contextLinesSpinBox_ ? contextLinesSpinBox_->value() : 0;
    switch ( mode ) {
        case 1:
            contextBefore_ = n;
            contextAfter_ = 0;
            break; // -B
        case 2:
            contextBefore_ = 0;
            contextAfter_ = n;
            break; // -A
        case 3:
            contextBefore_ = n;
            contextAfter_ = n;
            break; // -C
        default:
            contextBefore_ = 0;
            contextAfter_ = 0;
            break; // None
    }
}

void FolderCrawlerWidget::onContextControlsChanged()
{
    // grep semantics: context is selected at scan time, so a control change must
    // re-scan (the engine captures context offsets while streaming). Re-run only
    // when a pattern is present so tweaking the controls before the first search
    // does not flash the "Searching..." status.
    updateContextFromControls();
    if ( !searchToolbar_->currentSearchText().isEmpty() ) {
        startSearch(); // bumps generation -> supersedes any in-flight scan
    }
}

void FolderCrawlerWidget::setResultsVisibility( FolderSearchResults::Visibility visibility )
{
    if ( visibilityBox_ == nullptr || activeResults() == nullptr ) {
        return;
    }
    using V = FolderSearchResults::Visibility;
    const int index = visibility == V::Marks ? 1 : visibility == V::Matches ? 2 : 0;
    // setCurrentIndex fires changeFilteredViewVisibility when it changes; set
    // the model directly too so it holds even if the index was already current.
    visibilityBox_->setCurrentIndex( index );
    if ( activeResults()->visibility() != visibility ) {
        activeResults()->setVisibility( visibility );
    }
}

void FolderCrawlerWidget::refreshAllPanesForMarks()
{
    // Marks live in the shared folderMarks_ store; every pane's mark query +
    // mark provider read it live, so a mark change must repaint/rebuild ALL
    // panes (frozen ones too), not just the active one. refreshForMarksChange is
    // a no-op except under the Marks visibility filter (where it rebuilds the
    // visible row set); forceRefresh always repaints the (possibly changed)
    // bullets.
    for ( auto& pane : panes_ ) {
        if ( pane == nullptr ) {
            continue;
        }
        if ( pane->results != nullptr ) {
            pane->results->refreshForMarksChange();
        }
        if ( pane->view != nullptr ) {
            pane->view->forceRefresh();
        }
    }
    if ( mainView_ != nullptr ) {
        mainView_->forceRefresh();
    }
}

FolderCrawlerWidget::ResultPane* FolderCrawlerWidget::createPane( const QString& title )
{
    // Build a self-contained pane: its own FolderSearchResults +
    // FolderFilteredView + FolderFilteredMarkProvider (pointing at this pane's
    // results + the shared folderMarks_), wired with the same connections the
    // single pane used to get in the ctor. Parenting the view to resultsTabs_
    // lets QTabWidget reparent it into its stack on addTab.
    auto pane = std::make_unique<ResultPane>();
    pane->results = std::make_shared<FolderSearchResults>();
    pane->title = title;
    auto* const view = new FolderFilteredView( pane->results.get(), quickFindPattern_.get(),
                                               resultsTabs_ );
    pane->view = view;

    pane->markProvider = std::make_unique<FolderFilteredMarkProvider>();
    pane->markProvider->marks = &folderMarks_;
    pane->markProvider->results = pane->results.get();
    view->setMarkProvider( pane->markProvider.get() );

    // The Marks visibility filter consults this query (filePath, localLine) ->
    // marked, reading the shared per-file store live (same lambda every pane
    // uses; it captures this, valid for the widget's lifetime).
    pane->results->setMarkedLineQuery( [ this ]( const QString& file, LineNumber line ) {
        const auto it = folderMarks_.find( file );
        return it != folderMarks_.end() && it->count( line.get() ) > 0;
    } );

    // Seed config so a pane created mid-session (Keep) matches the others.
    const auto& config = Configuration::get();
    view->setLineNumbersVisible( config.filteredLineNumbersVisible() );
    QFont font = config.mainFont();
    font.setKerning( false );
    font.setFixedPitch( true );
    if ( config.forceFontAntialiasing() ) {
        font.setStyleStrategy( QFont::PreferAntialias );
    }
    font.setBold( config.useBoldFont() );
    view->updateFont( font );
    view->registerShortcuts();
    if ( quickFindPattern_ != nullptr ) {
        view->setQuickFindPattern( quickFindPattern_.get() );
    }
    view->setSearchPattern( currentSearchPattern_ );

    // Color labels: every pane's results view joins the tab's shared label set
    // (seeded with any labels already set before the pane existed).
    colorLabelsController_.watchView( view );

    // Shared view-signal wiring + hover -> minimap highlight for the results
    // view (single-file wires hover on the filtered view only).
    viewSignalWiring_->wireView( view );
    viewSignalWiring_->wireHover( view );
    view->setControlsSearchLimits( false );

    // Per-pane signal wiring (self-contained: switching tabs needs no re-wiring).
    connect( view, &FolderFilteredView::newSelection, this, &FolderCrawlerWidget::onResultSelected );
    connect( view, &FolderFilteredView::headerClicked, this, &FolderCrawlerWidget::onHeaderClicked );
    connect( view, &AbstractLogView::markLines, this,
             &FolderCrawlerWidget::onFilteredViewMarkLines );
    connect( view, &AbstractLogView::deleteMarkLines, this,
             &FolderCrawlerWidget::onFilteredViewDeleteMarkLines );
    // v is captured by value; the connection is auto-disconnected when the pane
    // results object is destroyed (on pane erase, after the view is deleted).
    connect( pane->results.get(), &FolderSearchResults::layoutChanged, this,
             [ view ]() {
                 view->updateData();
                 view->forceRefresh();
             } );

    const int tabIndex = resultsTabs_->addTab( view, title );
    ResultPane* raw = pane.get();
    panes_.push_back( std::move( pane ) );
    activePaneIndex_ = static_cast<int>( panes_.size() ) - 1;
    // Keep tab index == pane index (append-only; close erases same index).
    resultsTabs_->setCurrentIndex( tabIndex );
    return raw;
}

void FolderCrawlerWidget::onActivePaneChanged( int tabIndex )
{
    // tab index == pane index (append-only + same-index erase). Re-apply the
    // global visibility combo to the now-active pane (all panes share the one
    // toolbar combo).
    if ( panes_.empty() || tabIndex < 0 || tabIndex >= static_cast<int>( panes_.size() ) ) {
        return;
    }
    activePaneIndex_ = tabIndex;
    if ( visibilityBox_ != nullptr ) {
        changeFilteredViewVisibility( visibilityBox_->currentIndex() );
    }
}

void FolderCrawlerWidget::onClosePane( int tabIndex )
{
    if ( panes_.empty() || tabIndex < 0 || tabIndex >= static_cast<int>( panes_.size() ) ) {
        return;
    }
    // Always keep one results surface.
    if ( panes_.size() == 1 ) {
        return;
    }
    // Capture the raw results pointer BEFORE erase so we can clear
    // searchTargetResults_ if the closed pane was the search target.
    const auto* const erased = panes_[ static_cast<size_t>( tabIndex ) ]->results.get();
    FolderFilteredView* const view = panes_[ static_cast<size_t>( tabIndex ) ]->view;

    // Delete the view SYNCHRONOUSLY before its results + mark provider are
    // destroyed: the view holds raw pointers to both (results_ in
    // FolderFilteredView, markProvider_ in AbstractLogView), and erase frees
    // them via the unique_ptr. Safe because this slot runs from
    // tabCloseRequested, not from inside the view's own event handling.
    resultsTabs_->removeTab( tabIndex );
    delete view;
    panes_.erase( panes_.begin() + tabIndex );

    activePaneIndex_ = resultsTabs_->currentIndex();
    if ( searchTargetResults_ == erased ) {
        if ( searchActive_ ) {
            engine_->interrupt();
        }
        searchTargetResults_ = nullptr;
    }
}

bool FolderCrawlerWidget::isMainViewLineMarked( LineNumber line ) const
{
    return mainViewMarkProvider_.isMarked( line );
}

bool FolderCrawlerWidget::isFilteredResultRowMarked( LineNumber row ) const
{
    // Resolves a filtered-view row to (file, localLine) and checks the shared
    // per-file mark store -- the source of truth both the main view and the
    // filtered view render from. Uses the active pane's results.
    auto* results = activeResults();
    if ( results == nullptr ) {
        return false;
    }
    const auto src = results->sourceForLine( row );
    const auto it = folderMarks_.find( src.filePath );
    return it != folderMarks_.end() && it->count( src.localLine.get() ) > 0;
}

void FolderCrawlerWidget::markMainViewLine( LineNumber line )
{
    addMark( currentMainFilePath_, line );
    mainView_->forceRefresh();
}

void FolderCrawlerWidget::unmarkMainViewLine( LineNumber line )
{
    removeMark( currentMainFilePath_, line );
    mainView_->forceRefresh();
}

void FolderCrawlerWidget::saveAsFavorite()
{
    // Mirrors CrawlerWidget::saveAsFavorite: folder search uses the same shared
    // PredefinedFiltersCollection + dialogs as single-file, so saved favorites
    // are available across tab kinds. (The duplication is a candidate for
    // extraction into SearchToolbar.)
    const auto currentText = searchToolbar_->currentSearchText().trimmed();
    if ( currentText.isEmpty() ) {
        return;
    }

    auto filters = PredefinedFiltersCollection::getSynced().getFilters();
    const auto useRegex = searchToolbar_->isUseRegexp();

    SaveFavoriteDialog dialog( currentText, filters, this );
    if ( dialog.exec() != QDialog::Accepted ) {
        return;
    }

    const auto trimmedName = dialog.favoriteName();
    if ( trimmedName.isEmpty() ) {
        return;
    }

    if ( dialog.isCreateNew() ) {
        auto existing = std::find_if(
            filters.begin(), filters.end(), [ &trimmedName ]( const auto& filter ) {
                return filter.name.compare( trimmedName, Qt::CaseInsensitive ) == 0;
            } );

        if ( existing != filters.end() ) {
            const auto isSamePattern = ( existing->pattern == currentText );
            const auto isSameRegex = ( existing->useRegex == useRegex );
            if ( isSamePattern && isSameRegex ) {
                QMessageBox::information(
                    this, tr( "klogg" ),
                    tr( "Favorite \"%1\" already exists with the same content." ).arg( trimmedName ) );
                return;
            }

            FilterDiffDialog diffDialog( trimmedName, *existing, currentText, useRegex, this );
            if ( diffDialog.exec() != QDialog::Accepted ) {
                return;
            }

            existing->pattern = currentText;
            existing->useRegex = useRegex;
        }
        else {
            filters.push_back( { trimmedName, currentText, useRegex } );
        }
    }
    else {
        const int index = dialog.selectedExistingIndex();
        if ( index < 0 || index >= filters.size() ) {
            return;
        }

        auto& existing = filters[ index ];

        const auto isSamePattern = ( existing.pattern == currentText );
        const auto isSameRegex = ( existing.useRegex == useRegex );
        if ( isSamePattern && isSameRegex ) {
            QMessageBox::information(
                this, tr( "klogg" ),
                tr( "Favorite \"%1\" already has the same content." ).arg( existing.name ) );
            return;
        }

        FilterDiffDialog diffDialog( existing.name, existing, currentText, useRegex, this );
        if ( diffDialog.exec() != QDialog::Accepted ) {
            return;
        }

        existing.pattern = currentText;
        existing.useRegex = useRegex;
    }

    PredefinedFiltersCollection::getSynced().saveToStorage( filters );
    reloadPredefinedFilters();
}

void FolderCrawlerWidget::updatePredefinedFiltersWidget()
{
    searchToolbar_->predefinedFilters()->updateSearchPattern( searchToolbar_->currentSearchText(),
                                                              searchToolbar_->isBoolean() );
}

void FolderCrawlerWidget::reloadPredefinedFilters() const
{
    searchToolbar_->predefinedFilters()->populatePredefinedFilters();
}

void FolderCrawlerWidget::applyConfiguration()
{
    // Mirrors CrawlerWidget::applyConfiguration (crawlerwidget.cpp:811-855) for
    // the subset relevant to folder mode: re-apply line-number visibility, font,
    // and overview to both views, and (re-)register view shortcuts. Called on
    // construction and whenever MainWindow emits optionsChanged (the View-menu
    // toggles), via a direct, deduplicated connection -- the folder is
    // intentionally NOT a signalMux document (it lacks the file/live-source
    // slots the mux routes), so optionsChanged is delivered directly.
    const auto& config = Configuration::get();

    QFont font = config.mainFont();
    font.setKerning( false );
    font.setFixedPitch( true );
    if ( config.forceFontAntialiasing() ) {
        font.setStyleStrategy( QFont::PreferAntialias );
    }
    font.setBold( config.useBoldFont() );

    mainView_->setLineNumbersVisible( config.mainLineNumbersVisible() );
    overview_.setVisible( config.isOverviewVisible() );
    mainView_->refreshOverview();
    mainView_->updateFont( font );
    // Apply font + line-numbers to EVERY pane (Keep-results snapshots included),
    // mirroring CrawlerWidget looping over all result tabs.
    for ( auto& pane : panes_ ) {
        if ( pane != nullptr && pane->view != nullptr ) {
            pane->view->setLineNumbersVisible( config.filteredLineNumbersVisible() );
            pane->view->updateFont( font );
        }
    }

    // Re-sync the predefined-filters combo from the shared collection, mirroring
    // CrawlerWidget::applyConfiguration (crawlerwidget.cpp:866). A filter saved
    // via another tab (or the options dialog) must appear here on the next
    // optionsChanged broadcast without restarting klogg.
    reloadPredefinedFilters();

    registerShortcuts();
}

void FolderCrawlerWidget::registerShortcuts()
{
    for ( auto& shortcut : shortcuts_ ) {
        shortcut.second->deleteLater();
    }
    shortcuts_.clear();

    // Widget-level crawler family (visibility cycling, option toggles,
    // keep-results, top-view resize, Esc refocus): shared with CrawlerWidget
    // via klogg::registerCrawlerShortcuts. Auto-refresh is excluded because
    // the folder toolbar hides that button.
    klogg::CrawlerShortcutHooks hooks;
    hooks.visibilityBox = [ this ]() { return visibilityBox_; };
    hooks.searchToolbar = [ this ]() { return searchToolbar_; };
    hooks.changeTopViewSize = [ this ]( int delta ) { changeTopViewSize( delta ); };
    hooks.activeView = [ this ]() { return activeView(); };
    hooks.includeAutoRefresh = false;
    klogg::registerCrawlerShortcuts( this, shortcuts_, hooks );

    // Register view-level keyboard shortcuts on both views, mirroring
    // CrawlerWidget (crawlerwidget.cpp:1725-1726). Without this, arrow keys,
    // PgUp/PgDn, jump-to-top/bottom, and the other AbstractLogView shortcuts are
    // not active in folder views.
    mainView_->registerShortcuts();
    for ( auto& pane : panes_ ) {
        if ( pane != nullptr && pane->view != nullptr ) {
            pane->view->registerShortcuts();
        }
    }

    // Widget-level color-label shortcuts (1..9 add, 0 remove, Cmd+D next,
    // Cmd+Shift+0 clear) — the single-file CrawlerWidget registers these on
    // itself; the shared controller does the same for this tab.
    colorLabelsController_.registerShortcuts();
}

void FolderCrawlerWidget::changeTopViewSize( int delta )
{
    if ( splitter_ == nullptr ) {
        return;
    }

    // CrawlerWidget uses QSplitter::moveSplitter (it IS the splitter); here the
    // splitter is a child, so adjust sizes directly. QSplitter clamps to the
    // children's minimum sizes, mirroring closestLegalPosition.
    auto sizes = splitter_->sizes();
    if ( sizes.size() != 2 ) {
        return;
    }

    const int step = delta * 10;
    const int total = sizes.at( 0 ) + sizes.at( 1 );
    int newTop = sizes.at( 0 ) + step;
    newTop = qBound( 0, newTop, total );
    splitter_->setSizes( { newTop, total - newTop } );
}

AbstractLogView* FolderCrawlerWidget::activeView() const
{
    // The focused view if one of ours has focus, else the active results view
    // (the primary folder surface). Mirrors CrawlerWidget::activeView. Only the
    // visible tab's view can hold focus, so activeFilteredView() is the only
    // candidate among panes.
    auto* const fv = activeFilteredView();
    if ( ( mainView_ != nullptr && mainView_->hasFocus() )
         || ( fv != nullptr && fv->hasFocus() ) ) {
        return qobject_cast<AbstractLogView*>( QApplication::focusWidget() );
    }
    return fv;
}

SearchableWidgetInterface* FolderCrawlerWidget::doGetActiveSearchable() const
{
    // activeView() is an AbstractLogView, which IS-A SearchableWidgetInterface.
    return activeView();
}

std::vector<QObject*> FolderCrawlerWidget::doGetAllSearchables() const
{
    // Both views are QObjects; the QuickFindMux registers pattern listeners on
    // each so incremental search works in whichever the user is focused on. Only
    // the active (visible) results pane can hold focus / be searched.
    return { mainView_, activeFilteredView() };
}

QString FolderCrawlerWidget::getSelectedText() const
{
    auto* view = activeView();
    return view != nullptr ? view->getSelectedText() : QString{};
}

bool FolderCrawlerWidget::isPartialSelection() const
{
    auto* view = activeView();
    return view != nullptr ? view->isPartialSelection() : false;
}

void FolderCrawlerWidget::selectAll()
{
    if ( auto* view = activeView() ) {
        view->selectAll();
    }
}

void FolderCrawlerWidget::setEncoding( std::optional<int> mib )
{
    // Apply the chosen encoding to the file currently shown in the main view
    // (mirrors CrawlerWidget::updateEncoding for the opened-file case). No-op
    // until a file is opened (currentMainData_ is the placeholder). The folder
    // results view decodes via its own per-file detected codec
    // (FileGroup::sourceCodec) and is unaffected.
    if ( currentMainData_ == nullptr || currentMainData_ == placeholderData_ ) {
        return;
    }
    encodingMibOverride_ = mib;
    QTextCodec* codec = nullptr;
    if ( mib.has_value() ) {
        codec = QTextCodec::codecForMib( *mib );
    }
    else {
        codec = currentMainData_->getDetectedEncoding();
    }
    if ( codec == nullptr ) {
        codec = QTextCodec::codecForName( "UTF-8" );
    }
    currentMainData_->setDisplayEncoding( codec->name() );
    mainView_->forceRefresh();
    Q_EMIT mainViewFileChanged();
}

std::optional<int> FolderCrawlerWidget::encodingMib() const
{
    return encodingMibOverride_;
}

void FolderCrawlerWidget::focusSearchEdit()
{
    // Mirrors CrawlerWidget::focusSearchEdit.
    searchToolbar_->searchLineEdit()->lineEdit()->setFocus( Qt::ShortcutFocusReason );
}

void FolderCrawlerWidget::goToLine()
{
    // Mirrors CrawlerWidget::goToLine, main view only: the results view is a
    // cross-file grep listing whose rows do not map to source line numbers.
    bool isLineSelected = true;
    const auto newLine = QInputDialog::getText( this, "Jump to line", "Line number" )
                             .toULongLong( &isLineSelected );

    if ( isLineSelected ) {
        const auto selectedLine = LineNumber(
            static_cast<LineNumber::UnderlyingType>( newLine > 0 ? newLine - 1 : 0 ) );
        mainView_->trySelectLine( selectedLine );
    }
}

void FolderCrawlerWidget::textWrapSet( bool checked )
{
    // Fan out to the main view and every results pane (including frozen
    // keep-results panes), mirroring CrawlerWidget's signal relay to its views.
    mainView_->textWrapSet( checked );
    for ( auto& pane : panes_ ) {
        if ( pane != nullptr && pane->view != nullptr ) {
            pane->view->textWrapSet( checked );
        }
    }
}

bool FolderCrawlerWidget::isTextWrapEnabled() const
{
    return mainView_->isTextWrapEnabled();
}

void FolderCrawlerWidget::enteringQuickFind()
{
    // Remember who had the focus (only if it is one of our views), mirroring
    // CrawlerWidget::enteringQuickFind.
    QWidget* const focusWidget = QApplication::focusWidget();
    if ( focusWidget == mainView_ ) {
        qfSavedFocus_ = mainView_;
        return;
    }
    qfSavedFocus_ = nullptr;
    for ( const auto& pane : panes_ ) {
        if ( pane != nullptr && pane->view == focusWidget ) {
            qfSavedFocus_ = pane->view;
            break;
        }
    }
}

void FolderCrawlerWidget::exitingQuickFind()
{
    // Restore the focus once the QFBar has been hidden.
    if ( qfSavedFocus_ != nullptr ) {
        qfSavedFocus_->setFocus();
    }
}

void FolderCrawlerWidget::applyDetectedEncoding()
{
    // Mirror CrawlerWidget::updateEncoding (crawlerwidget.cpp:1873) for the
    // auto-open path: bridge the indexer-detected encoding
    // (IndexingData::encodingGuess_, exposed via getDetectedEncoding) into the
    // display decoder (LogData::codec_). Without this bridge a non-UTF-8 file is
    // indexed with correct line positions but displayed decoded as UTF-8
    // (mojibake) and the info line reports the wrong codec. doSetDisplayEncoding
    // short-circuits (no re-index) when the display codec already equals the
    // detected guess, so this is a no-op for UTF-8 files.
    if ( currentMainData_ == nullptr || currentMainData_ == placeholderData_ ) {
        return;
    }
    auto* codec = currentMainData_->getDetectedEncoding();
    if ( codec == nullptr ) {
        codec = QTextCodec::codecForName( "UTF-8" );
    }
    currentMainData_->setDisplayEncoding( codec->name() );
}

void FolderCrawlerWidget::clearSearchHistory()
{
    // Mirrors CrawlerWidget::clearSearchHistory (crawlerwidget.cpp:478).
    searchToolbar_->searchLineEdit()->clear();
    if ( savedSearches_ != nullptr ) {
        auto& searches = SavedSearches::getSynced();
        savedSearches_->clear();
        searches.save();
    }
    searchToolbar_->setItems( {} );
}

void FolderCrawlerWidget::editSearchHistory()
{
    // Mirrors CrawlerWidget::editSearchHistory (crawlerwidget.cpp:492). The host
    // owns the shared SavedSearches; the toolbar owns the dropdown model.
    if ( savedSearches_ == nullptr ) {
        return;
    }
    auto& searches = SavedSearches::getSynced();

    const auto history = savedSearches_->recentSearches().join( QChar::LineFeed );
    bool ok = false;
    const auto newHistory = QInputDialog::getMultiLineText( this, tr( "klogg" ),
                                                            tr( "Search history:" ), history, &ok );

    if ( ok ) {
        savedSearches_->clear();
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
        const auto items = newHistory.split( QChar::LineFeed, Qt::SkipEmptyParts );
#else
        const auto items = newHistory.split( QChar::LineFeed, QString::SkipEmptyParts );
#endif
        std::for_each( items.rbegin(), items.rend(), [ this ]( const auto& item ) {
            savedSearches_->addRecent( item );
        } );
    }
    searches.save();
    searchToolbar_->setItems( savedSearches_->recentSearches() );
}

void FolderCrawlerWidget::searchFor( const QString& pattern )
{
    // Set the pattern text WITHOUT triggering setSearchPattern's auto-run /
    // reset side effects (which would double-fire startSearch when
    // autoRunSearchOnPatternChange is on), then start exactly one search.
    auto* combo = searchToolbar_->searchLineEdit();
    const bool wasBlocked = combo->blockSignals( true );
    combo->setEditText( pattern );
    combo->blockSignals( wasBlocked );
    startSearch();
}

void FolderCrawlerWidget::selectResultRow( LineNumber line )
{
    onResultSelected( line, 1_lcount, 0_lcol, 0_length );
}

void FolderCrawlerWidget::clickHeaderRow( LineNumber line )
{
    onHeaderClicked( line );
}

void FolderCrawlerWidget::doSetData( std::shared_ptr<SearchableLogData>,
                                     std::shared_ptr<LogFilteredData> )
{
    // Single-file data injection does not apply to folder mode.
}

void FolderCrawlerWidget::doSetQuickFindPattern( std::shared_ptr<QuickFindPattern> qfp )
{
    // Accept the session-wide QuickFindPattern and RE-POINT both views to it
    // (AbstractLogView::setQuickFindPattern), so the app-wide QuickFindMux --
    // which drives this pattern -- actually drives the folder's views. Without
    // the rebind the views keep the ctor-local pattern the mux never updates,
    // and the Ctrl+F QuickFind bar is inert on folder tabs.
    //
    // Re-point the views BEFORE replacing quickFindPattern_: the views hold RAW
    // pointers into the current pattern, and AbstractLogView::setQuickFindPattern
    // disconnects that pattern's patternUpdated signal. Reassigning the member
    // first would drop the last shared_ptr (the views don't own one), freeing
    // the pattern mid-disconnect -- a use-after-free. Keeping the member alive
    // until after the rebind avoids that.
    if ( qfp != nullptr ) {
        if ( mainView_ != nullptr ) {
            mainView_->setQuickFindPattern( qfp.get() );
        }
        // Re-point EVERY pane so QuickFind works on frozen tabs too.
        for ( auto& pane : panes_ ) {
            if ( pane != nullptr && pane->view != nullptr ) {
                pane->view->setQuickFindPattern( qfp.get() );
            }
        }
        quickFindPattern_ = std::move( qfp );
    }
}

void FolderCrawlerWidget::doSetSavedSearches( SavedSearches* saved_searches )
{
    // Wire the session-wide search history into the toolbar so the folder's
    // recent grep patterns appear in the dropdown (and startSearch records into
    // it). The toolbar was constructed with null SavedSearches (no history);
    // setSearchHistory + setItems populate it post-construction.
    savedSearches_ = saved_searches;
    if ( searchToolbar_ != nullptr ) {
        searchToolbar_->setSearchHistory( saved_searches );
        if ( saved_searches != nullptr ) {
            searchToolbar_->setItems( saved_searches->recentSearches() );
        }
    }
}

void FolderCrawlerWidget::doSetViewContext( const QString& view_context )
{
    // Parse the stored context and restore the pattern text + option toggles
    // WITHOUT auto-running a search (one Enter re-runs). Mirrors searchFor's
    // blockSignals+setEditText pattern (foldercrawlerwidget.cpp) minus the
    // startSearch() call, and CrawlerWidget::doSetViewContext's toggle restore.
    const FolderCrawlerContext context{ view_context };

    if ( splitter_ != nullptr && !context.sizes().isEmpty() ) {
        // Per-tab session sizes beat the global default seeded on first
        // geometry. Restore can run while the tab is still hidden (pre-layout
        // setSizes is discarded), so the sizes are stashed and applied by the
        // event filter on the splitter's first Resize; if the splitter already
        // has real geometry, apply immediately.
        pendingSplitterSizes_ = context.sizes();
        if ( splitterSeedApplied_ ) {
            splitter_->setSizes( pendingSplitterSizes_ );
        }
    }

    // Restore toggles first (idempotent; optionsChanged is not wired to a
    // search in folder mode, so no search is triggered).
    searchToolbar_->setMatchCase( !context.ignoreCase() );
    searchToolbar_->setUseRegexp( context.useRegexp() );
    searchToolbar_->setInverse( context.inverse() );
    searchToolbar_->setBoolean( context.boolean() );

    // Restore the pattern text into the combo without emitting searchRequested
    // (blockSignals so setEditText does not fire auto-run on pattern change).
    if ( !context.pattern().isNull() ) {
        auto* combo = searchToolbar_->searchLineEdit();
        const bool wasBlocked = combo->blockSignals( true );
        combo->setEditText( context.pattern() );
        combo->blockSignals( wasBlocked );
    }
}

std::shared_ptr<const ViewContextInterface> FolderCrawlerWidget::doGetViewContext() const
{
    // Build the context from the live toolbar state. NEVER return null: the
    // session save path dereferences this (WindowSession::save calls
    // view_context->toString()), and a null shared_ptr crashes it.
    QList<int> sizes;
    if ( splitter_ != nullptr ) {
        sizes = splitter_->sizes();
    }

    // searchToolbar_ is constructed unconditionally in the ctor and never
    // reset, so it is always non-null here -- no null-guard needed (mirrors
    // CrawlerWidget, which derefs searchToolbar_ unconditionally throughout).
    auto context = std::make_shared<const FolderCrawlerContext>(
        searchToolbar_->currentSearchText(),
        !searchToolbar_->isMatchCase(),
        searchToolbar_->isUseRegexp(),
        searchToolbar_->isInverse(),
        searchToolbar_->isBoolean(), sizes );

    return static_cast<std::shared_ptr<const ViewContextInterface>>( context );
}

void FolderCrawlerWidget::startSearch()
{
    if ( filePaths_.isEmpty() ) {
        return;
    }

    // A new search clears a previous invalid-pattern error state.
    statusErrorActive_ = false;
    statusLabel_->setPalette( statusLabelDefaultPalette_ );
    statusLabel_->setAutoFillBackground( false );

    // Parity with CrawlerWidget::replaceCurrentSearch: a new search returns a
    // marks-only view to "Marks and matches" so the fresh matches are visible.
    if ( visibilityBox_->currentIndex() == 1 ) {
        visibilityBox_->setCurrentIndex( 0 );
    }

    const auto pattern = searchToolbar_->currentSearchText();
    if ( pattern.isEmpty() ) {
        // Supersede any in-flight scan BEFORE mutating state: interrupt it and
        // bump the generation so its queued progress/finish signals are
        // treated as stale (parity with CrawlerWidget::replaceCurrentSearch,
        // crawlerwidget.cpp:1566-1567). The pane is detached so no late group
        // can re-append to it after the clear.
        engine_->interrupt();
        currentSearchGeneration_ = engine_->bumpGeneration();
        searchTargetResults_ = nullptr;

        // Parity with CrawlerWidget::replaceCurrentSearch(""): an empty search
        // clears the results pane instead of leaving stale results behind.
        // (Single-file additionally honors showAllInFilteredViewWhenSearchEmpty
        // by dumping the open file unfiltered; that preference is specific to
        // the single-file lens view and is intentionally not mirrored into the
        // cross-file grep results pane.)
        searchToolbar_->setSearchInProgress( false );
        searchActive_ = false;
        currentSearchPattern_ = {};
        lastResultStatusText_.clear();
        if ( auto* const results = activeResults() ) {
            results->beginSearch( filePaths_ );
        }
        updateReadyStatus();
        return;
    }

    // Record the search into the shared history (persisted to disk, parity
    // with single-file search; the toolbar no-ops without an injected
    // SavedSearches).
    searchToolbar_->recordSearch();

    searchToolbar_->setSearchInProgress( true );
    searchActive_ = true;

    // Keep results: if checked AND the current pane already has results,
    // snapshot it (it becomes a frozen tab) and start the new search in a fresh
    // pane. Mirrors CrawlerWidget::startNewSearch. Reset the toggle so the next
    // search defaults to overwriting the active pane (parity with single-file).
    if ( searchToolbar_->isKeepResultsChecked() ) {
        searchToolbar_->setKeepResultsChecked( false );
        const bool hasResults
            = activeResults() != nullptr && activeResults()->getNbLine().get() > 0;
        if ( hasResults ) {
            if ( resultsTabs_ != nullptr && !currentSearchPattern_.pattern.isEmpty() ) {
                resultsTabs_->setTabText(
                    resultsTabs_->currentIndex(),
                    QStringLiteral( "Find \"%1\"" ).arg( currentSearchPattern_.pattern ) );
            }
            createPane( QString() );
        }
    }

    // The pane the new search streams into. Set BEFORE engine_->startSearch so
    // the first queued signal cannot arrive before the target is pinned.
    searchTargetResults_ = activeResults();
    // Reset the view for streaming: beginSearch sizes the pending buffer and
    // clears any prior result set so file groups stream into a clean view. This
    // also covers the invalid-pattern path (which never emits searchStarted).
    if ( searchTargetResults_ != nullptr ) {
        searchTargetResults_->beginSearch( filePaths_ );
    }

    // Build the pattern from the toolbar's option flags (case / regex / inverse
    // / boolean). This UPGRADES folder search from the former plain-text
    // RegularExpressionPattern{ pattern } to honor the toggles for free.
    const auto regexpPattern = searchToolbar_->currentRegularExpressionPattern();

    // Parity with CrawlerWidget::replaceCurrentSearch: an invalid pattern
    // clears stale results (beginSearch above) and surfaces the error instead
    // of silently scanning to zero matches.
    const RegularExpression validation{ regexpPattern };
    if ( !validation.isValid() ) {
        // Supersede any in-flight scan (see the empty-pattern path above).
        engine_->interrupt();
        currentSearchGeneration_ = engine_->bumpGeneration();
        searchTargetResults_ = nullptr;

        searchToolbar_->setSearchInProgress( false );
        searchActive_ = false;
        currentSearchPattern_ = {};
        lastResultStatusText_.clear();
        // Parity with CrawlerWidget: clear the stale highlight in both views.
        if ( activeFilteredView() != nullptr ) {
            activeFilteredView()->setSearchPattern( {} );
        }
        if ( mainView_ != nullptr ) {
            mainView_->setSearchPattern( {} );
        }
        statusErrorActive_ = true;
        // Same error styling as CrawlerWidget's searchInfoLine_ (dark yellow).
        statusLabel_->setPalette( QPalette( Qt::darkYellow ) );
        statusLabel_->setAutoFillBackground( true );
        statusLabel_->setText( tr( "Error in expression: %1" ).arg( validation.errorString() ) );
        return;
    }

    // Context is a scan-time property: resolve the current -A/-B/-C window from
    // the toolbar so programmatic startSearch (searchFor, session restore) also
    // honors it, not just direct control edits.
    updateContextFromControls();
    currentSearchGeneration_ = engine_->startSearch(
        filePaths_, regexpPattern, klogg::folder::ContextOptions{ contextBefore_, contextAfter_ } );
    // Store + forward the pattern to BOTH views so that the matched substring
    // is highlighted in the filtered results AND in any file already open in
    // the main view (single-file parity: crawlerwidget.cpp forwards to both
    // logMainView_ and filteredView_). Storing here also lets openFileInMainView
    // re-apply the pattern right after each setDataSource swap.
    currentSearchPattern_ = regexpPattern;
    if ( activeFilteredView() != nullptr ) {
        activeFilteredView()->setSearchPattern( regexpPattern );
    }
    if ( mainView_ != nullptr ) {
        mainView_->setSearchPattern( regexpPattern );
    }
    if ( resultsTabs_ != nullptr ) {
        resultsTabs_->setTabText( resultsTabs_->currentIndex(),
                                  QStringLiteral( "Find \"%1\"" ).arg( pattern ) );
    }
}

void FolderCrawlerWidget::stopSearch()
{
    engine_->interrupt();
}

void FolderCrawlerWidget::onSearchStarted( quint64 generation )
{
    if ( generation != currentSearchGeneration_ ) {
        return; // stale: a superseded scan (empty/invalid pattern replaced it)
    }
    statusLabel_->setText( tr( "Searching..." ) );
}

void FolderCrawlerWidget::onSearchProgressed( quint64 nbMatches, int percent, quint64 generation )
{
    if ( generation != currentSearchGeneration_ ) {
        return; // stale: a superseded scan (empty/invalid pattern replaced it)
    }
    statusLabel_->setText( tr( "%1 match(es)  %2%" ).arg( static_cast<qulonglong>( nbMatches ) ).arg( percent ) );
}

void FolderCrawlerWidget::onSearchFinished( quint64 generation )
{
    if ( generation != currentSearchGeneration_ ) {
        return; // stale: superseded by a newer search
    }

    // Defensive safety net: commit any groups still buffered (interrupted
    // scans may leave a predecessor that never arrived). Under normal
    // completion all per-file groups were already committed incrementally via
    // onFileGroupReady before searchFinished was emitted. Stream into the pane
    // the search targets (NOT necessarily the active one if the user switched
    // tabs mid-search).
    if ( searchTargetResults_ != nullptr ) {
        searchTargetResults_->flushPending();
    }

    searchToolbar_->setSearchInProgress( false );
    searchActive_ = false;

    const quint64 total = engine_->matchCount();
    if ( total == 0 ) {
        lastResultStatusText_ = tr( "No matches" );
    }
    else {
        lastResultStatusText_ = tr( "%1 match(es)" ).arg( static_cast<qulonglong>( total ) );
    }
    statusLabel_->setText( lastResultStatusText_ );
}

void FolderCrawlerWidget::onFileGroupReady( int fileIndex, quint64 generation )
{
    if ( generation != currentSearchGeneration_ ) {
        return; // stale: superseded by a newer search
    }
    auto group = engine_->takeCompletedGroup( fileIndex );
    if ( group.has_value() && searchTargetResults_ != nullptr ) {
        searchTargetResults_->addFileGroup( fileIndex, std::move( *group ) );
    }
}

void FolderCrawlerWidget::onResultSelected( LineNumber line, LinesCount nLines,
                                            LineColumn startCol, LineLength nSymbols )
{
    auto* results = activeResults();
    if ( results == nullptr || line >= results->getNbLine() ) {
        return;
    }
    if ( results->lineKind( line ) != LineKind::Data ) {
        return;
    }
    const auto source = results->sourceForLine( line );

    // Single-file parity (CrawlerWidget::jumpToMatchingLine): `line` is the
    // drag's moving end; the range/portion payload is forwarded unchanged --
    // the main view selects that source line and re-emits the payload for the
    // status bar. Recomputing a span from neighboring rows would inflate the
    // payload for non-contiguous matches (and mis-handle upward drags).
    openFileInMainView( source.filePath, source.localLine, nLines, startCol, nSymbols );
}

void FolderCrawlerWidget::selectInMainView( LineNumber line, LinesCount nLines,
                                            LineColumn startCol, LineLength nSymbols )
{
    if ( nSymbols.get() > 0 || nLines.get() > 1 ) {
        mainView_->selectPortionAndDisplayLine( line, nLines, startCol, nSymbols );
    }
    else {
        mainView_->selectAndDisplayLine( line );
    }
}

void FolderCrawlerWidget::highlightOverviewForRow( const AbstractLogView* view, LineNumber row )
{
    // The hovered results row resolves to (file, localLine) via the pane that
    // owns the view; highlight it in the overview only when that file is the
    // one currently open in the main view (the overview is per-file). Any row
    // that does not resolve to the open file CLEARS the previous highlight --
    // otherwise a stale marker lingers while hovering header/foreign rows
    // (single-file's hover hook re-targets or clears on every hover).
    for ( const auto& pane : panes_ ) {
        if ( pane == nullptr || pane->view != view || pane->results == nullptr ) {
            continue;
        }
        if ( row < pane->results->getNbLine()
             && pane->results->lineKind( row ) == LineKind::Data ) {
            const auto source = pane->results->sourceForLine( row );
            if ( source.filePath == currentMainFilePath_ && overviewWidget_ != nullptr ) {
                overviewWidget_->highlightLine( source.localLine );
                return;
            }
        }
        if ( overviewWidget_ != nullptr ) {
            overviewWidget_->removeHighlight();
        }
        return;
    }
}

void FolderCrawlerWidget::onHeaderClicked( LineNumber line )
{
    auto* results = activeResults();
    if ( results == nullptr ) {
        return;
    }
    const auto fileId = results->fileIdForLine( line );
    if ( fileId < 0 ) {
        return;
    }
    results->toggleCollapse( fileId ); // emits layoutChanged -> view refreshes
}

void FolderCrawlerWidget::collapseAll()
{
    if ( activeResults() != nullptr ) {
        activeResults()->collapseAll();
    }
}

void FolderCrawlerWidget::expandAll()
{
    if ( activeResults() != nullptr ) {
        activeResults()->expandAll();
    }
}

void FolderCrawlerWidget::openFileInMainView( const QString& filePath, LineNumber localLine,
                                              LinesCount nLines, LineColumn startCol,
                                              LineLength nSymbols )
{
    if ( filePath.isEmpty() ) {
        return;
    }

    // Already showing this file -> just jump.
    if ( filePath == currentMainFilePath_ && currentMainData_ != nullptr ) {
        lastMainViewLine_ = localLine;
        // select+announce (not just scroll) so the Ln: field and the selection
        // highlight track the clicked result -- parity with single-file, whose
        // main view selects the line a filtered-view click jumps to.
        selectInMainView( localLine, nLines, startCol, nSymbols );
        return;
    }

    // A different file is about to be shown: the encoding override belongs to
    // the previously displayed file, so it resets to auto-detect (the new
    // file's detected codec). Mirrors single-file, where the override lives
    // and dies with the one document in the tab.
    encodingMibOverride_.reset();

    // Cached (recently opened) -> swap instantly. Promote to most-recently-used
    // so the LRU eviction policy keeps it resident.
    const auto it = mainViewCache_.find( filePath );
    if ( it != mainViewCache_.end() ) {
        mainViewCacheOrder_.splice( mainViewCacheOrder_.begin(), mainViewCacheOrder_,
                                    it.value().second );
        currentMainData_ = it.value().first;
        currentMainFilePath_ = filePath;
        lastMainViewLine_ = localLine;
        mainView_->setDataSource( currentMainData_.get() );
        // Re-apply the current search pattern so the swapped-in (cached) file
        // highlights its matches at first paint (idempotent: setDataSource does
        // not reset searchPattern_, this is the explicit parity guarantee).
        mainView_->setSearchPattern( currentSearchPattern_ );
        // A cached file already had its display codec synced on first load, so
        // no applyDetectedEncoding() here.
        selectInMainView( localLine, nLines, startCol, nSymbols );
        refreshFileOverview( filePath );
        Q_EMIT mainViewFileChanged();
        return;
    }

    // Not loaded yet -> index the file asynchronously, then swap + jump.
    pendingMainFilePath_ = filePath;
    pendingJumpLine_ = localLine;
    pendingJumpNLines_ = nLines;
    pendingJumpCol_ = startCol;
    pendingJumpLen_ = nSymbols;
    pendingMainData_ = std::make_shared<LogData>();

    connect( pendingMainData_.get(), &SearchableLogData::loadingFinished, this,
             [ this ]( LoadingStatus ) {
                 if ( pendingMainData_ == nullptr ) {
                     return;
                 }
                 currentMainData_ = pendingMainData_;
                 currentMainFilePath_ = pendingMainFilePath_;
                 lastMainViewLine_ = pendingJumpLine_;
                 cacheMainViewData( currentMainFilePath_, currentMainData_ );

                 // Sync the display codec to the indexer-detected encoding
                 // BEFORE setDataSource renders, so a non-UTF-8 file decodes
                 // correctly at first paint and the info line reports the right
                 // encoding (parity with CrawlerWidget::updateEncoding, which
                 // single-file calls from its own loadingFinished). Idempotent
                 // for UTF-8 (detected == default codec -> no reload).
                 applyDetectedEncoding();
                 // Any override the user picked while this load was in flight
                 // belonged to the previously displayed file: drop it so the
                 // encoding menu does not check a codec the new file does not
                 // actually use.
                 encodingMibOverride_.reset();

                 mainView_->setDataSource( currentMainData_.get() );
                 // Re-apply the current search pattern so the freshly-indexed
                 // file highlights its matches at first paint. Runs on the main
                 // thread (loadingFinished is a queued signal), so threading is
                 // correct.
                 mainView_->setSearchPattern( currentSearchPattern_ );
                 selectInMainView( pendingJumpLine_, pendingJumpNLines_, pendingJumpCol_,
                                   pendingJumpLen_ );
                 // getNbLine() is only valid now that indexing finished: the
                 // overview repoint MUST happen here, not at attachFile time.
                 refreshFileOverview( pendingMainFilePath_ );
                 Q_EMIT mainViewFileChanged();

                 pendingMainData_.reset();
                 pendingMainFilePath_.clear();
             } );

    // The opened file's path/size/date surface in MainWindow's info line; keep
    // the toolbar status focused on search state (no path leak here).
    pendingMainData_->attachFile( filePath );
}

void FolderCrawlerWidget::refreshFileOverview( const QString& filePath )
{
    // No-op until a real file is loaded and the overview is user-visible. Uses
    // the active pane's matches (the overview belongs to whatever results the
    // user is browsing).
    auto* results = activeResults();
    if ( !overview_.isVisible() || currentMainData_ == nullptr || results == nullptr ) {
        return;
    }
    overview_.setMatchLines( results->matchLinesForFile( filePath ) );
    overview_.updateData( currentMainData_->getNbLine() );
    mainView_->refreshOverview();
}

void FolderCrawlerWidget::cacheMainViewData( const QString& filePath, std::shared_ptr<LogData> logData )
{
    if ( filePath.isEmpty() ) {
        return;
    }
    // Insert at the front (most-recently-used). If the file was already cached,
    // drop its prior order entry first so we do not leak stale list nodes.
    const auto existing = mainViewCache_.find( filePath );
    if ( existing != mainViewCache_.end() ) {
        mainViewCacheOrder_.erase( existing.value().second );
    }
    mainViewCacheOrder_.push_front( filePath );
    mainViewCache_[ filePath ] = { std::move( logData ), mainViewCacheOrder_.begin() };

    // Evict least-recently-used entries (back of the order list) to bound memory.
    while ( mainViewCache_.size() > static_cast<qsizetype>( MainViewCacheLimit ) ) {
        const auto lru = mainViewCacheOrder_.back();
        mainViewCache_.remove( lru );
        mainViewCacheOrder_.pop_back();
    }
}
