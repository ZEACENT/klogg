#include <catch2/catch.hpp>
#include <future>

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QFileInfo>
#include <QTimer>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QTemporaryDir>
#include <QSignalSpy>
#include "highlighterset.h"
#include "highlighterstatepublisher.h"

#include "abstractcrawlerwidget.h"
#include "colorlabelscontroller.h"
#include "mainwindow.h"
#include "foldercrawlerwidget.h"
#include "crawlerwidget.h"
#include "foldersearchengine.h"
#include "optionsdialog.h"
#include "logger.h"
#include "styles.h"
#include "reviewcontroladapter.h"
#include "reviewcontentadapter.h"
#include "reviewcontrolservice.h"
#include "test_utils.h"
#include "searchtoolbar.h"
#include "session.h"
#include "tabbedcrawlerwidget.h"

using namespace klogg::control;

namespace {
QJsonObject resultObject( const DecodedResponse& response )
{
    REQUIRE( std::holds_alternative<QJsonValue>( response ) );
    REQUIRE( std::get<QJsonValue>( response ).isObject() );
    return std::get<QJsonValue>( response ).toObject();
}

DecodedResponse localRequest( const InstanceDescriptor& descriptor, const QString& method,
                              const QJsonObject& params = {} )
{
    auto future = std::async( std::launch::async, [ descriptor, method, params ] {
        return callLocal( descriptor, { "gui-integration", method, params }, 5000 );
    } );
    REQUIRE( waitUiState( [ & ] {
        return future.wait_for( std::chrono::milliseconds( 0 ) ) == std::future_status::ready;
    } ) );
    const auto result = future.get();
    INFO( result.error.toStdString() );
    REQUIRE( result.response );
    return *result.response;
}

QString errorCode( const DecodedResponse& response )
{
    REQUIRE( std::holds_alternative<RpcError>( response ) );
    return std::get<RpcError>( response ).data.toObject().value( "code" ).toString();
}

struct ReviewFixture {
    QTemporaryDir directory;
    std::shared_ptr<Session> session = std::make_shared<Session>();
    MainWindow window{ WindowSession{ session, "ControlAdapter", 0 } };
    ReviewControlAdapter adapter{ [ this ] {
        return std::vector<QPointer<MainWindow>>{ &window };
    } };

    ReviewFixture()
    {
        REQUIRE( directory.isValid() );
        QFile file( directory.filePath( QStringLiteral( "evidence.log" ) ) );
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        REQUIRE( file.write( "Evidence\n" ) == 9 );
        file.close();
        window.openFolderByPath( directory.path() );
        window.openFolderByPath( directory.path() );
        REQUIRE( window.inventorySnapshot().tabs.size() == 2 );
    }

    QString tabId( size_t index = 0 ) const
    {
        return window.inventorySnapshot().tabs.at( index ).tabId;
    }

    AbstractCrawlerWidget::ReviewControls controls( size_t index = 0 ) const
    {
        const auto entry = window.inventorySnapshot().tabs.at( index );
        auto* document = dynamic_cast<AbstractCrawlerWidget*>( entry.widget.data() );
        REQUIRE( document != nullptr );
        return document->reviewControls();
    }

    DecodedResponse call( const QString& method, const QJsonObject& params = {} )
    {
        return adapter.dispatch( { "test-request", method, params } );
    }

    QString openEvidenceFileTab()
    {
        window.loadInitialFile( directory.filePath( QStringLiteral( "evidence.log" ) ), false );
        QString id;
        REQUIRE( waitUiState( [&] {
            for ( const auto& entry : window.inventorySnapshot().tabs ) {
                if ( entry.kind != DocumentKind::File ) continue;
                const auto* document = dynamic_cast<AbstractCrawlerWidget*>( entry.widget.data() );
                if ( document && document->paneMetadata( AbstractCrawlerWidget::Pane::Main ).sourceRowCount == 1_lcount ) {
                    id = entry.tabId;
                    return true;
                }
            }
            return false;
        } ) );
        return id;
    }
};
struct RestoreHighlighters {
    HighlighterSetCollection saved = HighlighterSetCollection::get();
    ~RestoreHighlighters() { HighlighterSetCollection::get() = saved; HighlighterSetCollection::get().save(); }
};

QJsonObject markRequest( ReviewFixture& fixture, const QString& tabId, const QString& pane )
{
    const auto panes = resultObject( fixture.call( "pane.list", { { "tab_id", tabId } } ) ).value( "panes" ).toArray();
    REQUIRE_FALSE( panes.isEmpty() );
    const auto owner = panes.last().toObject().value( "marks_owner_id" ).toString();
    REQUIRE_FALSE( owner.isEmpty() );
    const auto snapshot = resultObject( fixture.call( "source.capture", {
        { "tab_id", tabId }, { "pane", pane }, { "scope", "presented" } } ) ).value( "snapshot_id" ).toString();
    REQUIRE_FALSE( snapshot.isEmpty() );
    return { { "tab_id", tabId }, { "owner_id", owner }, { "snapshot_id", snapshot }, { "rows", QJsonArray{ "0" } } };
}

QJsonObject replacement( const QString& pattern = QStringLiteral( "Evidence" ) )
{
    return { { "sets", QJsonArray{ QJsonObject{ { "id", "rpc-evidence" }, { "name", "Evidence rules" },
        { "rules", QJsonArray{ QJsonObject{ { "pattern", pattern }, { "foreground", "#ff000000" },
                                          { "background", "#ffffff00" } } } } } } },
        { "active_set_ids", QJsonArray{ "rpc-evidence" } } };
}
} // namespace

TEST_CASE( "An abandoned transaction releases its unpublished document without another RPC",
           "[control-mixed-document]" )
{
    ReviewFixture fixture;
    QTemporaryDir unopened;
    REQUIRE( unopened.isValid() );
    QFile log{ unopened.filePath( "hidden.log" ) };
    REQUIRE( log.open( QIODevice::WriteOnly ) );
    log.close();
    qint64 clock = 0;
    ReviewControlAdapter adapter{ [&] { return std::vector<QPointer<MainWindow>>{ &fixture.window }; },
                                  [&] { return clock; } };
    const auto tx = resultObject( adapter.dispatch( { "id", "transaction.begin", {} } ) ).value( "transaction_id" );
    const auto opened = resultObject( adapter.dispatch( { "id", "documents.open", {
        { "window_id", fixture.window.runtimeWindowId() }, { "path", unopened.path() },
        { "kind", "folder" }, { "transaction_id", tx } } } ) );
    QJsonObject receipt;
    REQUIRE( waitUiState( [&] {
        receipt = resultObject( adapter.dispatch( { "id", "operation.get", { { "operation_id", opened.value( "operation_id" ) } } } ) );
        return receipt.value( "status" ) != "pending";
    } ) );
    REQUIRE( receipt.value( "status" ) == "completed" );
    QPointer<FolderCrawlerWidget> staged;
    const auto id = receipt.value( "result" ).toObject().value( "tab_id" );
    for ( auto* widget : QApplication::allWidgets() ) {
        auto* folder = qobject_cast<FolderCrawlerWidget*>( widget );
        if ( folder && folder->runtimeTabId() == id ) staged = folder;
    }
    REQUIRE( staged );
    clock = ReviewControlAdapter::TransactionLifetimeMs + 1;
    REQUIRE( waitUiState( [&] { return staged.isNull(); } ) );
    CHECK( fixture.window.inventorySnapshot().tabs.size() == 2 );
}

TEST_CASE( "A private folder, query and source-backed bookmark publish as a single transaction",
           "[control-mixed-document]" )
{
    ReviewFixture fixture;
    QTemporaryDir unopened;
    REQUIRE( unopened.isValid() );
    QFile log{ unopened.filePath( "new.log" ) };
    REQUIRE( log.open( QIODevice::WriteOnly ) );
    REQUIRE( log.write( "before\nPRIVATE MATCH\nafter\n" ) == 27 );
    log.close();
    const auto call = [&]( const QString& method, const QJsonObject& params = {} ) {
        return resultObject( fixture.call( method, params ) );
    };
    const auto await = [&]( const QJsonObject& started, QJsonObject scope = {} ) {
        scope.insert( "operation_id", started.value( "operation_id" ) );
        QJsonObject receipt;
        REQUIRE( waitUiState( [&] {
            receipt = call( "operation.get", scope );
            return receipt.value( "status" ) != "pending";
        } ) );
        REQUIRE( receipt.value( "status" ) == "completed" );
        return receipt.value( "result" ).toObject();
    };
    const auto before = call( "state.get" );
    const auto tx = call( "transaction.begin" ).value( "transaction_id" );
    const QJsonObject scope{ { "transaction_id", tx } };
    const auto open = call( "documents.open", { { "window_id", fixture.window.runtimeWindowId() },
        { "path", unopened.path() }, { "kind", "folder" }, { "transaction_id", tx } } );
    CHECK( errorCode( fixture.call( "transaction.commit", scope ) ) == "TRANSACTION_BUSY" );
    const auto tab = await( open ).value( "tab_id" );
    const QJsonObject scopedTab{ { "tab_id", tab }, { "transaction_id", tx } };
    CHECK( errorCode( fixture.call( "tab.get", { { "tab_id", tab } } ) ) == "TARGET_NOT_FOUND" );
    REQUIRE( call( "tab.get", scopedTab ).value( "kind" ) == "folder" );
    auto apply = scopedTab;
    apply.insert( "filter", QJsonObject{ { "text", "MATCH" }, { "inverse", false }, { "boolean", false } } );
    await( call( "filter.apply", apply ) );
    auto pane = scopedTab;
    pane.insert( "pane", "results" );
    const auto owner = call( "pane.get", pane ).value( "marks_owner_id" );
    pane.insert( "scope", "matches" );
    const auto snap = call( "source.capture", pane );
    REQUIRE( snap.value( "row_count" ) == "1" );
    QJsonObject read{ { "snapshot_id", snap.value( "snapshot_id" ) }, { "start", "0" }, { "count", 1 } };
    CHECK( errorCode( fixture.call( "source.read", read ) ) == "SNAPSHOT_NOT_FOUND" );
    read.insert( "transaction_id", tx );
    const auto reading = call( "source.read", read );
    CHECK( errorCode( fixture.call( "operation.get", { { "operation_id", reading.value( "operation_id" ) } } ) )
           == "OPERATION_NOT_FOUND" );
    CHECK( await( reading, scope ).value( "rows" ).toArray().first().toObject().value( "text" ) == "PRIVATE MATCH" );
    call( "mark.add", { { "transaction_id", tx }, { "tab_id", tab }, { "owner_id", owner },
        { "snapshot_id", snap.value( "snapshot_id" ) }, { "rows", QJsonArray{ "0" } } } );
    CHECK( call( "state.get" ) == before );
    SECTION( "abort publishes nothing" ) {
        call( "transaction.abort", scope );
        CHECK( call( "state.get" ) == before );
        CHECK( errorCode( fixture.call( "pane.list", { { "tab_id", tab } } ) ) == "TARGET_NOT_FOUND" );
    }
    SECTION( "commit publishes the configured folder with its bookmark" ) {
        call( "transaction.commit", scope );
        QJsonObject receipt;
        REQUIRE( waitUiState( [&] {
            receipt = call( "transaction.get", scope );
            return receipt.value( "status" ) != "preparing";
        } ) );
        REQUIRE( receipt.value( "status" ) == "committed" );
        CHECK( fixture.window.inventorySnapshot().tabs.size() == 3 );
        CHECK( call( "tab.get", { { "tab_id", tab } } ).value( "filter_draft" ).toObject().value( "text" ) == "MATCH" );
        const auto groups = call( "mark.list", { { "tab_id", tab }, { "owner_id", owner } } ).value( "groups" ).toArray();
        REQUIRE( groups.size() == 1 );
        CHECK( groups.first().toObject().value( "rows" ) == QJsonArray{ "1" } );
    }
}

