#include <catch2/catch.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QThreadPool>

#include "abstractcrawlerwidget.h"
#include "mainwindow.h"
#include "reviewcontroladapter.h"
#include "searchtoolbar.h"
#include "session.h"
#include "test_utils.h"

namespace {
using namespace klogg::control;
QJsonObject filterResult( const DecodedResponse& response )
{
    REQUIRE( std::holds_alternative<QJsonValue>( response ) );
    REQUIRE( std::get<QJsonValue>( response ).isObject() );
    return std::get<QJsonValue>( response ).toObject();
}
QString filterError( const DecodedResponse& response )
{
    REQUIRE( std::holds_alternative<RpcError>( response ) );
    return std::get<RpcError>( response ).data.toObject().value( "code" ).toString();
}
struct FilterControlFixture {
    QTemporaryDir root;
    std::shared_ptr<Session> session = std::make_shared<Session>();
    MainWindow window{ WindowSession{ session, "FilterControl", 0 } };
    ReviewControlAdapter adapter{ [this] { return std::vector<QPointer<MainWindow>>{ &window }; } };
    QString tabId;
    FilterControlFixture()
    {
        REQUIRE( root.isValid() );
        const auto path = root.filePath( "source.log" );
        QFile file{ path };
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        const QByteArray text{ "INFO ordinary\nERROR PRIVATE BODY\n" };
        REQUIRE( file.write( text ) == text.size() );
        file.close();
        const auto opened = await( call( "documents.open", {
            { "window_id", window.runtimeWindowId() }, { "path", path } } ) );
        REQUIRE( opened.value( "status" ) == "completed" );
        tabId = opened.value( "result" ).toObject().value( "tab_id" ).toString();
        REQUIRE_FALSE( tabId.isEmpty() );
    }
    DecodedResponse raw( const QString& method, const QJsonObject& params = {} )
    {
        return adapter.dispatch( { "filter-test", method, params } );
    }
    QJsonObject call( const QString& method, const QJsonObject& params = {} )
    {
        return filterResult( raw( method, params ) );
    }
    QString openFolder( const QString& path )
    {
        const auto opened = await( call( "documents.open", { { "window_id", window.runtimeWindowId() },
            { "path", path }, { "kind", "folder" } } ) );
        REQUIRE( opened.value( "status" ) == "completed" );
        return opened.value( "result" ).toObject().value( "tab_id" ).toString();
    }
    QJsonObject params( const QString& text ) const
    {
        return { { "tab_id", tabId }, { "filter", QJsonObject{
            { "text", text }, { "match_case", true }, { "use_regexp", true },
            { "inverse", false }, { "boolean", false }, { "auto_refresh", false },
            { "keep_results", false } } } };
    }
    QJsonObject draft()
    {
        return call( "tab.get", { { "tab_id", tabId } } ).value( "filter_draft" ).toObject();
    }
    QJsonObject await( const QJsonObject& started )
    {
        QJsonObject result;
        REQUIRE( waitUiState( [&] {
            result = call( "operation.get", { { "operation_id", started.value( "operation_id" ) } } );
            return result.value( "status" ) != "pending";
        } ) );
        return result;
    }
    SearchToolbar* toolbar()
    {
        for ( const auto& tab : window.inventorySnapshot().tabs ) {
            if ( tab.tabId == tabId ) {
                auto* crawler = dynamic_cast<AbstractCrawlerWidget*>( tab.widget.data() );
                REQUIRE( crawler );
                return crawler->reviewControls().filter;
            }
        }
        FAIL( "Fixture tab disappeared" );
        return nullptr;
    }
};
}

