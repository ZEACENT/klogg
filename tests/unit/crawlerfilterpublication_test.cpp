#include <catch2/catch.hpp>

#include <future>
#include <mutex>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QUuid>

#include "crawlerwidget.h"
#include "configuration.h"
#include "logdata.h"
#include "preparedfilesource.h"
#include "quickfindpattern.h"
#include "savedsearches.h"
#include "streaminglogdata.h"
#include "test_utils.h"

class CrawlerFilterPublicationAccess;
class CrawlerQueuedSearchAccess;
template <>
struct LogFilteredData::access_by<CrawlerQueuedSearchAccess> {
    static LogFilteredDataWorker& worker( LogFilteredData& model ) { return model.workerThread_; }
};
template <>
struct LogFilteredDataWorker::access_by<CrawlerQueuedSearchAccess> {
    static void holdConsumer( LogFilteredDataWorker& worker )
    {
        worker.shutdownAndWait();
        // Match the existing worker state-machine fixture: queue real requests
        // after the consumer has joined. Production shutdown remains terminal.
        std::lock_guard<std::mutex> lock( worker.requestMutex_ );
        worker.dispatchShutdown_ = false;
    }
    static bool pending( LogFilteredDataWorker& worker )
    {
        std::lock_guard<std::mutex> lock( worker.requestMutex_ );
        return worker.requestPending_;
    }
};
template <>
struct CrawlerWidget::access_by<CrawlerFilterPublicationAccess> {
    static void showOnlyMarks( CrawlerWidget& owner ) { owner.visibilityBox_->setCurrentIndex( 1 ); }
    static void setContext( CrawlerWidget& owner, int mode, int count )
    {
        owner.contextLinesComboBox_->setCurrentIndex( mode );
        owner.contextLinesSpinBox_->setValue( count );
    }
    static bool hasNoSearch( const CrawlerWidget& owner )
    {
        return owner.searchState_.getState() == CrawlerWidget::SearchState::NoSearch;
    }
};

namespace {
struct CrawlerFilterFixture {
    QTemporaryDir root;
    std::shared_ptr<SearchableLogData> source;
    std::shared_ptr<LogFilteredData> filtered;
    std::shared_ptr<QuickFindPattern> quick = std::make_shared<QuickFindPattern>();
    std::unique_ptr<CrawlerWidget> widget;
    AtomicFlag cancelled;
    explicit CrawlerFilterFixture( bool live = false )
    {
        REQUIRE( root.isValid() );
        const QByteArray text{ "ERROR first\nplain\nERROR third\nlast\n" };
        if ( live ) {
            source = std::make_shared<StreamingLogData>(
                QUuid::createUuid().toString( QUuid::WithoutBraces ), root.path() );
        } else {
            const auto path = root.filePath( "source.log" );
            QFile file{ path };
            REQUIRE( file.open( QIODevice::WriteOnly ) );
            REQUIRE( file.write( text ) == text.size() );
            file.close();
            const auto settings = IndexingSettings::capture();
            auto prepared = std::async( std::launch::async, [&] {
                return prepareFileSource( path, settings, cancelled );
            } ).get();
            REQUIRE( prepared.source );
            source = LogData::fromPrepared( prepared.source );
        }
        REQUIRE( source );
        filtered = source->getNewFilteredData();
        quick->changeSearchPattern( QStringLiteral( "ERROR" ) );
        widget = std::make_unique<CrawlerWidget>( nullptr, CrawlerWidget::InitialFocusPolicy::Preserve );
        widget->setData( source, filtered );
        widget->setQuickFindPattern( quick );
        widget->setSavedSearches( &SavedSearches::getSynced() );
        if ( live ) {
            dynamic_cast<StreamingLogData*>( source.get() )->appendUtf8( text );
            REQUIRE( waitUiState( [&] { return widget->isFirstLoadDone(); } ) );
        } else {
            REQUIRE( dynamic_cast<LogData*>( source.get() )->completePreparedLoading() );
        }
        REQUIRE( widget->isFirstLoadDone() );
    }
    SearchToolbar* toolbar() { return widget->reviewControls().filter; }
    SearchToolbar::FilterSpec candidate()
    {
        auto result = toolbar()->filterDraftSnapshot().filter;
        result.text = QStringLiteral( "ERROR" );
        result.matchCase = true;
        result.useRegexp = true;
        result.autoRefresh = true;
        result.keepResults = false;
        return result;
    }
    CrawlerWidget::FilterUpdatePreparation prepare()
    {
        const auto captured = widget->captureFilterUpdate( candidate() );
        REQUIRE( captured );
        return std::async( std::launch::async, [&] {
            return CrawlerWidget::prepareFilterUpdate( captured, cancelled );
        } ).get();
    }
};
}