TEST_CASE( "Control global highlighters and labels publish together after private preparation",
           "[ui][control-adapter][control-highlighter-rpc]" )
{
    ReviewFixture fixture;
    RestoreHighlighters restore;
    auto& global = HighlighterSetCollection::get();
    const auto revision = global.revision();
    const auto before = resultObject( fixture.call( "highlighter.list" ) );
    CHECK( before.value( "revision" ) == QString::number( revision ) );
    const auto id = resultObject( fixture.call( "transaction.begin" ) ).value( "transaction_id" ).toString();
    const QJsonObject tx{ { "transaction_id", id } };
    auto change = replacement();
    change.insert( "transaction_id", id );
    change.insert( "expected_revision", QString::number( revision ) );
    resultObject( fixture.call( "highlighter.replace", change ) );
    resultObject( fixture.call( "label.set", { { "transaction_id", id }, { "tab_id", fixture.tabId() },
        { "label", 1 }, { "texts", QJsonArray{ "Evidence" } } } ) );
    const auto staged = resultObject( fixture.call( "highlighter.list", tx ) );
    CHECK( staged.value( "active_set_ids" ) == change.value( "active_set_ids" ) );
    CHECK( staged.value( "sets" ).toArray().first().toObject().value( "id" ) == QLatin1String( "rpc-evidence" ) );
    CHECK( resultObject( fixture.call( "highlighter.list" ) ) == before );
    QSignalSpy finished( &fixture.adapter, &ReviewControlAdapter::preparationFinished );
    QSignalSpy published( &HighlighterStatePublisher::instance(), &HighlighterStatePublisher::changed );
    auto* labels = fixture.controls().labels;
    QObject::connect( labels, &ColorLabelsController::labelsChanged, labels, [&] {
        CHECK( global.activeSetIds() == QStringList{ QStringLiteral( "rpc-evidence" ) } );
        CHECK( published.count() == 1 );
    } );
    const auto* focus = QApplication::focusWidget();
    const auto* active = QApplication::activeWindow();
    REQUIRE( resultObject( fixture.call( "transaction.commit", tx ) ).value( "status" ) == QLatin1String( "preparing" ) );
    CHECK( global.revision() == revision );
    CHECK( labels->snapshot().labels.front().isEmpty() );
    REQUIRE( waitUiState( [&] { return !finished.isEmpty(); } ) );
    CHECK( resultObject( fixture.call( "transaction.get", tx ) ).value( "status" ) == QLatin1String( "committed" ) );
    CHECK( published.count() == 1 );
    CHECK( global.revision() == revision + 1 );
    CHECK_FALSE( labels->snapshot().labels.front().isEmpty() );
    CHECK( QApplication::focusWidget() == focus );
    CHECK( QApplication::activeWindow() == active );
    CHECK( resultObject( fixture.call( "transaction.commit", tx ) ).value( "status" ) == QLatin1String( "committed" ) );
    CHECK( published.count() == 1 );
}

TEST_CASE( "Preparing control transactions reject human edits and detached owners without a prefix",
           "[ui][control-adapter][control-highlighter-rpc]" )
{
    const bool detach = GENERATE( false, true );
    ReviewFixture fixture;
    RestoreHighlighters restore;
    const auto before = HighlighterSetCollection::get().revision();
    const auto id = resultObject( fixture.call( "transaction.begin" ) ).value( "transaction_id" ).toString();
    const QJsonObject tx{ { "transaction_id", id } };
    auto change = replacement();
    change.insert( "transaction_id", id );
    resultObject( fixture.call( "highlighter.replace", change ) );
    resultObject( fixture.call( "label.set", { { "transaction_id", id }, { "tab_id", fixture.tabId() },
        { "label", 1 }, { "texts", QJsonArray{ "Agent" } } } ) );
    auto* labels = fixture.controls().labels;
    QSignalSpy finished( &fixture.adapter, &ReviewControlAdapter::preparationFinished );
    QSignalSpy published( &HighlighterStatePublisher::instance(), &HighlighterStatePublisher::changed );
    REQUIRE( resultObject( fixture.call( "transaction.commit", tx ) ).value( "status" ) == QLatin1String( "preparing" ) );
    // No event-loop turn: completion cannot overtake this human action.
    if ( detach ) fixture.window.findChild<TabbedCrawlerWidget*>()->removeCrawler( 0 );
    else {
        ColorLabelsController::Edit human;
        human.texts = QStringList{ QStringLiteral( "Human" ) };
        labels->apply( { human } );
    }
    REQUIRE( waitUiState( [&] { return !finished.isEmpty(); } ) );
    const auto result = resultObject( fixture.call( "transaction.get", tx ) );
    CHECK( result.value( "status" ) == QLatin1String( detach ? "target_closed" : "conflict" ) );
    CHECK( result.value( "error" ).toObject().value( "code" )
           == QLatin1String( detach ? "TARGET_CLOSED" : "REVISION_CONFLICT" ) );
    CHECK( HighlighterSetCollection::get().revision() == before );
    CHECK( published.isEmpty() );
    if ( detach ) CHECK( labels->snapshot().labels.front().isEmpty() );
    else CHECK( labels->snapshot().labels.front().front().text == QLatin1String( "Human" ) );
}

TEST_CASE( "Highlighter autocommit returns an inspectable transaction and reports compiler rejection",
           "[ui][control-adapter][control-highlighter-rpc]" )
{
    const bool invalid = GENERATE( false, true );
    ReviewFixture fixture;
    RestoreHighlighters restore;
    QSignalSpy finished( &fixture.adapter, &ReviewControlAdapter::preparationFinished );
    const auto started = resultObject( fixture.call( "highlighter.replace", replacement( invalid ? "(" : "Evidence" ) ) );
    REQUIRE( started.value( "status" ) == QLatin1String( "preparing" ) );
    const auto id = started.value( "transaction_id" ).toString();
    REQUIRE_FALSE( id.isEmpty() );
    REQUIRE( waitUiState( [&] { return !finished.isEmpty(); } ) );
    const auto result = resultObject( fixture.call( "transaction.get", { { "transaction_id", id } } ) );
    CHECK( result.value( "status" ) == QLatin1String( invalid ? "invalid_argument" : "committed" ) );
    if ( invalid ) CHECK( result.value( "error" ).toObject().value( "code" ) == QLatin1String( "INVALID_PARAMS" ) );
}

TEST_CASE( "Reset and expiry prevent late private highlighter publication without client polling",
           "[ui][control-adapter][control-highlighter-rpc]" )
{
    const bool reset = GENERATE( false, true );
    ReviewFixture fixture;
    RestoreHighlighters restore;
    qint64 now = 0;
    ReviewControlAdapter adapter{ [&] { return std::vector<QPointer<MainWindow>>{ &fixture.window }; },
                                  [&] { return now; } };
    const auto before = HighlighterSetCollection::get().revision();
    QSignalSpy finished( &adapter, &ReviewControlAdapter::preparationFinished );
    QSignalSpy published( &HighlighterStatePublisher::instance(), &HighlighterStatePublisher::changed );
    const auto started = resultObject( adapter.dispatch( { "id", "highlighter.replace", replacement() } ) );
    REQUIRE( started.value( "status" ) == QLatin1String( "preparing" ) );
    if ( reset ) adapter.reset();
    else now += ReviewControlAdapter::TransactionLifetimeMs;
    REQUIRE( waitUiState( [&] { return !finished.isEmpty(); } ) );
    CHECK( HighlighterSetCollection::get().revision() == before );
    CHECK( published.isEmpty() );
    const auto result = adapter.dispatch( { "id", "transaction.get",
        { { "transaction_id", started.value( "transaction_id" ) } } } );
    if ( reset ) CHECK( errorCode( result ) == QLatin1String( "TRANSACTION_NOT_FOUND" ) );
    else CHECK( resultObject( result ).value( "status" ) == QLatin1String( "expired" ) );
}

TEST_CASE( "Control highlighter reads reject oversized state and escaped wire responses",
           "[ui][control-adapter][control-highlighter-bounds]" )
{
    ReviewFixture fixture;
    RestoreHighlighters restore;
    SECTION( "collection cardinality is bounded before detachment" ) {
        QList<HighlighterSet> sets;
        for ( int i = 0; i < 129; ++i ) sets.append( HighlighterSet::createNewSet( "Imported" ) );
        HighlighterSetCollection::get().setHighlighterSets( sets );
    }
    SECTION( "JSON escaping cannot overflow the transport frame" ) {
        HighlighterSetCollection::get().setHighlighterSets( {
            HighlighterSet::fromDescription( "imported", QString( 100000, QChar( 1 ) ), {} ) } );
    }
    CHECK( errorCode( fixture.call( "highlighter.list" ) ) == QLatin1String( "RESOURCE_LIMIT" ) );
}

TEST_CASE( "Preparing transactions retain their admission slot and reject further staging",
           "[ui][control-adapter][control-highlighter-bounds]" )
{
    ReviewFixture fixture;
    RestoreHighlighters restore;
    QSignalSpy finished( &fixture.adapter, &ReviewControlAdapter::preparationFinished );
    const auto started = resultObject( fixture.call( "highlighter.replace", replacement() ) );
    REQUIRE( started.value( "status" ) == QLatin1String( "preparing" ) );
    const QJsonObject tx{ { "transaction_id", started.value( "transaction_id" ) } };
    CHECK( errorCode( fixture.call( "highlighter.list", tx ) ) == QLatin1String( "TRANSACTION_PREPARING" ) );
    for ( size_t i = 1; i < ReviewControlAdapter::MaxTransactions; ++i )
        resultObject( fixture.call( "transaction.begin" ) );
    CHECK( errorCode( fixture.call( "transaction.begin" ) ) == QLatin1String( "RESOURCE_LIMIT" ) );
    CHECK( resultObject( fixture.call( "transaction.get", tx ) ).value( "status" ) == QLatin1String( "preparing" ) );
    REQUIRE( waitUiState( [&] { return !finished.isEmpty(); } ) );
    CHECK( resultObject( fixture.call( "transaction.get", tx ) ).value( "status" ) == QLatin1String( "committed" ) );
}