TEST_CASE( "Control applies a filter privately and reads its matches only explicitly",
           "[control-filters]" )
{
    FilterControlFixture fixture;
    QWidget editorWindow;
    QLineEdit editor{ &editorWindow };
    editorWindow.show();
    editor.setFocus();
    REQUIRE( waitUiState( [&] { return QApplication::focusWidget() == &editor; } ) );
    const auto before = fixture.draft();
    const auto pending = fixture.call( "filter.apply", fixture.params( "ERROR" ) );
    CHECK( pending.value( "status" ) == "pending" );
    CHECK( fixture.draft() == before );
    const auto completed = fixture.await( pending );
    REQUIRE( completed.value( "status" ) == "completed" );
    CHECK( completed.value( "result" ).toObject().value( "tab_id" ) == fixture.tabId );
    CHECK( fixture.draft().value( "text" ) == "ERROR" );
    CHECK_FALSE( QJsonDocument{ completed }.toJson().contains( "PRIVATE BODY" ) );
    CHECK( QApplication::focusWidget() == &editor );
    const auto captured = fixture.call( "source.capture", {
        { "tab_id", fixture.tabId }, { "pane", "results" }, { "scope", "matches" } } );
    CHECK( captured.value( "row_count" ) == "1" );
    CHECK( completed.value( "result" ).toObject().value( "pane_id" )
           == fixture.call( "pane.get", { { "tab_id", fixture.tabId }, { "pane", "results" } } )
                  .value( "pane_id" ) );
    const auto read = fixture.await( fixture.call( "source.read", {
        { "snapshot_id", captured.value( "snapshot_id" ) }, { "start", "0" }, { "count", 1 } } ) );
    REQUIRE( read.value( "status" ) == "completed" );
    const auto rows = read.value( "result" ).toObject().value( "rows" ).toArray();
    REQUIRE( rows.size() == 1 );
    CHECK( rows.first().toObject().value( "text" ) == "ERROR PRIVATE BODY" );
    const auto cleared = fixture.await( fixture.call( "filter.apply", fixture.params( "" ) ) );
    CHECK( cleared.value( "status" ) == "completed" );
    CHECK( fixture.draft().value( "text" ).toString().isEmpty() );
    CHECK( QApplication::focusWidget() == &editor );
}

TEST_CASE( "Control publishes a new kept pane while preserving the previous query and focus",
           "[control-kept-filters]" )
{
    const bool clear = GENERATE( false, true );
    FilterControlFixture fixture;
    fixture.window.show();
    QWidget editorWindow;
    QLineEdit editor{ &editorWindow };
    editorWindow.show();
    editor.setFocus();
    REQUIRE( waitUiState( [&] { return QApplication::focusWidget() == &editor; } ) );
    const auto original = fixture.await( fixture.call( "filter.apply", fixture.params( "ERROR" ) ) );
    REQUIRE( original.value( "status" ) == "completed" );
    const auto oldPaneId = original.value( "result" ).toObject().value( "pane_id" );
    const auto before = fixture.draft();
    auto params = fixture.params( clear ? QString{} : QStringLiteral( "INFO" ) );
    auto filter = params.value( "filter" ).toObject();
    filter.insert( "keep_results", true );
    params.insert( "filter", filter );
    params.insert( "expected_revision", before.value( "revision" ) );
    const auto pending = fixture.call( "filter.apply", params );
    CHECK( fixture.draft() == before );
    CHECK( fixture.call( "pane.list", { { "tab_id", fixture.tabId } } ).value( "panes" ).toArray().size() == 2 );
    const auto completed = fixture.await( pending );
    REQUIRE( completed.value( "status" ) == "completed" );
    const auto newPaneId = completed.value( "result" ).toObject().value( "pane_id" );
    CHECK( newPaneId != oldPaneId );
    CHECK( fixture.call( "pane.list", { { "tab_id", fixture.tabId } } ).value( "panes" ).toArray().size() == 3 );
    const auto oldPane = fixture.call( "pane.get", { { "tab_id", fixture.tabId }, { "pane_id", oldPaneId } } );
    CHECK( oldPane.value( "applied_filter" ).toObject().value( "text" ) == "ERROR" );
    CHECK( oldPane.value( "applied_filter" ).toObject().value( "complete" ).toBool() );
    CHECK_FALSE( oldPane.value( "current" ).toBool( true ) );
    const auto newPane = fixture.call( "pane.get", { { "tab_id", fixture.tabId }, { "pane_id", newPaneId } } );
    CHECK( newPane.value( "current" ).toBool() );
    CHECK( newPane.value( "applied_filter" ).toObject().value( "text" ) == filter.value( "text" ) );
    CHECK( newPane.value( "applied_filter" ).toObject().value( "complete" ).toBool() == !clear );
    CHECK_FALSE( fixture.draft().value( "keep_results" ).toBool( true ) );
    CHECK( QApplication::focusWidget() == &editor );
    CHECK_FALSE( QJsonDocument{ completed }.toJson().contains( "PRIVATE BODY" ) );
    const auto retained = fixture.call( "source.capture", { { "tab_id", fixture.tabId },
        { "pane_id", oldPaneId }, { "scope", "matches" } } );
    CHECK( retained.value( "row_count" ) == "1" );
}

