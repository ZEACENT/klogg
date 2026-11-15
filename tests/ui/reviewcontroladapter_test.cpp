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

#include "abstractcrawlerwidget.h"
#include "colorlabelscontroller.h"
#include "mainwindow.h"
#include "optionsdialog.h"
#include "styles.h"
#include "reviewcontroladapter.h"
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
} // namespace

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
    edit.texts = { QStringLiteral( "Evidence" ) };
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
    const auto captured = call( "source.capture", { { "tab_id", fileTab }, { "pane", "main" },
                                                   { "scope", "presented" } } ).value( "result" ).toObject();
    const auto snapshot = captured.value( "snapshot_id" ).toString();
    REQUIRE_FALSE( snapshot.isEmpty() );
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
    REQUIRE( service.setEnabled( false ) );
    const auto unavailable = run( target + QStringList{ "state", "get" }, 3 );
    CHECK( unavailable.contains( "error" ) );
}