TEST_CASE( "Highlighter publication tolerates an observer destroying the control adapter",
           "[ui][control-adapter][control-highlighter-rpc]" )
{
    ReviewFixture fixture;
    RestoreHighlighters restore;
    auto adapter = std::make_unique<ReviewControlAdapter>( [&] {
        return std::vector<QPointer<MainWindow>>{ &fixture.window };
    } );
    const auto id = resultObject( adapter->dispatch( { "id", "transaction.begin", {} } ) )
                        .value( "transaction_id" ).toString();
    auto change = replacement();
    change.insert( "transaction_id", id );
    resultObject( adapter->dispatch( { "id", "highlighter.replace", change } ) );
    auto* labels = fixture.controls().labels;
    resultObject( adapter->dispatch( { "id", "label.set", { { "transaction_id", id },
        { "tab_id", fixture.tabId() }, { "label", 1 }, { "texts", QJsonArray{ "Evidence" } } } } ) );
    QSignalSpy destroyed( adapter.get(), &QObject::destroyed );
    QSignalSpy labelNotifications( labels, &ColorLabelsController::labelsChanged );
    const auto connection = QObject::connect( &HighlighterStatePublisher::instance(),
        &HighlighterStatePublisher::changed, &fixture.window, [&] {
            CHECK_FALSE( labels->snapshot().labels.front().isEmpty() );
            adapter.reset();
        } );
    REQUIRE( resultObject( adapter->dispatch( { "id", "transaction.commit", { { "transaction_id", id } } } ) )
                 .value( "status" ) == QLatin1String( "preparing" ) );
    REQUIRE( waitUiState( [&] { return !destroyed.isEmpty(); } ) );
    QObject::disconnect( connection );
    CHECK( labelNotifications.count() == 1 );
    CHECK( HighlighterSetCollection::get().activeSetIds() == QStringList{ QStringLiteral( "rpc-evidence" ) } );
}

TEST_CASE( "Control adapter projects actual window and tab identities without GUI effects",
           "[ui][control-adapter]" )
{
    ReviewFixture fixture;
    const auto before = fixture.window.inventorySnapshot();
    const auto* focus = QApplication::focusWidget();
    const auto state = resultObject( fixture.call( "state.get" ) );
    CHECK( state.value( "scope" ).toString() == QStringLiteral( "window-tab-inventory" ) );
    const auto windows = state.value( "windows" ).toArray();
    REQUIRE( windows.size() == 1 );
    const auto window = windows.first().toObject();
    CHECK( window.value( "window_id" ).toString() == before.windowId );
    CHECK( window.value( "revision" ).toString() == QString::number( before.revision ) );
    CHECK( window.value( "current_tab_id" ).toString() == before.currentTabId );
    const auto tabs = window.value( "tabs" ).toArray();
    REQUIRE( tabs.size() == 2 );
    CHECK( tabs[ 0 ].toObject().value( "tab_id" ).toString() == fixture.tabId() );
    CHECK( tabs[ 1 ].toObject().value( "tab_id" ).toString() == fixture.tabId( 1 ) );
    CHECK( tabs[ 0 ].toObject().value( "kind" ).toString() == QStringLiteral( "folder" ) );
    CHECK_FALSE( tabs[ 0 ].toObject().contains( "text" ) );
    CHECK( QApplication::focusWidget() == focus );
    CHECK( fixture.window.inventorySnapshot().revision == before.revision );
    CHECK( fixture.window.inventorySnapshot().currentTabId == before.currentTabId );
}

TEST_CASE( "Control tab observation reads shared draft and labels without submitting a search",
           "[ui][control-adapter]" )
{
    ReviewFixture fixture;
    const auto controls = fixture.controls();
    REQUIRE( controls.filter != nullptr );
    REQUIRE( controls.labels != nullptr );
    ColorLabelsController::Edit edit;
    edit.texts = QStringList{ QStringLiteral( "Evidence" ) };
    REQUIRE( controls.labels->apply( { edit } ) == ColorLabelsController::EditResult::Applied );
    const auto draft = controls.filter->filterDraftSnapshot();
    const auto tab = resultObject( fixture.call( "tab.get", { { "tab_id", fixture.tabId() } } ) );
    CHECK( tab.value( "tab_id" ).toString() == fixture.tabId() );
    CHECK( tab.value( "filter_draft" ).toObject().value( "revision" ).toString()
           == QString::number( draft.revision ) );
    const auto labels = tab.value( "labels" ).toObject();
    CHECK( labels.value( "revision" ).toString()
           == QString::number( controls.labels->snapshot().revision ) );
    const auto entries = labels.value( "slots" ).toArray().first().toObject()
                             .value( "entries" ).toArray();
    REQUIRE( entries.size() == 1 );
    CHECK( entries.first().toObject().value( "text" ).toString() == QStringLiteral( "Evidence" ) );
    CHECK( controls.filter->filterDraftSnapshot().revision == draft.revision );
}

TEST_CASE( "Control pane inventory provides explicit handles without capturing source content",
           "[ui][control-adapter][control-pane-inventory]" )
{
    ReviewFixture fixture;
    const auto tabId = fixture.tabId();
    const auto listed = resultObject( fixture.call( "pane.list", { { "tab_id", tabId } } ) );
    const auto panes = listed.value( "panes" ).toArray();
    REQUIRE( panes.size() == 2 );
    for ( const auto& item : panes ) {
        const auto pane = item.toObject();
        const auto id = pane.value( "pane_id" ).toString();
        REQUIRE_FALSE( id.isEmpty() );
        const auto observed = resultObject( fixture.call( "pane.get", { { "tab_id", tabId }, { "pane_id", id } } ) );
        CHECK( observed.value( "pane_id" ) == id );
        CHECK( observed.value( "available" ) == pane.value( "available" ) );
        CHECK_FALSE( pane.contains( "text" ) );
        CHECK_FALSE( pane.contains( "snapshot_id" ) );
        CHECK( errorCode( fixture.call( "pane.get", { { "tab_id", tabId }, { "pane_id", id }, { "pane", "main" } } ) )
               == QLatin1String( "INVALID_PARAMS" ) );
    }
    CHECK( errorCode( fixture.call( "pane.get", { { "tab_id", tabId }, { "pane_id", "retired" } } ) )
           == QLatin1String( "PANE_NOT_FOUND" ) );
    CHECK( errorCode( fixture.call( "pane.list", { { "tab_id", tabId }, { "include_text", true } } ) )
           == QLatin1String( "INVALID_PARAMS" ) );
}

TEST_CASE( "Control captures a kept pane by identity without changing the active results page",
           "[ui][control-adapter][control-pane-inventory]" )
{
    ReviewFixture fixture;
    const auto tabId = fixture.tabId();
    auto* widget = qobject_cast<FolderCrawlerWidget*>( fixture.window.inventorySnapshot().tabs.front().widget.data() );
    REQUIRE( widget );
    auto* engine = widget->findChild<FolderSearchEngine*>();
    REQUIRE( engine );
    const auto search = [&]( const QString& pattern ) {
        QObject completion;
        bool finished = false;
        QObject::connect( engine, &FolderSearchEngine::searchFinished, &completion,
                          [&] { finished = true; }, Qt::QueuedConnection );
        widget->searchFor( pattern );
        REQUIRE( waitUiState( [&] { return finished && !widget->isSearchActive(); } ) );
    };
    search( "Evidence" );
    const auto panes = resultObject( fixture.call( "pane.list", { { "tab_id", tabId } } ) ).value( "panes" ).toArray();
    REQUIRE( panes.size() == 2 );
    const auto id = panes.last().toObject().value( "pane_id" ).toString();
    widget->searchToolbar()->setKeepResultsChecked( true );
    search( "absent" );
    const auto* focus = QApplication::focusWidget();
    const auto current = widget->resultsTabs()->currentIndex();
    const QJsonObject target{ { "tab_id", tabId }, { "pane_id", id } };
    CHECK( resultObject( fixture.call( "pane.get", target ) ).value( "row_count" ) == QLatin1String( "2" ) );
    auto capture = target;
    capture.insert( "scope", "matches" );
    const auto snapshot = resultObject( fixture.call( "source.capture", capture ) );
    CHECK( snapshot.value( "row_count" ) == QLatin1String( "1" ) );
    CHECK( widget->resultsTabs()->currentIndex() == current );
    CHECK( QApplication::focusWidget() == focus );
    REQUIRE( QMetaObject::invokeMethod( widget->resultsTabs(), "tabCloseRequested", Qt::DirectConnection, Q_ARG( int, 0 ) ) );
    CHECK( errorCode( fixture.call( "pane.get", target ) ) == QLatin1String( "PANE_NOT_FOUND" ) );
    CHECK( errorCode( fixture.call( "source.capture", capture ) ) == QLatin1String( "PANE_NOT_FOUND" ) );
    resultObject( fixture.call( "source.release", { { "snapshot_id", snapshot.value( "snapshot_id" ) } } ) );
}