TEST_CASE( "Pane observations distinguish applied query values from unsubmitted filter drafts",
           "[control-filters][control-applied-filter]" )
{
    FilterControlFixture fixture;
    const auto completed = fixture.await( fixture.call( "filter.apply", fixture.params( "ERROR" ) ) );
    REQUIRE( completed.value( "status" ) == "completed" );
    const auto paneId = completed.value( "result" ).toObject().value( "pane_id" );
    const auto observe = [&] {
        return fixture.call( "pane.get", { { "tab_id", fixture.tabId }, { "pane_id", paneId } } );
    };
    const auto pane = observe();
    REQUIRE( pane.value( "applied_filter" ).isObject() );
    const QJsonObject expected{ { "text", "ERROR" }, { "match_case", true },
        { "use_regexp", true }, { "inverse", false }, { "boolean", false }, { "complete", true } };
    CHECK( pane.value( "applied_filter" ).toObject() == expected );
    CHECK_FALSE( QJsonDocument{ pane }.toJson().contains( "PRIVATE BODY" ) );
    fixture.toolbar()->searchLineEdit()->setEditText( QStringLiteral( "unsubmitted" ) );
    CHECK( fixture.draft().value( "text" ) == "unsubmitted" );
    CHECK( observe().value( "applied_filter" ).toObject() == expected );
    CHECK( fixture.call( "pane.get", { { "tab_id", fixture.tabId }, { "pane", "main" } } )
               .value( "applied_filter" ).isUndefined() );
    const auto listed = fixture.call( "pane.list", { { "tab_id", fixture.tabId } } )
        .value( "panes" ).toArray();
    REQUIRE( listed.size() == 2 );
    CHECK( listed.last().toObject().value( "applied_filter" ).toObject() == expected );
    REQUIRE( fixture.await( fixture.call( "filter.apply", fixture.params( "" ) ) )
                 .value( "status" ) == "completed" );
    const auto reset = observe().value( "applied_filter" ).toObject();
    CHECK( reset.value( "text" ).toString().isEmpty() );
    CHECK_FALSE( reset.value( "complete" ).toBool( true ) );
}