TEST_CASE( "File and live result panes observe applied queries independently of drafts and painting",
           "[review-applied-filter]" )
{
    const bool live = GENERATE( false, true );
    CrawlerFilterFixture fixture{ live };
    fixture.widget->setPresentationActive( false );
    auto prepared = fixture.prepare();
    REQUIRE( prepared.update );
    auto publication = fixture.widget->lockFilterUpdate( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    publication->release();
    publication->notify();
    const auto panes = fixture.widget->paneInventory();
    REQUIRE( panes.size() == 2 );
    CHECK_FALSE( panes.front().appliedFilter );
    REQUIRE( panes.back().appliedFilter );
    CHECK( panes.back().appliedFilter->pattern.pattern == QStringLiteral( "ERROR" ) );
    CHECK( panes.back().appliedFilter->complete );
    const auto originalId = panes.back().paneId;
    fixture.toolbar()->searchLineEdit()->setEditText( QStringLiteral( "unsubmitted draft" ) );
    CHECK( fixture.widget->paneInventory().back().appliedFilter->pattern.pattern
           == QStringLiteral( "ERROR" ) );
    fixture.toolbar()->setKeepResultsChecked( true );
    fixture.toolbar()->searchLineEdit()->setEditText( QStringLiteral( "plain" ) );
    REQUIRE( QMetaObject::invokeMethod( fixture.widget.get(), "startNewSearch", Qt::DirectConnection ) );
    REQUIRE( waitUiState( [&] {
        const auto current = fixture.widget->paneInventory();
        return current.size() == 3 && current.back().appliedFilter
            && current.back().appliedFilter->complete;
    } ) );
    const auto kept = fixture.widget->paneInventory();
    CHECK( kept.at( 1 ).paneId == originalId );
    CHECK( kept.at( 1 ).appliedFilter->pattern.pattern == QStringLiteral( "ERROR" ) );
    CHECK( kept.at( 2 ).appliedFilter->pattern.pattern == QStringLiteral( "plain" ) );
    fixture.toolbar()->searchLineEdit()->setEditText( QString{} );
    REQUIRE( QMetaObject::invokeMethod( fixture.widget.get(), "startNewSearch", Qt::DirectConnection ) );
    const auto cleared = fixture.widget->paneInventory();
    CHECK( cleared.back().appliedFilter->pattern.pattern.isEmpty() );
    CHECK_FALSE( cleared.back().appliedFilter->complete );
    CHECK( cleared.at( 1 ).appliedFilter->pattern.pattern == QStringLiteral( "ERROR" ) );
}

TEST_CASE( "Crawler privately prepares and coherently installs a current-pane query",
           "[crawler-filter-publication]" )
{
    QWidget editorWindow;
    QLineEdit editor{ &editorWindow };
    editorWindow.show();
    editor.setFocus();
    REQUIRE( waitUiState( [&] { return QApplication::focusWidget() == &editor; } ) );
    const bool live = GENERATE( false, true );
    CrawlerFilterFixture fixture{ live };
    fixture.filtered->addMark( 1_lnum );
    CrawlerWidget::access_by<CrawlerFilterPublicationAccess>::showOnlyMarks( *fixture.widget );
    auto* main = fixture.widget->findChild<LogMainView*>();
    auto* results = fixture.widget->findChild<FilteredView*>();
    REQUIRE( main );
    REQUIRE( results );
    const auto paneId = results->runtimePaneId();
    const auto before = fixture.toolbar()->filterDraftSnapshot();
    const auto desired = fixture.candidate();
    QSignalSpy published{ fixture.filtered.get(), &LogFilteredData::searchResultsChanged };
    const auto prepared = fixture.prepare();
    REQUIRE( prepared.status == SearchPreparation::Status::Ready );
    REQUIRE( prepared.update );
    CHECK( fixture.toolbar()->filterDraftSnapshot().revision == before.revision );
    CHECK( fixture.filtered->getNbMatches() == 0_lcount );
    auto publication = fixture.widget->lockFilterUpdate( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    CHECK( fixture.filtered->isSearchComplete() );
    CHECK( fixture.filtered->getNbMatches() == 2_lcount );
    CHECK( fixture.filtered->getNbLine() == 3_lcount );
    CHECK( fixture.filtered->getMarks() == QList<LineNumber>{ 1_lnum } );
    CHECK( fixture.toolbar()->filterDraftSnapshot().filter == desired );
    CHECK( main->searchPattern() == desired.regularExpressionPattern() );
    CHECK( results->searchPattern() == main->searchPattern() );
    CHECK( results->runtimePaneId() == paneId );
    CHECK( published.empty() );
    CHECK( QApplication::focusWidget() == &editor );
    publication->release();
    publication->notify();
    CHECK( published.size() == 1 );
    CHECK( QApplication::focusWidget() == &editor );
    if ( live ) {
        dynamic_cast<StreamingLogData*>( fixture.source.get() )->appendUtf8( "ERROR appended\n" );
        REQUIRE( waitUiState( [&] {
            return fixture.filtered->isSearchComplete() && fixture.filtered->getNbMatches() == 3_lcount;
        } ) );
        CHECK( QApplication::focusWidget() == &editor );
    }
}

TEST_CASE( "Crawler query publication rejects newer drafts and human match-selection work",
           "[crawler-filter-publication]" )
{
    CrawlerFilterFixture fixture;
    const auto prepared = fixture.prepare();
    REQUIRE( prepared.status == SearchPreparation::Status::Ready );
    auto* main = fixture.widget->findChild<LogMainView*>();
    REQUIRE( main );
    SECTION( "draft ABA" ) {
        const auto original = fixture.toolbar()->currentSearchText();
        fixture.toolbar()->searchLineEdit()->setEditText( QStringLiteral( "human" ) );
        fixture.toolbar()->searchLineEdit()->setEditText( original );
    }
    SECTION( "pending human selection is not interrupted" ) {
        REQUIRE( main->selectAllMatches( true ) );
        REQUIRE( main->matchSelectionPending() );
    }
    SECTION( "completed cancellation still revokes the earlier capture" ) {
        REQUIRE( main->selectAllMatches( true ) );
        main->cancelMatchSelection();
        REQUIRE_FALSE( main->matchSelectionPending() );
    }
    CHECK_FALSE( fixture.widget->lockFilterUpdate( prepared.update ) );
    CHECK( fixture.filtered->getNbMatches() == 0_lcount );
}

TEST_CASE( "Stopping native auto-refresh revokes a privately prepared crawler replacement",
           "[crawler-filter-publication]" )
{
    CrawlerFilterFixture fixture;
    const auto first = fixture.prepare();
    auto publication = fixture.widget->lockFilterUpdate( first.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    publication->release();
    publication->notify();
    const auto pending = fixture.prepare();
    REQUIRE( pending.status == SearchPreparation::Status::Ready );
    const auto generation = fixture.filtered->currentSearchGeneration();
    REQUIRE( QMetaObject::invokeMethod( fixture.widget.get(), "stopSearch", Qt::DirectConnection ) );
    CHECK( fixture.filtered->currentSearchGeneration() == generation );
    CHECK_FALSE( fixture.widget->lockFilterUpdate( pending.update ) );
    CHECK( fixture.filtered->getNbMatches() == 2_lcount );
}

TEST_CASE( "Crawler publications install all UI owners before observers and retain captured history",
           "[crawler-filter-publication]" )
{
    CrawlerFilterFixture first;
    CrawlerFilterFixture second{ true };
    const auto firstPrepared = first.prepare();
    const auto secondPrepared = second.prepare();
    auto firstPublication = first.widget->lockFilterUpdate( firstPrepared.update );
    auto secondPublication = second.widget->lockFilterUpdate( secondPrepared.update );
    REQUIRE( firstPublication );
    REQUIRE( secondPublication );
    bool observed = false;
    QObject::connect( first.filtered.get(), &LogFilteredData::searchResultsChanged,
        first.source.get(), [&] {
            observed = true;
            CHECK( second.filtered->isSearchComplete() );
            CHECK( second.toolbar()->currentSearchText() == "ERROR" );
            CHECK( second.widget->findChild<FilteredView*>()->searchPattern().pattern == "ERROR" );
            second.toolbar()->searchLineEdit()->setEditText( QStringLiteral( "unsubmitted" ) );
        } );
    REQUIRE( firstPublication->install() );
    REQUIRE( secondPublication->install() );
    firstPublication->release();
    secondPublication->release();
    firstPublication->notify();
    secondPublication->notify();
    CHECK( observed );
    CHECK( second.toolbar()->currentSearchText() == "unsubmitted" );
    CHECK( SavedSearches::get().recentSearches().front() == "ERROR" );
}

TEST_CASE( "Crawler query publication survives observers destroying its owner and guard",
           "[crawler-filter-publication]" )
{
    CrawlerFilterFixture fixture;
    const auto prepared = fixture.prepare();
    auto publication = fixture.widget->lockFilterUpdate( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    publication->release();
    bool observed = false;
    QObject::connect( fixture.filtered.get(), &LogFilteredData::searchResultsChanged,
        fixture.source.get(), [&] {
            observed = true;
            publication.reset();
            fixture.widget.reset();
        } );
    publication->notify();
    CHECK( observed );
    CHECK_FALSE( publication );
    CHECK_FALSE( fixture.widget );
}

TEST_CASE( "GUI filter staging binds one detached preparation to its live owner",
           "[crawler-filter-publication][crawler-filter-staging]" )
{
    CrawlerFilterFixture fixture;
    auto staging = fixture.widget->stageFilterUpdate( fixture.candidate() );
    REQUIRE( staging );
    const auto prepared = std::async( std::launch::async,
        [capture = staging->capture(), &cancelled = fixture.cancelled] {
            return CrawlerWidget::prepareFilterUpdate( capture, cancelled );
        } ).get();
    REQUIRE( prepared.update );
    CHECK( fixture.filtered->getNbMatches() == 0_lcount );
    SECTION( "publish through the staging owner" ) {
        auto publication = staging->lock( prepared.update );
        REQUIRE( publication );
        REQUIRE( publication->install() );
        publication->release();
        publication->notify();
        CHECK( fixture.filtered->getNbMatches() == 2_lcount );
    }
    SECTION( "closed owner is not retained or recreated" ) {
        fixture.widget.reset();
        CHECK_FALSE( staging->lock( prepared.update ) );
    }
    SECTION( "another staging handle cannot claim the preparation" ) {
        auto other = fixture.widget->stageFilterUpdate( fixture.candidate() );
        REQUIRE( other );
        CHECK_FALSE( other->lock( prepared.update ) );
        REQUIRE( staging->lock( prepared.update ) );
    }
}

TEST_CASE( "Crawler prepares a new kept pane without publishing or copying the old pane marks",
           "[crawler-kept-filter-publication]" )
{
    QWidget editorWindow;
    QLineEdit editor{ &editorWindow };
    editorWindow.show();
    editor.setFocus();
    REQUIRE( waitUiState( [&] { return QApplication::focusWidget() == &editor; } ) );
    const bool live = GENERATE( false, true );
    CrawlerFilterFixture fixture{ live };
    auto initial = fixture.prepare();
    auto initialPublication = fixture.widget->lockFilterUpdate( initial.update );
    REQUIRE( initialPublication );
    REQUIRE( initialPublication->install() );
    initialPublication->release();
    initialPublication->notify();
    fixture.filtered->addMark( 0_lnum );
    const auto before = fixture.widget->paneInventory();
    REQUIRE( before.size() == 2 );
    const auto oldId = before.back().paneId;
    const auto oldDraft = fixture.toolbar()->filterDraftSnapshot();
    auto candidate = fixture.candidate();
    candidate.text = QStringLiteral( "plain" );
    candidate.autoRefresh = false;
    candidate.keepResults = true;
    const auto staging = fixture.widget->stageFilterUpdate( candidate );
    REQUIRE( staging );
    const auto captured = staging->capture();
    REQUIRE( captured );
    const auto prepared = std::async( std::launch::async, [&] {
        return CrawlerWidget::prepareFilterUpdate( captured, fixture.cancelled );
    } ).get();
    REQUIRE( prepared.update );
    CHECK( fixture.widget->paneInventory().size() == 2 );
    CHECK( fixture.toolbar()->filterDraftSnapshot().revision == oldDraft.revision );
    CHECK( fixture.filtered->getNbMatches() == 2_lcount );
    auto publication = staging->lock( prepared.update );
    REQUIRE( publication );
    CHECK( publication->paneId() != oldId );
    REQUIRE( publication->install() );
    const auto installed = fixture.widget->paneInventory();
    REQUIRE( installed.size() == 3 );
    CHECK( installed.at( 1 ).paneId == oldId );
    CHECK_FALSE( installed.at( 1 ).current );
    CHECK( installed.back().current );
    CHECK( installed.back().paneId == publication->paneId() );
    REQUIRE( installed.at( 1 ).appliedFilter );
    CHECK( installed.at( 1 ).appliedFilter->pattern.pattern == QStringLiteral( "ERROR" ) );
    CHECK( installed.at( 1 ).appliedFilter->complete );
    CHECK( fixture.filtered->getNbMarks() == 1_lcount );
    const auto newRows = fixture.widget->createPaneSnapshot(
        installed.back().paneId, klogg::SnapshotScope::PresentedRows );
    REQUIRE( newRows );
    CHECK( newRows->lineCount() == 1_lcount );
    CHECK( newRows->lineString( 0_lnum ) == QStringLiteral( "plain" ) );
    CHECK_FALSE( fixture.toolbar()->isKeepResultsChecked() );
    publication->release();
    publication->notify();
    CHECK( QApplication::focusWidget() == &editor );
}

TEST_CASE( "Kept-pane publication cancels an old queued search without early notifications",
           "[crawler-kept-filter-publication]" )
{
    CrawlerFilterFixture fixture;
    auto& worker = LogFilteredData::access_by<CrawlerQueuedSearchAccess>::worker( *fixture.filtered );
    LogFilteredDataWorker::access_by<CrawlerQueuedSearchAccess>::holdConsumer( worker );
    fixture.toolbar()->searchLineEdit()->setEditText( QStringLiteral( "ERROR" ) );
    REQUIRE( QMetaObject::invokeMethod( fixture.widget.get(), "startNewSearch", Qt::DirectConnection ) );
    REQUIRE( LogFilteredDataWorker::access_by<CrawlerQueuedSearchAccess>::pending( worker ) );
    auto candidate = fixture.candidate();
    candidate.text = QStringLiteral( "plain" );
    candidate.keepResults = true;
    const auto staging = fixture.widget->stageFilterUpdate( candidate );
    REQUIRE( staging );
    const auto prepared = std::async( std::launch::async,
        [capture = staging->capture(), &cancelled = fixture.cancelled] {
            return CrawlerWidget::prepareFilterUpdate( capture, cancelled );
        } ).get();
    REQUIRE( prepared.update );
    auto publication = staging->lock( prepared.update );
    REQUIRE( publication );
    QSignalSpy progress{ fixture.filtered.get(), &LogFilteredData::searchProgressed };
    QSignalSpy switched{ fixture.widget.get(), &CrawlerWidget::filteredViewChanged };
    REQUIRE( publication->install() );
    CHECK_FALSE( LogFilteredDataWorker::access_by<CrawlerQueuedSearchAccess>::pending( worker ) );
    CHECK( progress.count() == 0 );
    CHECK( switched.count() == 0 );
    publication->release();
    publication->notify();
    CHECK( switched.count() == 1 );
    REQUIRE( waitUiState( [&] { return !progress.isEmpty() && progress.last().at( 1 ).toInt() == 100; } ) );
    CHECK( fixture.widget->paneInventory().size() == 3 );
    CHECK( fixture.widget->paneInventory().back().appliedFilter->pattern.pattern == QStringLiteral( "plain" ) );
}

TEST_CASE( "Unpublished kept-pane resources survive closed owners only until the GUI guard releases",
           "[crawler-kept-filter-publication]" )
{
    const bool live = GENERATE( false, true );
    CrawlerFilterFixture fixture{ live };
    auto candidate = fixture.candidate();
    candidate.keepResults = true;
    auto staging = fixture.widget->stageFilterUpdate( candidate );
    REQUIRE( staging );
    const auto prepared = std::async( std::launch::async,
        [capture = staging->capture(), &cancelled = fixture.cancelled] {
            return CrawlerWidget::prepareFilterUpdate( capture, cancelled );
        } ).get();
    REQUIRE( prepared.update );
    std::weak_ptr<SearchableLogData> source = fixture.source;
    SECTION( "owner closes before acquisition" ) {
        fixture.widget.reset();
        fixture.filtered.reset();
        fixture.source.reset();
        CHECK_FALSE( source.expired() );
        CHECK_FALSE( staging->lock( prepared.update ) );
        staging.reset();
        CHECK( source.expired() );
    }
    SECTION( "detached worker capture remains usable after every GUI owner disappears" ) {
        const auto capture = staging->capture();
        fixture.widget.reset();
        fixture.filtered.reset();
        fixture.source.reset();
        staging.reset();
        REQUIRE( source.expired() );
        const auto detached = std::async( std::launch::async, [&] {
            return CrawlerWidget::prepareFilterUpdate( capture, fixture.cancelled );
        } ).get();
        CHECK( detached.status == SearchPreparation::Status::Ready );
        CHECK( detached.update );
    }
    SECTION( "publication guard retains unpublished view model and source" ) {
        auto publication = staging->lock( prepared.update );
        REQUIRE( publication );
        staging.reset();
        fixture.widget.reset();
        fixture.filtered.reset();
        fixture.source.reset();
        CHECK_FALSE( source.expired() );
        CHECK_FALSE( publication->install() );
        publication.reset();
        CHECK( source.expired() );
    }
}

TEST_CASE( "Kept-pane notification tolerates observers destroying its owner and publication guard",
           "[crawler-kept-filter-publication]" )
{
    CrawlerFilterFixture fixture;
    auto candidate = fixture.candidate();
    candidate.keepResults = true;
    auto staging = fixture.widget->stageFilterUpdate( candidate );
    REQUIRE( staging );
    const auto prepared = std::async( std::launch::async,
        [capture = staging->capture(), &cancelled = fixture.cancelled] {
            return CrawlerWidget::prepareFilterUpdate( capture, cancelled );
        } ).get();
    REQUIRE( prepared.update );
    auto publication = staging->lock( prepared.update );
    REQUIRE( publication );
    staging.reset();
    REQUIRE( publication->install() );
    publication->release();
    bool observed = false;
    QObject::connect( fixture.widget.get(), &CrawlerWidget::filteredViewChanged,
        fixture.source.get(), [&] {
            observed = true;
            CHECK( fixture.widget->paneInventory().size() == 3 );
            publication.reset();
            fixture.widget.reset();
        } );
    publication->notify();
    CHECK( observed );
    CHECK_FALSE( publication );
    CHECK_FALSE( fixture.widget );
}

TEST_CASE( "Unpublished kept panes reject human edits and failed preparation without altering membership",
           "[crawler-kept-filter-publication]" )
{
    CrawlerFilterFixture fixture;
    auto candidate = fixture.candidate();
    candidate.keepResults = true;
    bool ready = true;
    SECTION( "human draft change" ) {}
    SECTION( "cancelled preparation" ) { fixture.cancelled.set(); ready = false; }
    SECTION( "invalid expression" ) { candidate.text = QStringLiteral( "[" ); ready = false; }
    auto staging = fixture.widget->stageFilterUpdate( candidate );
    REQUIRE( staging );
    const auto prepared = std::async( std::launch::async,
        [capture = staging->capture(), &cancelled = fixture.cancelled] {
            return CrawlerWidget::prepareFilterUpdate( capture, cancelled );
        } ).get();
    if ( ready ) {
        REQUIRE( prepared.update );
        fixture.toolbar()->searchLineEdit()->setEditText( QStringLiteral( "human" ) );
        CHECK_FALSE( staging->lock( prepared.update ) );
        CHECK( fixture.toolbar()->currentSearchText() == QStringLiteral( "human" ) );
    } else {
        CHECK_FALSE( prepared.update );
        CHECK( fixture.toolbar()->currentSearchText().isEmpty() );
    }
    staging.reset();
    CHECK( fixture.widget->paneInventory().size() == 2 );
    CHECK( fixture.filtered->getNbMatches() == 0_lcount );
}

TEST_CASE( "Private kept panes prepare final context and reveal matches without inheriting marks",
           "[crawler-kept-filter-publication]" )
{
    CrawlerFilterFixture fixture;
    using Access = CrawlerWidget::access_by<CrawlerFilterPublicationAccess>;
    Access::setContext( *fixture.widget, 3, 1 );
    fixture.filtered->addMark( 3_lnum );
    Access::showOnlyMarks( *fixture.widget );
    auto candidate = fixture.candidate();
    candidate.text = QStringLiteral( "plain" );
    candidate.keepResults = true;
    auto staging = fixture.widget->stageFilterUpdate( candidate );
    REQUIRE( staging );
    const auto prepared = std::async( std::launch::async,
        [capture = staging->capture(), &cancelled = fixture.cancelled] {
            return CrawlerWidget::prepareFilterUpdate( capture, cancelled );
        } ).get();
    REQUIRE( prepared.update );
    auto publication = staging->lock( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    const auto rows = fixture.widget->createPaneSnapshot(
        publication->paneId(), klogg::SnapshotScope::PresentedRows );
    REQUIRE( rows );
    CHECK( rows->lineCount() == 3_lcount );
    CHECK( fixture.filtered->getNbMarks() == 1_lcount );
    publication->release();
    publication->notify();
    CHECK( fixture.widget->createPaneSnapshot( publication->paneId(), klogg::SnapshotScope::PresentedRows )
               ->lineCount() == 3_lcount );
}

TEST_CASE( "Private kept-pane admission rejects a changed live source cutoff",
           "[crawler-kept-filter-publication]" )
{
    CrawlerFilterFixture fixture{ true };
    auto candidate = fixture.candidate();
    candidate.keepResults = true;
    auto staging = fixture.widget->stageFilterUpdate( candidate );
    REQUIRE( staging );
    const auto prepared = std::async( std::launch::async,
        [capture = staging->capture(), &cancelled = fixture.cancelled] {
            return CrawlerWidget::prepareFilterUpdate( capture, cancelled );
        } ).get();
    REQUIRE( prepared.update );
    dynamic_cast<StreamingLogData*>( fixture.source.get() )->appendUtf8( "later\n" );
    CHECK_FALSE( staging->lock( prepared.update ) );
    CHECK( fixture.widget->paneInventory().size() == 2 );
    CHECK( fixture.toolbar()->currentSearchText().isEmpty() );
}

TEST_CASE( "Private empty kept-pane publication follows the native NoSearch display policy",
           "[crawler-kept-filter-publication]" )
{
    const bool showAll = GENERATE( false, true );
    struct RestorePolicy {
        bool previous = Configuration::get().showAllInFilteredViewWhenSearchEmpty();
        ~RestorePolicy() { Configuration::get().setShowAllInFilteredViewWhenSearchEmpty( previous ); }
    } restore;
    Configuration::get().setShowAllInFilteredViewWhenSearchEmpty( showAll );
    CrawlerFilterFixture fixture;
    auto candidate = fixture.candidate();
    candidate.text.clear();
    candidate.keepResults = true;
    auto staging = fixture.widget->stageFilterUpdate( candidate );
    REQUIRE( staging );
    const auto prepared = std::async( std::launch::async,
        [capture = staging->capture(), &cancelled = fixture.cancelled] {
            return CrawlerWidget::prepareFilterUpdate( capture, cancelled );
        } ).get();
    REQUIRE( prepared.update );
    auto publication = staging->lock( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    const auto pane = fixture.widget->paneInventory().back();
    REQUIRE( pane.appliedFilter );
    CHECK_FALSE( pane.appliedFilter->complete );
    CHECK( pane.appliedFilter->pattern.pattern.isEmpty() );
    const auto rows = fixture.widget->createPaneSnapshot( pane.paneId, klogg::SnapshotScope::PresentedRows );
    REQUIRE( rows );
    CHECK( rows->lineCount() == ( showAll ? 4_lcount : 0_lcount ) );
    CHECK_FALSE( fixture.widget->createPaneSnapshot( pane.paneId, klogg::SnapshotScope::Matches ) );
    publication->release();
    publication->notify();
    CHECK( CrawlerWidget::access_by<CrawlerFilterPublicationAccess>::hasNoSearch( *fixture.widget ) );
}

TEST_CASE( "Crawler empty query publication follows native display policy and enters NoSearch",
           "[crawler-filter-publication][crawler-filter-reset]" )
{
    const bool showAll = GENERATE( false, true );
    struct RestorePolicy {
        bool previous = Configuration::get().showAllInFilteredViewWhenSearchEmpty();
        ~RestorePolicy() { Configuration::get().setShowAllInFilteredViewWhenSearchEmpty( previous ); }
    } restore;
    Configuration::get().setShowAllInFilteredViewWhenSearchEmpty( showAll );
    CrawlerFilterFixture fixture{ GENERATE( false, true ) };
    fixture.filtered->addMark( 1_lnum );
    const auto initial = fixture.prepare();
    auto initialPublication = fixture.widget->lockFilterUpdate( initial.update );
    REQUIRE( initialPublication );
    REQUIRE( initialPublication->install() );
    initialPublication->release();
    initialPublication->notify();
    auto candidate = fixture.toolbar()->filterDraftSnapshot().filter;
    candidate.text.clear();
    const auto captured = fixture.widget->captureFilterUpdate( candidate );
    REQUIRE( captured );
    const auto prepared = std::async( std::launch::async, [&] {
        return CrawlerWidget::prepareFilterUpdate( captured, fixture.cancelled );
    } ).get();
    REQUIRE( prepared.status == SearchPreparation::Status::Ready );
    CHECK( fixture.toolbar()->currentSearchText() == "ERROR" );
    auto publication = fixture.widget->lockFilterUpdate( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    CHECK( fixture.toolbar()->currentSearchText().isEmpty() );
    CHECK( fixture.widget->findChild<LogMainView*>()->searchPattern().pattern.isEmpty() );
    CHECK( fixture.widget->findChild<FilteredView*>()->searchPattern().pattern.isEmpty() );
    CHECK( CrawlerWidget::access_by<CrawlerFilterPublicationAccess>::hasNoSearch( *fixture.widget ) );
    CHECK_FALSE( fixture.filtered->isSearchComplete() );
    CHECK( fixture.filtered->getNbMatches() == 0_lcount );
    CHECK( fixture.filtered->getNbLine() == ( showAll ? 4_lcount : 1_lcount ) );
    CHECK( fixture.filtered->getMarks() == QList<LineNumber>{ 1_lnum } );
    publication->release();
    publication->notify();
    QCoreApplication::processEvents();
    CHECK( CrawlerWidget::access_by<CrawlerFilterPublicationAccess>::hasNoSearch( *fixture.widget ) );
    CHECK_FALSE( fixture.filtered->isSearchComplete() );
}

TEST_CASE( "Changing empty-filter display policy revokes a privately prepared reset",
           "[crawler-filter-publication][crawler-filter-reset]" )
{
    struct RestorePolicy {
        bool previous = Configuration::get().showAllInFilteredViewWhenSearchEmpty();
        ~RestorePolicy() { Configuration::get().setShowAllInFilteredViewWhenSearchEmpty( previous ); }
    } restore;
    CrawlerFilterFixture fixture;
    const auto before = fixture.toolbar()->filterDraftSnapshot();
    auto candidate = fixture.candidate();
    candidate.text.clear();
    const auto captured = fixture.widget->captureFilterUpdate( candidate );
    REQUIRE( captured );
    const auto prepared = std::async( std::launch::async, [&] {
        return CrawlerWidget::prepareFilterUpdate( captured, fixture.cancelled );
    } ).get();
    REQUIRE( prepared.status == SearchPreparation::Status::Ready );
    Configuration::get().setShowAllInFilteredViewWhenSearchEmpty( !restore.previous );
    CHECK_FALSE( fixture.widget->lockFilterUpdate( prepared.update ) );
    CHECK( fixture.toolbar()->filterDraftSnapshot().revision == before.revision );
}

TEST_CASE( "Crawler query preparation failure leaves draft and applied results untouched",
           "[crawler-filter-publication]" )
{
    CrawlerFilterFixture fixture;
    const auto before = fixture.toolbar()->filterDraftSnapshot();
    auto candidate = fixture.candidate();
    auto status = SearchPreparation::Status::CompileFailure;
    SECTION( "invalid expression" ) { candidate.text = "["; }
    SECTION( "cancelled" ) { fixture.cancelled.set(); status = SearchPreparation::Status::Cancelled; }
    const auto captured = fixture.widget->captureFilterUpdate( candidate );
    REQUIRE( captured );
    const auto prepared = std::async( std::launch::async, [&] {
        return CrawlerWidget::prepareFilterUpdate( captured, fixture.cancelled );
    } ).get();
    CHECK( prepared.status == status );
    CHECK_FALSE( prepared.update );
    CHECK( fixture.toolbar()->filterDraftSnapshot().filter == before.filter );
    CHECK( fixture.toolbar()->filterDraftSnapshot().revision == before.revision );
    CHECK( fixture.filtered->getNbMatches() == 0_lcount );
}

TEST_CASE( "Crawler records the published query rather than a reentrant newer draft",
           "[crawler-filter-publication]" )
{
    CrawlerFilterFixture fixture;
    const auto prepared = fixture.prepare();
    auto publication = fixture.widget->lockFilterUpdate( prepared.update );
    REQUIRE( publication );
    REQUIRE( publication->install() );
    publication->release();
    QObject::connect( fixture.filtered.get(), &LogFilteredData::searchResultsChanged,
        fixture.widget.get(), [&] {
            fixture.toolbar()->searchLineEdit()->setEditText( QStringLiteral( "unsubmitted" ) );
        } );
    publication->notify();
    CHECK( fixture.toolbar()->currentSearchText() == "unsubmitted" );
    REQUIRE_FALSE( SavedSearches::get().recentSearches().isEmpty() );
    CHECK( SavedSearches::get().recentSearches().front() == "ERROR" );
}