TEST_CASE( "Control source-backed marks join label transactions and finalize without client polling",
           "[ui][control-adapter][control-mark-writes]" )
{
    const bool folderMode = GENERATE( false, true );
    ReviewFixture fixture;
    const auto tabId = folderMode ? fixture.tabId() : fixture.openEvidenceFileTab();
    if ( folderMode ) {
        auto* folder = qobject_cast<FolderCrawlerWidget*>( fixture.window.inventorySnapshot().tabs.front().widget.data() );
        REQUIRE( folder );
        auto* engine = folder->findChild<FolderSearchEngine*>();
        REQUIRE( engine );
        QObject completion;
        bool searched = false;
        QObject::connect( engine, &FolderSearchEngine::searchFinished, &completion,
                          [&] { searched = true; }, Qt::QueuedConnection );
        folder->searchFor( "Evidence" );
        REQUIRE( waitUiState( [&] { return searched && !folder->isSearchActive(); } ) );
    }
    auto change = markRequest( fixture, tabId, folderMode ? "results" : "main" );
    const QJsonObject read{ { "tab_id", tabId }, { "owner_id", change.value( "owner_id" ) } };
    const auto before = resultObject( fixture.call( "mark.list", read ) );
    const auto id = resultObject( fixture.call( "transaction.begin" ) ).value( "transaction_id" ).toString();
    const QJsonObject tx{ { "transaction_id", id } };
    change.insert( "transaction_id", id );
    change.insert( "expected_revision", before.value( "revision" ) );
    REQUIRE( resultObject( fixture.call( "mark.add", change ) ).value( "status" ) == QLatin1String( "open" ) );
    resultObject( fixture.call( "label.set", { { "transaction_id", id }, { "tab_id", fixture.tabId() },
        { "label", 1 }, { "texts", QJsonArray{ "Evidence" } } } ) );
    auto stagedRead = read;
    stagedRead.insert( "transaction_id", id );
    const auto groups = resultObject( fixture.call( "mark.list", stagedRead ) ).value( "groups" ).toArray();
    REQUIRE( groups.size() == 1 );
    CHECK( groups.first().toObject().value( "rows" ) == QJsonArray{ "0" } );
    CHECK( resultObject( fixture.call( "mark.list", read ) ) == before );
    // Staging keeps value proofs, not an extra pin on the client's evidence lease.
    resultObject( fixture.call( "source.release", { { "snapshot_id", change.value( "snapshot_id" ) } } ) );
    bool allInstalled = false;
    auto* labels = fixture.controls().labels;
    QObject::connect( labels, &ColorLabelsController::labelsChanged, labels, [&] {
        allInstalled = resultObject( fixture.call( "mark.list", read ) ).value( "groups" ).toArray().size() == 1;
    } );
    QSignalSpy finished( &fixture.adapter, &ReviewControlAdapter::preparationFinished );
    const auto* focus = QApplication::focusWidget();
    const auto* active = QApplication::activeWindow();
    REQUIRE( resultObject( fixture.call( "transaction.commit", tx ) ).value( "status" ) == QLatin1String( "preparing" ) );
    REQUIRE( waitUiState( [&] { return !finished.isEmpty(); } ) );
    CHECK( resultObject( fixture.call( "transaction.get", tx ) ).value( "status" ) == QLatin1String( "committed" ) );
    CHECK( allInstalled );
    CHECK( QApplication::focusWidget() == focus );
    CHECK( QApplication::activeWindow() == active );
}

TEST_CASE( "Control mark add remove replace and clear preserve native no-op revisions",
           "[ui][control-adapter][control-mark-writes]" )
{
    ReviewFixture fixture;
    const auto tabId = fixture.openEvidenceFileTab();
    const auto change = markRequest( fixture, tabId, "main" );
    const QJsonObject read{ { "tab_id", tabId }, { "owner_id", change.value( "owner_id" ) } };
    const auto complete = [&]( const QString& method, const QJsonObject& params ) {
        QSignalSpy finished( &fixture.adapter, &ReviewControlAdapter::preparationFinished );
        auto result = resultObject( fixture.call( method, params ) );
        if ( result.value( "status" ) == QLatin1String( "preparing" ) ) {
            REQUIRE( waitUiState( [&] { return !finished.isEmpty(); } ) );
            result = resultObject( fixture.call( "transaction.get", { { "transaction_id", result.value( "transaction_id" ) } } ) );
        }
        REQUIRE( result.value( "status" ) == QLatin1String( "committed" ) );
    };
    complete( "mark.add", change );
    const auto marked = resultObject( fixture.call( "mark.list", read ) );
    complete( "mark.replace", change );
    CHECK( resultObject( fixture.call( "mark.list", read ) ) == marked );
    SECTION( "source-backed removal" ) { complete( "mark.remove", change ); }
    SECTION( "clear needs no source lease" ) { complete( "mark.clear", read ); }
    CHECK( resultObject( fixture.call( "mark.list", read ) ).value( "groups" ).toArray().isEmpty() );
    auto stale = change;
    stale.insert( "expected_revision", marked.value( "revision" ) );
    CHECK( errorCode( fixture.call( "mark.add", stale ) ) == QLatin1String( "REVISION_CONFLICT" ) );
}

TEST_CASE( "Invalid mark wire arguments reject the whole staged prefix before publication",
           "[ui][control-adapter][control-mark-writes]" )
{
    ReviewFixture fixture;
    const auto tabId = fixture.openEvidenceFileTab();
    auto change = markRequest( fixture, tabId, "main" );
    const auto id = resultObject( fixture.call( "transaction.begin" ) ).value( "transaction_id" ).toString();
    change.insert( "transaction_id", id );
    resultObject( fixture.call( "label.set", { { "transaction_id", id }, { "tab_id", fixture.tabId() },
        { "label", 1 }, { "texts", QJsonArray{ "Unpublished" } } } ) );
    QString expected = QStringLiteral( "INVALID_PARAMS" );
    SECTION( "numeric rather than canonical string row" ) { change.insert( "rows", QJsonArray{ 0 } ); }
    SECTION( "noncanonical row" ) { change.insert( "rows", QJsonArray{ "00" } ); }
    SECTION( "empty rows" ) { change.insert( "rows", QJsonArray{} ); }
    SECTION( "no evidence lease" ) { change.remove( "snapshot_id" ); }
    SECTION( "raw path is not an evidence address" ) { change.insert( "member_path", "/arbitrary/path" ); }
    SECTION( "released evidence" ) {
        resultObject( fixture.call( "source.release", { { "snapshot_id", change.value( "snapshot_id" ) } } ) );
        expected = QStringLiteral( "SNAPSHOT_NOT_FOUND" );
    }
    SECTION( "bounded row admission" ) {
        QJsonArray rows;
        for ( uint64_t index = 0; index <= ReviewContentAdapter::MaxRows; ++index ) rows.append( "0" );
        change.insert( "rows", rows );
        expected = QStringLiteral( "RESOURCE_LIMIT" );
    }
    CHECK( errorCode( fixture.call( "mark.add", change ) ) == expected );
    CHECK( errorCode( fixture.call( "transaction.commit", { { "transaction_id", id } } ) ) == expected );
    CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
}

TEST_CASE( "Control mark completion revalidates closed tabs and global reset",
           "[ui][control-adapter][control-mark-writes]" )
{
    ReviewFixture fixture;
    const auto tabId = fixture.openEvidenceFileTab();
    const auto change = markRequest( fixture, tabId, "main" );
    QSignalSpy finished( &fixture.adapter, &ReviewControlAdapter::preparationFinished );
    const auto started = resultObject( fixture.call( "mark.add", change ) );
    REQUIRE( started.value( "status" ) == QLatin1String( "preparing" ) );
    const QJsonObject tx{ { "transaction_id", started.value( "transaction_id" ) } };
    bool reset = false;
    SECTION( "tab removed during preparation" ) {
        auto* tabs = fixture.window.findChild<TabbedCrawlerWidget*>();
        REQUIRE( tabs );
        tabs->removeCrawler( 2 );
    }
    SECTION( "global controller reset" ) { fixture.adapter.reset(); reset = true; }
    REQUIRE( waitUiState( [&] { return !finished.isEmpty(); } ) );
    if ( reset ) {
        CHECK( errorCode( fixture.call( "transaction.get", tx ) ) == QLatin1String( "TRANSACTION_NOT_FOUND" ) );
        CHECK( resultObject( fixture.call( "mark.list", { { "tab_id", tabId }, { "owner_id", change.value( "owner_id" ) } } ) )
                   .value( "groups" ).toArray().isEmpty() );
    }
    else CHECK( resultObject( fixture.call( "transaction.get", tx ) ).value( "status" ) == QLatin1String( "target_closed" ) );
}

TEST_CASE( "Rejected transaction commands cannot leave a valid staged prefix committable",
           "[ui][control-adapter][control-transactions][control-rejected-prefix]" )
{
    const QString method = GENERATE( "mark.unknown-command", "state.get", "pane.list", "source.read" );
    const auto expected = method == QLatin1String( "mark.unknown-command" )
        ? QStringLiteral( "METHOD_NOT_FOUND" ) : QStringLiteral( "INVALID_PARAMS" );
    ReviewFixture fixture;
    const auto id = resultObject( fixture.call( "transaction.begin" ) ).value( "transaction_id" ).toString();
    const QJsonObject tx{ { "transaction_id", id } };
    resultObject( fixture.call( "label.set", { { "transaction_id", id }, { "tab_id", fixture.tabId() },
        { "label", 1 }, { "texts", QJsonArray{ "Unpublished" } } } ) );
    CHECK( errorCode( fixture.call( method, tx ) ) == expected );
    CHECK( errorCode( fixture.call( "transaction.commit", tx ) ) == expected );
    CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
}

TEST_CASE( "Control mark reads bind the shared owner and conflict with human changes",
           "[ui][control-adapter][control-marks-rpc]" )
{
    const bool asynchronous = GENERATE( false, true );
    ReviewFixture fixture;
    RestoreHighlighters restore;
    const auto globalRevision = HighlighterSetCollection::get().revision();
    const auto tabId = fixture.tabId();
    auto* widget = qobject_cast<FolderCrawlerWidget*>( fixture.window.inventorySnapshot().tabs.front().widget.data() );
    REQUIRE( widget );
    widget->setViewContext( QStringLiteral( "{\"M\":{\"evidence.log\":[0]}}" ) );
    const auto panes = resultObject( fixture.call( "pane.list", { { "tab_id", tabId } } ) ).value( "panes" ).toArray();
    REQUIRE( panes.size() == 2 );
    const auto ownerId = panes.last().toObject().value( "marks_owner_id" ).toString();
    REQUIRE_FALSE( ownerId.isEmpty() );
    CHECK( panes.first().toObject().value( "marks_owner_id" ) == ownerId );
    const auto id = resultObject( fixture.call( "transaction.begin" ) ).value( "transaction_id" ).toString();
    const QJsonObject tx{ { "transaction_id", id } };
    const QJsonObject read{ { "tab_id", tabId }, { "owner_id", ownerId }, { "transaction_id", id } };
    const auto before = resultObject( fixture.call( "mark.list", read ) );
    CHECK( before.value( "scope" ) == QLatin1String( "folder-tab" ) );
    CHECK( before.value( "coordinates" ) == QLatin1String( "stored-source-model-zero-based" ) );
    const auto groups = before.value( "groups" ).toArray();
    REQUIRE( groups.size() == 1 );
    CHECK( groups.first().toObject().value( "rows" ) == QJsonArray{ "0" } );
    CHECK( groups.first().toObject().value( "member_path" ) == fixture.directory.filePath( "evidence.log" ) );
    resultObject( fixture.call( "label.set", { { "transaction_id", id }, { "tab_id", tabId },
        { "label", 1 }, { "texts", QJsonArray{ "Unpublished" } } } ) );
    QSignalSpy finished( &fixture.adapter, &ReviewControlAdapter::preparationFinished );
    if ( asynchronous ) {
        auto change = replacement();
        change.insert( "transaction_id", id );
        resultObject( fixture.call( "highlighter.replace", change ) );
        REQUIRE( resultObject( fixture.call( "transaction.commit", tx ) ).value( "status" ) == QLatin1String( "preparing" ) );
    }
    widget->setViewContext( QStringLiteral( "{}" ) );
    if ( asynchronous ) {
        REQUIRE( waitUiState( [&] { return !finished.isEmpty(); } ) );
        CHECK( resultObject( fixture.call( "transaction.get", tx ) ).value( "status" ) == QLatin1String( "conflict" ) );
    }
    else {
        CHECK( resultObject( fixture.call( "mark.list", read ) ) == before );
        CHECK( errorCode( fixture.call( "transaction.commit", tx ) ) == QLatin1String( "REVISION_CONFLICT" ) );
    }
    CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
    CHECK( HighlighterSetCollection::get().revision() == globalRevision );
}