TEST_CASE( "Control filter preparation cannot overwrite human edits or publish cancelled work",
           "[control-filters]" )
{
    FilterControlFixture fixture;
    const auto pending = fixture.call( "filter.apply", fixture.params( "ERROR" ) );
    SECTION( "human edit" ) {
        fixture.toolbar()->searchLineEdit()->setEditText( QStringLiteral( "human draft" ) );
        const auto result = fixture.await( pending );
        CHECK( result.value( "status" ) == "failed" );
        CHECK( result.value( "error" ).toObject().value( "code" ) == "CONFLICT" );
        CHECK( fixture.draft().value( "text" ) == "human draft" );
    }
    SECTION( "cancel" ) {
        fixture.call( "operation.cancel", { { "operation_id", pending.value( "operation_id" ) } } );
        CHECK( fixture.await( pending ).value( "status" ) == "cancelled" );
        CHECK( fixture.draft().value( "text" ).toString().isEmpty() );
    }
    SECTION( "close target" ) {
        fixture.call( "documents.close", {
            { "window_id", fixture.window.runtimeWindowId() }, { "tab_id", fixture.tabId } } );
        const auto result = fixture.await( pending );
        CHECK( result.value( "status" ) == "failed" );
        CHECK( result.value( "error" ).toObject().value( "code" ) == "TARGET_CLOSED" );
        CHECK( fixture.window.inventorySnapshot().tabs.empty() );
    }
    SECTION( "disable/reset" ) {
        fixture.adapter.reset();
        REQUIRE( QThreadPool::globalInstance()->waitForDone( 10000 ) );
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
        CHECK( fixture.draft().value( "text" ).toString().isEmpty() );
    }
}

TEST_CASE( "Filter preparation bounds pending deliveries without publishing rejected work",
           "[control-filters]" )
{
    FilterControlFixture fixture;
    const auto before = fixture.draft();
    auto folder = fixture.params( "INFO" );
    folder.insert( "tab_id", fixture.openFolder( fixture.root.path() ) );
    // File and folder share permits. No event turn: even a finished worker
    // retains its permit until delivery retires.
    const auto first = fixture.call( "filter.apply", fixture.params( "ERROR" ) );
    const auto second = fixture.call( "filter.apply", folder );
    CHECK( filterError( fixture.raw( "filter.apply", fixture.params( "third" ) ) )
           == "RESOURCE_LIMIT" );
    for ( const auto& pending : { first, second } ) {
        fixture.call( "operation.cancel", { { "operation_id", pending.value( "operation_id" ) } } );
    }
    REQUIRE( QThreadPool::globalInstance()->waitForDone( 10000 ) );
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    CHECK( fixture.await( first ).value( "status" ) == "cancelled" );
    CHECK( fixture.await( second ).value( "status" ) == "cancelled" );
    CHECK( fixture.draft() == before );
}

TEST_CASE( "Expired filter work cannot publish even without operation polling",
           "[control-filters]" )
{
    FilterControlFixture fixture;
    qint64 clock = 0;
    ReviewControlAdapter adapter{ [&] { return std::vector<QPointer<MainWindow>>{ &fixture.window }; },
                                  [&] { return clock; } };
    const auto before = fixture.draft();
    const auto pending = filterResult( adapter.dispatch( {
        "expiry", "filter.apply", fixture.params( "ERROR" ) } ) );
    clock = 5 * 60000 + 1;
    REQUIRE( QThreadPool::globalInstance()->waitForDone( 10000 ) );
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    CHECK( fixture.draft() == before );
    CHECK( filterError( adapter.dispatch( { "expiry", "operation.get", {
        { "operation_id", pending.value( "operation_id" ) } } } ) ) == "OPERATION_NOT_FOUND" );
}

TEST_CASE( "Unsupported filter transaction aborts its staged annotation prefix",
           "[control-filters]" )
{
    FilterControlFixture fixture;
    const auto before = fixture.call( "label.list", { { "tab_id", fixture.tabId } } );
    const auto transaction = fixture.call( "transaction.begin" ).value( "transaction_id" );
    fixture.call( "label.set", { { "transaction_id", transaction }, { "tab_id", fixture.tabId },
        { "label", 1 }, { "texts", QJsonArray{ "unpublished" } } } );
    auto params = fixture.params( "ERROR" );
    params.insert( "transaction_id", transaction );
    CHECK( filterError( fixture.raw( "filter.apply", params ) ) == "INVALID_PARAMS" );
    CHECK( fixture.call( "transaction.get", { { "transaction_id", transaction } } )
               .value( "status" ) == "invalid_argument" );
    CHECK( filterError( fixture.raw( "transaction.commit", { { "transaction_id", transaction } } ) )
           == "INVALID_PARAMS" );
    CHECK( fixture.call( "label.list", { { "tab_id", fixture.tabId } } ) == before );
    CHECK( fixture.draft().value( "text" ).toString().isEmpty() );
}