TEST_CASE( "Mark owner IDs are scoped to document membership and never rebound by revision alone",
           "[ui][control-adapter][control-marks-rpc]" )
{
    ReviewFixture fixture;
    const auto fileTab = fixture.openEvidenceFileTab();
    const auto panes = resultObject( fixture.call( "pane.list", { { "tab_id", fileTab } } ) ).value( "panes" ).toArray();
    REQUIRE( panes.size() == 2 );
    const auto ownerId = panes.last().toObject().value( "marks_owner_id" ).toString();
    REQUIRE_FALSE( ownerId.isEmpty() );
    CHECK( panes.first().toObject().value( "marks_owner_id" ) == ownerId );
    CHECK( errorCode( fixture.call( "mark.list", { { "tab_id", fixture.tabId() }, { "owner_id", ownerId } } ) )
           == QLatin1String( "MARK_OWNER_NOT_FOUND" ) );
    const auto id = resultObject( fixture.call( "transaction.begin" ) ).value( "transaction_id" ).toString();
    const auto state = resultObject( fixture.call( "mark.list", { { "tab_id", fileTab },
        { "owner_id", ownerId }, { "transaction_id", id } } ) );
    CHECK( state.value( "scope" ) == QLatin1String( "source-model" ) );
    CHECK( state.value( "groups" ).toArray().isEmpty() );
    resultObject( fixture.call( "label.set", { { "transaction_id", id }, { "tab_id", fixture.tabId() },
        { "label", 1 }, { "texts", QJsonArray{ "Unpublished" } } } ) );
    auto* tabs = fixture.window.findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabs );
    tabs->removeCrawler( 2 );
    CHECK( errorCode( fixture.call( "transaction.commit", { { "transaction_id", id } } ) )
           == QLatin1String( "TARGET_CLOSED" ) );
    CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
}

TEST_CASE( "Kept file result models retain distinct mark scopes across current-page changes",
           "[ui][control-adapter][control-marks-rpc]" )
{
    ReviewFixture fixture;
    const auto fileTab = fixture.openEvidenceFileTab();
    auto* widget = qobject_cast<CrawlerWidget*>( fixture.window.inventorySnapshot().tabs.back().widget.data() );
    REQUIRE( widget );
    const auto owners = widget->reviewMarksOwners();
    REQUIRE( owners.size() == 1 );
    auto* first = dynamic_cast<LogFilteredData*>( owners.front() );
    REQUIRE( first );
    first->addMark( 0_lnum );
    const auto firstId = first->reviewMarksId();
    const auto begin = [&] {
        const auto id = resultObject( fixture.call( "transaction.begin" ) ).value( "transaction_id" ).toString();
        resultObject( fixture.call( "mark.list", { { "tab_id", fileTab }, { "owner_id", firstId }, { "transaction_id", id } } ) );
        resultObject( fixture.call( "label.set", { { "tab_id", fixture.tabId() }, { "transaction_id", id },
            { "label", 1 }, { "texts", QJsonArray{ id } } } ) );
        return id;
    };
    const auto id = begin();
    widget->reviewControls().filter->setKeepResultsChecked( true );
    widget->reviewControls().filter->searchLineEdit()->setEditText( "absent" );
    REQUIRE( QMetaObject::invokeMethod( widget, "startNewSearch", Qt::DirectConnection ) );
    const auto panes = resultObject( fixture.call( "pane.list", { { "tab_id", fileTab } } ) ).value( "panes" ).toArray();
    REQUIRE( panes.size() == 3 );
    CHECK( panes.at( 1 ).toObject().value( "marks_owner_id" ) == firstId );
    const auto secondId = panes.at( 2 ).toObject().value( "marks_owner_id" ).toString();
    REQUIRE_FALSE( secondId.isEmpty() );
    CHECK( secondId != firstId );
    CHECK( panes.first().toObject().value( "marks_owner_id" ) == secondId );
    CHECK( resultObject( fixture.call( "mark.list", { { "tab_id", fileTab }, { "owner_id", secondId } } ) )
               .value( "groups" ).toArray().isEmpty() );
    CHECK( resultObject( fixture.call( "transaction.commit", { { "transaction_id", id } } ) )
               .value( "status" ) == QLatin1String( "committed" ) );
    const auto closing = begin();
    const auto labelRevision = fixture.controls().labels->snapshot().revision;
    REQUIRE( QMetaObject::invokeMethod( widget, "closeFilteredView", Qt::DirectConnection, Q_ARG( int, 0 ) ) );
    CHECK( errorCode( fixture.call( "mark.list", { { "tab_id", fileTab }, { "owner_id", firstId } } ) )
           == QLatin1String( "MARK_OWNER_NOT_FOUND" ) );
    CHECK( errorCode( fixture.call( "transaction.commit", { { "transaction_id", closing } } ) )
           == QLatin1String( "TARGET_CLOSED" ) );
    CHECK( fixture.controls().labels->snapshot().revision == labelRevision );
}

TEST_CASE( "Control pane observation reports presentation coordinates without content or activation",
           "[ui][control-adapter][control-panes]" )
{
    ReviewFixture fixture;
    const auto before = fixture.window.inventorySnapshot();
    const auto* focus = QApplication::focusWidget();
    const auto* active = QApplication::activeWindow();
    for ( const auto* name : { "main", "results" } ) {
        const auto pane = resultObject( fixture.call( "pane.get",
            { { "tab_id", fixture.tabId() }, { "pane", name } } ) );
        CHECK( pane.value( "tab_id" ).toString() == fixture.tabId() );
        CHECK( pane.value( "pane" ).toString() == QLatin1String( name ) );
        CHECK( pane.value( "coordinates" ).toString() == QStringLiteral( "presented-model-zero-based" ) );
        CHECK( pane.value( "available" ).isBool() );
        CHECK( pane.value( "row_count" ).isString() );
        CHECK( pane.value( "top_row" ).isNull() );
        CHECK( pane.value( "follow" ).isBool() );
        CHECK( pane.value( "wrap" ).isBool() );
        CHECK( pane.value( "visible" ).isBool() );
        CHECK( pane.value( "focused" ).isBool() );
        CHECK_FALSE( pane.contains( "text" ) );
        CHECK_FALSE( pane.contains( "selection_text" ) );
    }
    CHECK( QApplication::focusWidget() == focus );
    CHECK( QApplication::activeWindow() == active );
    CHECK( fixture.window.inventorySnapshot().revision == before.revision );
    CHECK( errorCode( fixture.call( "pane.get", { { "tab_id", fixture.tabId() }, { "pane", "other" } } ) )
           == QStringLiteral( "INVALID_PARAMS" ) );
    CHECK( errorCode( fixture.call( "pane.get", { { "tab_id", fixture.tabId() }, { "pane", "main" },
                                                  { "include_text", true } } ) )
           == QStringLiteral( "INVALID_PARAMS" ) );
    CHECK( errorCode( fixture.call( "pane.get", { { "tab_id", "closed" }, { "pane", "main" } } ) )
           == QStringLiteral( "TARGET_NOT_FOUND" ) );
}

TEST_CASE( "Control content RPC reads a retained file snapshot through asynchronous operations",
           "[ui][control-adapter][control-content-rpc]" )
{
    ReviewFixture fixture;
    const auto tabId = fixture.openEvidenceFileTab();
    const auto* focus = QApplication::focusWidget();
    const auto captured = resultObject( fixture.call( "source.capture",
        { { "tab_id", tabId }, { "pane", "main" }, { "scope", "presented" } } ) );
    CHECK( captured.value( "row_count" ) == QLatin1String( "1" ) );
    const auto snapshot = captured.value( "snapshot_id" ).toString();
    REQUIRE_FALSE( snapshot.isEmpty() );
    const auto started = resultObject( fixture.call( "source.read",
        { { "snapshot_id", snapshot }, { "start", "0" }, { "count", 1 } } ) );
    const auto operation = started.value( "operation_id" ).toString();
    REQUIRE_FALSE( operation.isEmpty() );
    resultObject( fixture.call( "source.release", { { "snapshot_id", snapshot } } ) );
    QJsonObject completed;
    REQUIRE( waitUiState( [&] {
        completed = resultObject( fixture.call( "operation.get", { { "operation_id", operation } } ) );
        return completed.value( "status" ) != QLatin1String( "pending" );
    } ) );
    CHECK( completed.value( "status" ) == QLatin1String( "completed" ) );
    const auto rows = completed.value( "result" ).toObject().value( "rows" ).toArray();
    REQUIRE( rows.size() == 1 );
    CHECK( rows.first().toObject().value( "text" ) == QLatin1String( "Evidence" ) );
    CHECK( QApplication::focusWidget() == focus );
    for ( const auto* scope : { "all", "matches" } ) {
        CHECK( errorCode( fixture.call( "source.capture", { { "tab_id", tabId }, { "pane", "main" },
                                                          { "scope", scope } } ) )
               == QStringLiteral( "INVALID_PARAMS" ) );
    }
    fixture.adapter.reset();
    CHECK( errorCode( fixture.call( "operation.get", { { "operation_id", operation } } ) )
           == QStringLiteral( "OPERATION_NOT_FOUND" ) );
}

TEST_CASE( "Control adapter rejects unknown methods and malformed or stale targets",
           "[ui][control-adapter]" )
{
    ReviewFixture fixture;
    CHECK( errorCode( fixture.call( "QObject.invoke" ) ) == QStringLiteral( "METHOD_NOT_FOUND" ) );
    CHECK( errorCode( fixture.call( "tab.get" ) ) == QStringLiteral( "INVALID_PARAMS" ) );
    CHECK( errorCode( fixture.call( "tab.get", { { "tab_id", 42 } } ) )
           == QStringLiteral( "INVALID_PARAMS" ) );
    CHECK( errorCode( fixture.call( "tab.get", { { "tab_id", "retired-tab" } } ) )
           == QStringLiteral( "TARGET_NOT_FOUND" ) );
    CHECK( errorCode( fixture.call( "state.get", { { "include_text", true } } ) )
           == QStringLiteral( "INVALID_PARAMS" ) );

    ReviewControlAdapter empty{ [] { return std::vector<QPointer<MainWindow>>{ nullptr }; } };
    CHECK( resultObject( empty.dispatch( { "id", "state.get", {} } ) )
               .value( "windows" ).toArray().isEmpty() );
}

TEST_CASE( "Control transactions stage shared labels and publish all owners before notification",
           "[ui][control-adapter][control-transactions]" )
{
    ReviewFixture fixture;
    const auto first = fixture.controls().labels;
    const auto second = fixture.controls( 1 ).labels;
    const auto transaction = resultObject( fixture.call( "transaction.begin" ) )
                                 .value( "transaction_id" ).toString();
    REQUIRE_FALSE( transaction.isEmpty() );
    const QJsonObject transactionParams{ { "transaction_id", transaction } };
    for ( size_t index = 0; index < 2; ++index ) {
        CHECK( resultObject( fixture.call( "label.set",
            { { "transaction_id", transaction }, { "tab_id", fixture.tabId( index ) },
              { "label", 1 }, { "texts", QJsonArray{ "Evidence" } },
              { "ignore_case", false }, { "whole_word", true } } ) )
                   .value( "status" ).toString() == QStringLiteral( "open" ) );
    }
    CHECK( first->snapshot().labels.front().isEmpty() );
    CHECK( second->snapshot().labels.front().isEmpty() );
    const auto staged = resultObject( fixture.call( "label.list",
        { { "transaction_id", transaction }, { "tab_id", fixture.tabId() } } ) );
    const auto entry = staged.value( "slots" ).toArray().first().toObject()
                           .value( "entries" ).toArray().first().toObject();
    CHECK( entry.value( "text" ).toString() == QStringLiteral( "Evidence" ) );
    CHECK_FALSE( entry.value( "ignore_case" ).toBool() );
    CHECK( entry.value( "whole_word" ).toBool() );
    int notifications = 0;
    QObject::connect( first, &ColorLabelsController::labelsChanged, first, [ & ] {
        ++notifications;
        CHECK_FALSE( second->snapshot().labels.front().isEmpty() );
    } );
    const auto* focus = QApplication::focusWidget();
    const auto before = fixture.window.inventorySnapshot();
    CHECK( resultObject( fixture.call( "transaction.commit", transactionParams ) )
               .value( "status" ).toString() == QStringLiteral( "committed" ) );
    CHECK( resultObject( fixture.call( "transaction.commit", transactionParams ) )
               .value( "status" ).toString() == QStringLiteral( "committed" ) );
    CHECK( notifications == 1 );
    CHECK( first->snapshot().labels.front().front().text == QStringLiteral( "Evidence" ) );
    CHECK( QApplication::focusWidget() == focus );
    CHECK( fixture.window.inventorySnapshot().revision == before.revision );
}

TEST_CASE( "Control transaction read dependencies reject intervening human edits",
           "[ui][control-adapter][control-transactions]" )
{
    ReviewFixture fixture;
    const auto transaction = resultObject( fixture.call( "transaction.begin" ) )
                                 .value( "transaction_id" ).toString();
    resultObject( fixture.call( "label.set",
        { { "transaction_id", transaction }, { "tab_id", fixture.tabId() },
          { "label", 1 }, { "texts", QJsonArray{ "Agent" } } } ) );
    resultObject( fixture.call( "tab.get",
        { { "transaction_id", transaction }, { "tab_id", fixture.tabId( 1 ) } } ) );
    ColorLabelsController::Edit human;
    human.texts = QStringList{ QStringLiteral( "Human" ) };
    REQUIRE( fixture.controls( 1 ).labels->apply( { human } )
             == ColorLabelsController::EditResult::Applied );
    CHECK( errorCode( fixture.call( "transaction.commit", { { "transaction_id", transaction } } ) )
           == QStringLiteral( "REVISION_CONFLICT" ) );
    CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
    CHECK( fixture.controls( 1 ).labels->snapshot().labels.front().front().text
           == QStringLiteral( "Human" ) );
    CHECK( resultObject( fixture.call( "transaction.get", { { "transaction_id", transaction } } ) )
               .value( "status" ).toString() == QStringLiteral( "conflict" ) );
}

TEST_CASE( "Control transaction rejects detached targets even before deferred deletion",
           "[ui][control-adapter][control-transactions]" )
{
    ReviewFixture fixture;
    const auto transaction = resultObject( fixture.call( "transaction.begin" ) )
                                 .value( "transaction_id" ).toString();
    const auto owner = fixture.controls().labels;
    resultObject( fixture.call( "label.set",
        { { "transaction_id", transaction }, { "tab_id", fixture.tabId() },
          { "label", 1 }, { "texts", QJsonArray{ "Agent" } } } ) );
    auto* tabs = fixture.window.findChild<TabbedCrawlerWidget*>();
    REQUIRE( tabs != nullptr );
    tabs->removeCrawler( 0 );
    CHECK( errorCode( fixture.call( "transaction.commit", { { "transaction_id", transaction } } ) )
           == QStringLiteral( "TARGET_CLOSED" ) );
    CHECK( owner->snapshot().labels.front().isEmpty() );
}

TEST_CASE( "Invalid staged control edits cannot publish a previously valid prefix",
           "[ui][control-adapter][control-transactions]" )
{
    ReviewFixture fixture;
    const auto transaction = resultObject( fixture.call( "transaction.begin" ) )
                                 .value( "transaction_id" ).toString();
    const QJsonObject valid{ { "transaction_id", transaction }, { "tab_id", fixture.tabId() },
                             { "label", 1 }, { "texts", QJsonArray{ "Agent" } } };
    resultObject( fixture.call( "label.set", valid ) );
    auto invalid = valid;
    invalid.insert( "label", 0 );
    CHECK( errorCode( fixture.call( "label.set", invalid ) ) == QStringLiteral( "INVALID_PARAMS" ) );
    CHECK( errorCode( fixture.call( "transaction.commit", { { "transaction_id", transaction } } ) )
           == QStringLiteral( "INVALID_PARAMS" ) );
    CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
}

TEST_CASE( "Standalone label edits autocommit with strict optional revision preconditions",
           "[ui][control-adapter][control-transactions]" )
{
    ReviewFixture fixture;
    const auto revision = fixture.controls().labels->snapshot().revision;
    const QJsonObject edit{ { "tab_id", fixture.tabId() }, { "label", 1 },
                            { "texts", QJsonArray{ "Evidence" } },
                            { "expected_revision", QString::number( revision ) } };
    CHECK( resultObject( fixture.call( "label.set", edit ) ).value( "status" ).toString()
           == QStringLiteral( "committed" ) );
    CHECK( errorCode( fixture.call( "label.set", edit ) ) == QStringLiteral( "REVISION_CONFLICT" ) );
    CHECK( errorCode( fixture.call( "label.clear",
        { { "tab_id", fixture.tabId() }, { "expected_revision", 1 } } ) )
           == QStringLiteral( "INVALID_PARAMS" ) );
    resultObject( fixture.call( "label.remove", { { "tab_id", fixture.tabId() },
                                                 { "texts", QJsonArray{ "Evidence" } } } ) );
    CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
}

TEST_CASE( "Control transaction leases expire and release admission capacity without publication",
           "[ui][control-adapter][control-lease]" )
{
    ReviewFixture fixture;
    qint64 now = 0;
    ReviewControlAdapter adapter{ [ & ] {
        return std::vector<QPointer<MainWindow>>{ &fixture.window };
    }, [ & ] { return now; } };
    const auto call = [ & ]( const QString& method, const QJsonObject& params = QJsonObject{} ) {
        return adapter.dispatch( { "lease", method, params } );
    };
    const auto id = resultObject( call( "transaction.begin" ) ).value( "transaction_id" ).toString();
    resultObject( call( "label.set", { { "transaction_id", id }, { "tab_id", fixture.tabId() },
                                       { "label", 1 }, { "texts", QJsonArray{ "Expired" } } } ) );
    for ( size_t index = 1; index < ReviewControlAdapter::MaxTransactions; ++index )
        resultObject( call( "transaction.begin" ) );
    CHECK( errorCode( call( "transaction.begin" ) ) == QStringLiteral( "RESOURCE_LIMIT" ) );
    now += ReviewControlAdapter::TransactionLifetimeMs;
    CHECK( errorCode( call( "transaction.commit", { { "transaction_id", id } } ) )
           == QStringLiteral( "TRANSACTION_EXPIRED" ) );
    CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
    CHECK( resultObject( call( "transaction.get", { { "transaction_id", id } } ) )
               .value( "status" ).toString() == QStringLiteral( "expired" ) );
    resultObject( call( "transaction.begin" ) );
}

TEST_CASE( "Control reset discards unpublished transactions and safely tolerates commit observers",
           "[ui][control-adapter][control-lease]" )
{
    ReviewFixture fixture;
    const auto begin = [ & ] {
        return resultObject( fixture.call( "transaction.begin" ) ).value( "transaction_id" ).toString();
    };
    const auto stage = [ & ]( const QString& id ) {
        resultObject( fixture.call( "label.set", { { "transaction_id", id },
            { "tab_id", fixture.tabId() }, { "label", 1 }, { "texts", QJsonArray{ "Evidence" } } } ) );
    };
    auto id = begin();
    stage( id );
    fixture.adapter.reset();
    CHECK( errorCode( fixture.call( "transaction.commit", { { "transaction_id", id } } ) )
           == QStringLiteral( "TRANSACTION_NOT_FOUND" ) );
    CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
    id = begin();
    stage( id );
    auto* owner = fixture.controls().labels;
    QObject::connect( owner, &ColorLabelsController::labelsChanged, owner,
                      [ & ] { fixture.adapter.reset(); } );
    CHECK( resultObject( fixture.call( "transaction.commit", { { "transaction_id", id } } ) )
               .value( "status" ).toString() == QStringLiteral( "committed" ) );
    CHECK( owner->snapshot().labels.front().front().text == QStringLiteral( "Evidence" ) );
}

TEST_CASE( "Control tab reads retain draft preconditions while unrelated labels do not conflict",
           "[ui][control-adapter][control-transactions]" )
{
    const bool readOther = GENERATE( false, true );
    ReviewFixture fixture;
    const auto id = resultObject( fixture.call( "transaction.begin" ) ).value( "transaction_id" ).toString();
    resultObject( fixture.call( "label.set", { { "transaction_id", id },
        { "tab_id", fixture.tabId() }, { "label", 1 }, { "texts", QJsonArray{ "Agent" } } } ) );
    const QJsonObject readParams{ { "tab_id", fixture.tabId( 1 ) }, { "transaction_id", id } };
    if ( readOther ) resultObject( fixture.call( "tab.get", readParams ) );
    fixture.controls( 1 ).filter->searchLineEdit()->setEditText( "Human draft" );
    if ( readOther ) {
        CHECK( resultObject( fixture.call( "tab.get", readParams ) ).value( "filter_draft" )
                   .toObject().value( "text" ).toString().isEmpty() );
        CHECK( errorCode( fixture.call( "transaction.commit", { { "transaction_id", id } } ) )
               == QStringLiteral( "REVISION_CONFLICT" ) );
        CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
    }
    else {
        CHECK( resultObject( fixture.call( "transaction.commit", { { "transaction_id", id } } ) )
                   .value( "status" ).toString() == QStringLiteral( "committed" ) );
    }
}