TEST_CASE( "Folder filter distinguishes an empty directory from a vanished root",
           "[control-filters][control-folder-root]" )
{
    FilterControlFixture fixture;
    QDir root{ fixture.root.path() };
    REQUIRE( root.mkdir( "logs" ) );
    const auto path = root.filePath( "logs" );
    QFile member{ QDir{ path }.filePath( "previous.log" ) };
    REQUIRE( member.open( QIODevice::WriteOnly ) );
    REQUIRE( member.write( "before\n" ) == 7 );
    member.close();
    fixture.tabId = fixture.openFolder( path );
    REQUIRE( fixture.await( fixture.call( "filter.apply", fixture.params( "before" ) ) ).value( "status" ) == "completed" );
    const auto previous = fixture.draft();
    bool missing = false;
    SECTION( "last member removed" ) { REQUIRE( member.remove() ); }
    SECTION( "directory moved" ) { REQUIRE( root.rename( "logs", "moved" ) ); missing = true; }
    const auto outcome = fixture.await( fixture.call( "filter.apply", fixture.params( "after" ) ) );
    CHECK( outcome.value( "status" ) == ( missing ? "failed" : "completed" ) );
    if ( missing ) CHECK( fixture.draft() == previous );
    const auto snapshot = fixture.call( "source.capture", { { "tab_id", fixture.tabId },
        { "pane", "results" }, { "scope", "matches" } } );
    CHECK( snapshot.value( "row_count" ) == ( missing ? "1" : "0" ) );
}

TEST_CASE( "Folder filter RPC publishes only the completed query and preserves other tabs",
           "[control-filters][control-folder-filter]" )
{
    FilterControlFixture fixture;
    fixture.tabId = fixture.openFolder( fixture.root.path() );
    QWidget editorWindow;
    QLineEdit editor{ &editorWindow };
    editorWindow.show();
    editor.setFocus();
    REQUIRE( waitUiState( [&] { return QApplication::focusWidget() == &editor; } ) );
    const auto before = fixture.draft();
    const auto current = fixture.window.inventorySnapshot().currentTabId;
    const auto pending = fixture.call( "filter.apply", fixture.params( "ERROR" ) );
    CHECK( fixture.draft() == before );
    SECTION( "cancel before delivery" ) {
        fixture.call( "operation.cancel", { { "operation_id", pending.value( "operation_id" ) } } );
        CHECK( fixture.draft() == before );
        CHECK( fixture.await( pending ).value( "status" ) != "completed" );
    }
    SECTION( "human edit before delivery" ) {
        fixture.toolbar()->searchLineEdit()->setEditText( "human edit" );
        CHECK( fixture.await( pending ).value( "error" ).toObject().value( "code" ) == "CONFLICT" );
        CHECK( fixture.draft().value( "text" ) == "human edit" );
    }
    SECTION( "closed folder cannot publish" ) {
        fixture.call( "documents.close", { { "window_id", fixture.window.runtimeWindowId() }, { "tab_id", fixture.tabId } } );
        CHECK( fixture.await( pending ).value( "error" ).toObject().value( "code" ) == "TARGET_CLOSED" );
    }
    SECTION( "replace, keep and empty reset" ) {
        const auto done = fixture.await( pending );
        REQUIRE( done.value( "status" ) == "completed" );
        CHECK_FALSE( QJsonDocument{ done }.toJson().contains( "PRIVATE BODY" ) );
        const auto original = done.value( "result" ).toObject().value( "pane_id" );
        const auto captured = fixture.call( "source.capture", { { "tab_id", fixture.tabId },
            { "pane_id", original }, { "scope", "matches" } } );
        CHECK( captured.value( "row_count" ) == "1" );
        fixture.call( "source.release", { { "snapshot_id", captured.value( "snapshot_id" ) } } );
        const auto beforeInvalid = fixture.draft();
        const auto invalid = fixture.await( fixture.call( "filter.apply", fixture.params( "[" ) ) );
        CHECK( invalid.value( "error" ).toObject().value( "code" ) == "FILTER_COMPILE_FAILED" );
        CHECK( fixture.draft() == beforeInvalid );
        const auto applyKeep = [&]( const QString& text ) {
            return fixture.await( fixture.call( "filter.apply", { { "tab_id", fixture.tabId },
                { "filter", QJsonObject{ { "text", text }, { "keep_results", true } } } } ) );
        };
        const auto kept = applyKeep( "INFO" );
        REQUIRE( kept.value( "status" ) == "completed" );
        const auto fresh = kept.value( "result" ).toObject().value( "pane_id" );
        CHECK( fresh != original );
        CHECK_FALSE( fixture.draft().value( "keep_results" ).toBool() );
        CHECK( fixture.call( "pane.get", { { "tab_id", fixture.tabId }, { "pane_id", original } } )
                   .value( "applied_filter" ).toObject().value( "text" ) == "ERROR" );
        const auto cleared = applyKeep( "" );
        REQUIRE( cleared.value( "status" ) == "completed" );
        CHECK( cleared.value( "result" ).toObject().value( "pane_id" ) == fresh );
        CHECK( fixture.draft().value( "keep_results" ).toBool() );
        CHECK( fixture.window.inventorySnapshot().currentTabId == current );
    }
    CHECK( QApplication::focusWidget() == &editor );
}