TEST_CASE( "Local service exposes real GUI owners and global disable discards staged writes",
           "[ui][control-service]" )
{
    ReviewFixture fixture;
    fixture.window.show();
    QCoreApplication::processEvents();
    RuntimeOptions runtime;
    runtime.descriptorRoot = QFileInfo( fixture.directory.path() ).canonicalFilePath() + "/control";
    ReviewControlService service{ [ & ] {
        return std::vector<QPointer<MainWindow>>{ &fixture.window };
    }, runtime };
    CHECK_FALSE( service.isRunning() );
    REQUIRE( service.setEnabled( Configuration{}.localControlEnabled() ) );
    REQUIRE( service.isRunning() );
    REQUIRE( service.lastError().isEmpty() );
    auto discovered = discoverInstances( runtime );
    REQUIRE( discovered.error.isEmpty() );
    REQUIRE( discovered.instances.size() == 1 );
    const auto descriptor = discovered.instances.front();
    CHECK( service.instanceId() == descriptor.instanceId );
    REQUIRE( service.setEnabled( true ) );
    CHECK( service.instanceId() == descriptor.instanceId );
    const auto* focus = QApplication::focusWidget();
    const auto current = fixture.window.inventorySnapshot().currentTabId;
    const auto state = resultObject( localRequest( descriptor, "state.get" ) );
    REQUIRE( state.value( "windows" ).toArray().size() == 1 );
    const auto transaction = resultObject( localRequest( descriptor, "transaction.begin" ) )
                                 .value( "transaction_id" ).toString();
    resultObject( localRequest( descriptor, "label.set", { { "transaction_id", transaction },
        { "tab_id", fixture.tabId() }, { "label", 1 }, { "texts", QJsonArray{ "Staged" } } } ) );
    CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
    CHECK( QApplication::focusWidget() == focus );
    CHECK( fixture.window.inventorySnapshot().currentTabId == current );
    REQUIRE( service.setEnabled( false ) );
    CHECK_FALSE( service.isRunning() );
    CHECK( discoverInstances( runtime ).instances.empty() );
    REQUIRE( service.setEnabled( true ) );
    discovered = discoverInstances( runtime );
    REQUIRE( discovered.instances.size() == 1 );
    const auto next = discovered.instances.front();
    CHECK( next.instanceId != descriptor.instanceId );
    CHECK( errorCode( localRequest( next, "transaction.commit", { { "transaction_id", transaction } } ) )
           == QStringLiteral( "TRANSACTION_NOT_FOUND" ) );
    CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
    resultObject( localRequest( next, "label.set", { { "tab_id", fixture.tabId() },
        { "label", 1 }, { "texts", QJsonArray{ "Committed" } } } ) );
    CHECK( fixture.controls().labels->snapshot().labels.front().front().text
           == QStringLiteral( "Committed" ) );
    CHECK( QApplication::focusWidget() == focus );
}

TEST_CASE( "Settings Apply reaches the application policy observer before the dialog closes",
           "[ui][control-service][control-policy]" )
{
    ReviewFixture fixture;
    auto& config = Configuration::get();
    struct Restore {
        QString style;
        bool enabled;
        ~Restore()
        {
            Configuration::get().setStyle( style );
            Configuration::get().setLocalControlEnabled( enabled );
            Configuration::get().save();
            // Options Apply also changes the process logger. Restore the
            // qtests_main policy before unrelated live-presentation tests run.
            logging::enableLogging( true, logging::LogLevel::Warning );
        }
    } restore{ config.style(), config.localControlEnabled() };
    config.setStyle( StyleManager::DarkStyleKey );
    config.setLocalControlEnabled( true );
    RuntimeOptions runtime;
    runtime.descriptorRoot = QFileInfo( fixture.directory.path() ).canonicalFilePath() + "/control";
    ReviewControlService service{ [ & ] {
        return std::vector<QPointer<MainWindow>>{ &fixture.window };
    }, runtime };
    REQUIRE( service.setEnabled( true ) );
    int notifications = 0;
    QObject::connect( &fixture.window, &MainWindow::optionsChanged, &service, [ & ] {
        ++notifications;
        service.setEnabled( Configuration::get().localControlEnabled() );
    } );
    bool applied = false;
    bool stoppedBeforeClose = false;
    QTimer::singleShot( 0, Qt::PreciseTimer, &fixture.window, [ & ] {
        auto* dialog = qobject_cast<OptionsDialog*>( QApplication::activeModalWidget() );
        if ( dialog ) {
            auto* toggle = dialog->findChild<QCheckBox*>( "localControlEnabledCheckBox" );
            if ( toggle ) {
                toggle->setChecked( false );
                applied = QMetaObject::invokeMethod( dialog, "updateConfigFromDialog", Qt::DirectConnection );
                stoppedBeforeClose = !service.isRunning();
            }
            dialog->reject();
        }
    } );
    REQUIRE( QMetaObject::invokeMethod( &fixture.window, "options", Qt::DirectConnection ) );
    REQUIRE( applied );
    CHECK( notifications == 1 );
    CHECK( stoppedBeforeClose );
    CHECK( discoverInstances( runtime ).instances.empty() );
}

TEST_CASE( "The real console executable stages GUI evidence and observes human conflicts",
           "[ui][control-service][control-cli-gui]" )
{
    ReviewFixture fixture;
    RestoreHighlighters restore;
    RuntimeOptions runtime;
    runtime.descriptorRoot = QFileInfo( fixture.directory.path() ).canonicalFilePath() + "/control";
    ReviewControlService service{ [ & ] {
        return std::vector<QPointer<MainWindow>>{ &fixture.window };
    }, runtime };
    REQUIRE( service.setEnabled( true ) );
    const auto run = [ & ]( QStringList args, int expectedExit = 0 ) {
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert( "KLOGG_CONTROL_DESCRIPTOR_ROOT", runtime.descriptorRoot );
        child.setProcessEnvironment( environment );
        child.start( QString::fromUtf8( KLOGG_CONTROL_CLI_PATH ), args );
        REQUIRE( child.waitForStarted( 5000 ) );
        // The GUI thread must pump the production server while the separate
        // console process waits for its reply; waitForFinished would deadlock.
        const bool finished = waitUiState( [ & ] { return child.state() == QProcess::NotRunning; } );
        if ( !finished ) { child.kill(); child.waitForFinished( 5000 ); }
        REQUIRE( finished );
        INFO( child.readAllStandardError().toStdString() );
        REQUIRE( child.exitStatus() == QProcess::NormalExit );
        REQUIRE( child.exitCode() == expectedExit );
        const auto document = QJsonDocument::fromJson( child.readAllStandardOutput() );
        REQUIRE( document.isObject() );
        return document.object();
    };
    const auto instances = run( { "instance", "list" } ).value( "result" ).toObject()
                               .value( "instances" ).toArray();
    REQUIRE( instances.size() == 1 );
    CHECK( instances.first().toObject().value( "instance_id" ).toString() == service.instanceId() );
    CHECK_FALSE( instances.first().toObject().contains( "token" ) );
    const QStringList target{ "--instance", service.instanceId() };
    const auto call = [ & ]( const QString& method, const QJsonObject& params, int expectedExit = 0 ) {
        return run( target + QStringList{ "call", method, "--params",
            QString::fromUtf8( QJsonDocument( params ).toJson( QJsonDocument::Compact ) ) }, expectedExit );
    };
    const auto state = run( target + QStringList{ "state", "get" } ).value( "result" ).toObject();
    REQUIRE( state.value( "windows" ).toArray().size() == 1 );
    const auto transaction = run( target + QStringList{ "transaction", "begin" } )
                                 .value( "result" ).toObject().value( "transaction_id" ).toString();
    REQUIRE_FALSE( transaction.isEmpty() );
    for ( size_t index = 0; index < 2; ++index ) {
        call( "label.set", { { "transaction_id", transaction }, { "tab_id", fixture.tabId( index ) },
            { "label", 1 }, { "texts", QJsonArray{ "Agent evidence" } } } );
        CHECK( fixture.controls( index ).labels->snapshot().labels.front().isEmpty() );
    }
    const auto staged = run( target + QStringList{ "label", "list", "--tab", fixture.tabId(),
                                                   "--transaction", transaction } )
                            .value( "result" ).toObject();
    CHECK( staged.value( "slots" ).toArray().first().toObject().value( "entries" )
               .toArray().first().toObject().value( "text" ).toString() == QStringLiteral( "Agent evidence" ) );
    ColorLabelsController::Edit human;
    human.texts = QStringList{ QStringLiteral( "Human evidence" ) };
    fixture.controls( 1 ).labels->apply( { human } );
    const auto conflict = run( target + QStringList{ "transaction", "commit", "--transaction", transaction }, 4 );
    CHECK( conflict.value( "error" ).toObject().value( "data" ).toObject().value( "code" ).toString()
           == QStringLiteral( "REVISION_CONFLICT" ) );
    CHECK( fixture.controls().labels->snapshot().labels.front().isEmpty() );
    const auto observed = run( target + QStringList{ "label", "list", "--tab", fixture.tabId( 1 ) } )
                              .value( "result" ).toObject();
    CHECK( observed.value( "slots" ).toArray().first().toObject().value( "entries" )
               .toArray().first().toObject().value( "text" ).toString() == QStringLiteral( "Human evidence" ) );
    call( "label.set", { { "tab_id", fixture.tabId() }, { "label", 1 },
                         { "texts", QJsonArray{ "Committed evidence" } } } );
    CHECK( fixture.controls().labels->snapshot().labels.front().front().text
           == QStringLiteral( "Committed evidence" ) );
    const auto fileTab = fixture.openEvidenceFileTab();
    const auto pane = run( target + QStringList{ "pane", "get", "--tab", fileTab, "--pane", "main" } )
                          .value( "result" ).toObject();
    CHECK( pane.value( "row_count" ) == QLatin1String( "1" ) );
    const auto listedPanes = call( "pane.list", { { "tab_id", fileTab } } )
                                 .value( "result" ).toObject().value( "panes" ).toArray();
    REQUIRE( listedPanes.size() == 2 );
    const auto mainPaneId = pane.value( "pane_id" ).toString();
    REQUIRE_FALSE( mainPaneId.isEmpty() );
    CHECK( listedPanes.first().toObject().value( "pane_id" ) == mainPaneId );
    const auto captured = call( "source.capture", { { "tab_id", fileTab }, { "pane_id", mainPaneId },
                                                   { "scope", "presented" } } ).value( "result" ).toObject();
    const auto snapshot = captured.value( "snapshot_id" ).toString();
    REQUIRE_FALSE( snapshot.isEmpty() );
    const auto markOwner = listedPanes.first().toObject().value( "marks_owner_id" );
    const QJsonObject markRead{ { "tab_id", fileTab }, { "owner_id", markOwner } };
    const auto marksBefore = call( "mark.list", markRead ).value( "result" ).toObject();
    QJsonObject markChange{ { "tab_id", fileTab }, { "owner_id", markOwner }, { "snapshot_id", snapshot },
        { "rows", QJsonArray{ "0" } }, { "expected_revision", marksBefore.value( "revision" ) } };
    const auto markStart = call( "mark.add", markChange ).value( "result" ).toObject();
    REQUIRE( markStart.value( "status" ) == QLatin1String( "preparing" ) );
    QJsonObject markDone;
    REQUIRE( waitUiState( [&] {
        markDone = call( "transaction.get", { { "transaction_id", markStart.value( "transaction_id" ) } } )
            .value( "result" ).toObject();
        return markDone.value( "status" ) != QLatin1String( "preparing" );
    } ) );
    CHECK( markDone.value( "status" ) == QLatin1String( "committed" ) );
    const auto marked = call( "mark.list", markRead ).value( "result" ).toObject().value( "groups" ).toArray();
    REQUIRE( marked.size() == 1 );
    CHECK( marked.first().toObject().value( "rows" ) == QJsonArray{ "0" } );
    CHECK( call( "mark.add", markChange, 4 ).value( "error" ).toObject().value( "data" ).toObject()
               .value( "code" ) == QLatin1String( "REVISION_CONFLICT" ) );
    const auto reading = call( "source.read", { { "snapshot_id", snapshot }, { "start", "0" }, { "count", 1 } } )
                             .value( "result" ).toObject().value( "operation_id" ).toString();
    REQUIRE_FALSE( reading.isEmpty() );
    call( "source.release", { { "snapshot_id", snapshot } } );
    QJsonObject content;
    REQUIRE( waitUiState( [&] {
        content = call( "operation.get", { { "operation_id", reading } } ).value( "result" ).toObject();
        return content.value( "status" ) != QLatin1String( "pending" );
    } ) );
    CHECK( content.value( "status" ) == QLatin1String( "completed" ) );
    const auto rows = content.value( "result" ).toObject().value( "rows" ).toArray();
    REQUIRE( rows.size() == 1 );
    CHECK( rows.first().toObject().value( "text" ) == QLatin1String( "Evidence" ) );
    const auto globalBefore = call( "highlighter.list", {} ).value( "result" ).toObject();
    auto globalChange = replacement();
    globalChange.insert( "expected_revision", globalBefore.value( "revision" ) );
    const auto globalStart = call( "highlighter.replace", globalChange ).value( "result" ).toObject();
    REQUIRE( globalStart.value( "status" ) == QLatin1String( "preparing" ) );
    const auto globalId = globalStart.value( "transaction_id" ).toString();
    REQUIRE_FALSE( globalId.isEmpty() );
    QJsonObject globalDone;
    REQUIRE( waitUiState( [&] {
        globalDone = call( "transaction.get", { { "transaction_id", globalId } } ).value( "result" ).toObject();
        return globalDone.value( "status" ) != QLatin1String( "preparing" );
    } ) );
    CHECK( globalDone.value( "status" ) == QLatin1String( "committed" ) );
    const auto globalAfter = call( "highlighter.list", {} ).value( "result" ).toObject();
    CHECK( globalAfter.value( "active_set_ids" ) == QJsonArray{ "rpc-evidence" } );
    CHECK( globalAfter.value( "label_palette" ) == globalBefore.value( "label_palette" ) );
    CHECK( call( "highlighter.replace", globalChange, 4 ).value( "error" ).toObject()
               .value( "data" ).toObject().value( "code" ) == QLatin1String( "REVISION_CONFLICT" ) );
    const auto currentBeforeOpen = fixture.window.inventorySnapshot().currentTabId;
    const auto lifecyclePath = fixture.directory.filePath( "cli-lifecycle.log" );
    QFile lifecycleFile{ lifecyclePath };
    REQUIRE( lifecycleFile.open( QIODevice::WriteOnly ) );
    REQUIRE( lifecycleFile.write( "CLI lifecycle\n" ) == 14 );
    lifecycleFile.close();
    const auto opening = call( "documents.open", {
        { "window_id", fixture.window.runtimeWindowId() },
        { "path", lifecyclePath }, { "kind", "file" } } )
        .value( "result" ).toObject();
    const auto openingId = opening.value( "operation_id" ).toString();
    REQUIRE_FALSE( openingId.isEmpty() );
    QJsonObject opened;
    REQUIRE( waitUiState( [&] {
        opened = call( "operation.get", { { "operation_id", openingId } } ).value( "result" ).toObject();
        return opened.value( "status" ) != QLatin1String( "pending" );
    } ) );
    REQUIRE( opened.value( "status" ) == QLatin1String( "completed" ) );
    CHECK( fixture.window.inventorySnapshot().currentTabId == currentBeforeOpen );
    const auto openedTab = opened.value( "result" ).toObject().value( "tab_id" );
    const auto filterDraft = call( "tab.get", { { "tab_id", openedTab } } )
        .value( "result" ).toObject().value( "filter_draft" ).toObject();
    const auto filtering = call( "filter.apply", { { "tab_id", openedTab },
        { "expected_revision", filterDraft.value( "revision" ) },
        { "filter", QJsonObject{ { "text", "CLI" }, { "use_regexp", false },
            { "inverse", false }, { "boolean", false }, { "auto_refresh", false },
            { "keep_results", false } } } } ).value( "result" ).toObject();
    QJsonObject filtered;
    REQUIRE( waitUiState( [&] {
        filtered = call( "operation.get", { { "operation_id", filtering.value( "operation_id" ) } } )
            .value( "result" ).toObject();
        return filtered.value( "status" ) != QLatin1String( "pending" );
    } ) );
    REQUIRE( filtered.value( "status" ) == QLatin1String( "completed" ) );
    CHECK( call( "tab.get", { { "tab_id", openedTab } } ).value( "result" ).toObject()
        .value( "filter_draft" ).toObject().value( "text" ) == QLatin1String( "CLI" ) );
    const auto filteredSnapshot = call( "source.capture", { { "tab_id", openedTab },
        { "pane_id", filtered.value( "result" ).toObject().value( "pane_id" ) },
        { "scope", "matches" } } ).value( "result" ).toObject();
    CHECK( filteredSnapshot.value( "row_count" ) == QLatin1String( "1" ) );
    call( "source.release", { { "snapshot_id", filteredSnapshot.value( "snapshot_id" ) } } );
    const auto keeping = call( "filter.apply", { { "tab_id", openedTab },
        { "filter", QJsonObject{ { "text", "lifecycle" }, { "keep_results", true } } } } )
        .value( "result" ).toObject();
    QJsonObject kept;
    REQUIRE( waitUiState( [&] {
        kept = call( "operation.get", { { "operation_id", keeping.value( "operation_id" ) } } )
            .value( "result" ).toObject();
        return kept.value( "status" ) != QLatin1String( "pending" );
    } ) );
    REQUIRE( kept.value( "status" ) == QLatin1String( "completed" ) );
    const auto oldResultPane = filtered.value( "result" ).toObject().value( "pane_id" );
    CHECK( kept.value( "result" ).toObject().value( "pane_id" ) != oldResultPane );
    CHECK( call( "pane.get", { { "tab_id", openedTab }, { "pane_id", oldResultPane } } )
        .value( "result" ).toObject().value( "applied_filter" ).toObject().value( "text" )
        == QLatin1String( "CLI" ) );
    CHECK( call( "pane.list", { { "tab_id", openedTab } } ).value( "result" ).toObject()
        .value( "panes" ).toArray().size() == 3 );
    CHECK( fixture.window.inventorySnapshot().currentTabId == currentBeforeOpen );
    const auto closed = call( "documents.close", {
        { "window_id", fixture.window.runtimeWindowId() }, { "tab_id", openedTab } } )
        .value( "result" ).toObject();
    CHECK( closed.value( "status" ) == QLatin1String( "completed" ) );
    CHECK( fixture.window.inventorySnapshot().currentTabId == currentBeforeOpen );
    // Exercise the same client executable and private transport against a
    // background folder tab, including fresh enumeration of cli-lifecycle.log.
    const auto folderId = fixture.tabId();
    const auto folderApply = [&]( const QString& text, bool keep ) {
        const auto started = call( "filter.apply", { { "tab_id", folderId }, { "filter", QJsonObject{
            { "text", text }, { "use_regexp", false }, { "inverse", false }, { "boolean", false },
            { "keep_results", keep } } } } ).value( "result" ).toObject();
        QJsonObject operation;
        REQUIRE( waitUiState( [&] {
            operation = call( "operation.get", { { "operation_id", started.value( "operation_id" ) } } )
                            .value( "result" ).toObject();
            return operation.value( "status" ) != "pending";
        } ) );
        REQUIRE( operation.value( "status" ) == "completed" );
        CHECK( fixture.window.inventorySnapshot().currentTabId == currentBeforeOpen );
        return operation.value( "result" ).toObject().value( "pane_id" ).toString();
    };
    const auto folderPane = folderApply( "CLI lifecycle", false );
    REQUIRE_FALSE( folderPane.isEmpty() );
    const auto folderSnapshot = call( "source.capture", { { "tab_id", folderId }, { "pane_id", folderPane },
        { "scope", "matches" } } ).value( "result" ).toObject();
    REQUIRE( folderSnapshot.value( "row_count" ) == "1" );
    call( "source.release", { { "snapshot_id", folderSnapshot.value( "snapshot_id" ) } } );
    const auto keptFolderPane = folderApply( "Evidence", true );
    REQUIRE( keptFolderPane != folderPane );
    CHECK( folderApply( QString{}, true ) == keptFolderPane );
    CHECK( call( "pane.get", { { "tab_id", folderId }, { "pane_id", folderPane } } )
               .value( "result" ).toObject().value( "applied_filter" ).toObject().value( "text" ) == "CLI lifecycle" );
    REQUIRE( service.setEnabled( false ) );
    const auto unavailable = run( target + QStringList{ "state", "get" }, 3 );
    CHECK( unavailable.contains( "error" ) );
}