TEST_CASE( "Control filter compile failure preserves the previous UI state",
           "[control-filters]" )
{
    FilterControlFixture fixture;
    const auto before = fixture.draft();
    const auto result = fixture.await( fixture.call( "filter.apply", fixture.params( "[" ) ) );
    CHECK( result.value( "status" ) == "failed" );
    CHECK( result.value( "error" ).toObject().value( "code" ) == "FILTER_COMPILE_FAILED" );
    CHECK( fixture.draft() == before );
}

TEST_CASE( "Control filter writes strictly validate revisions and options",
           "[control-filters]" )
{
    FilterControlFixture fixture;
    auto params = fixture.params( "ERROR" );
    QString expected{ "INVALID_PARAMS" };
    SECTION( "unknown option" ) {
        auto filter = params.value( "filter" ).toObject();
        filter.insert( "unknown", true );
        params.insert( "filter", filter );
    }
    SECTION( "wrong option type" ) {
        auto filter = params.value( "filter" ).toObject();
        filter.insert( "match_case", "yes" );
        params.insert( "filter", filter );
    }
    SECTION( "missing filter object" ) { params.remove( "filter" ); }
    SECTION( "embedded NUL" ) {
        auto filter = params.value( "filter" ).toObject();
        filter.insert( "text", QStringLiteral( "ERROR" ) + QChar{} + QStringLiteral( "ignored" ) );
        params.insert( "filter", filter );
    }
    SECTION( "pattern exceeds editor capacity" ) {
        auto filter = params.value( "filter" ).toObject();
        filter.insert( "text", QString( 32768, QChar{ 'x' } ) );
        params.insert( "filter", filter );
        expected = "RESOURCE_LIMIT";
    }
    SECTION( "noncanonical revision" ) { params.insert( "expected_revision", "00" ); }
    SECTION( "stale revision" ) {
        params.insert( "expected_revision", "18446744073709551615" );
        expected = "REVISION_CONFLICT";
    }
    SECTION( "explicit pane selector" ) { params.insert( "pane_id", "not-a-target-selector" ); }
    CHECK( filterError( fixture.raw( "filter.apply", params ) ) == expected );
    CHECK( fixture.draft().value( "text" ).toString().isEmpty() );
}
