#include <catch2/catch.hpp>

#include <QApplication>
#include <QFile>
#include <QEvent>
#include <QJsonObject>
#include <QJsonArray>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QThreadPool>

#include "mainwindow.h"
#include "reviewcontroladapter.h"
#include "session.h"
#include "tabgroup.h"
#include "test_utils.h"

namespace {
QJsonObject documentResult( const klogg::control::DecodedResponse& response )
{
    REQUIRE( std::holds_alternative<QJsonValue>( response ) );
    REQUIRE( std::get<QJsonValue>( response ).isObject() );
    return std::get<QJsonValue>( response ).toObject();
}
struct DocumentControlFixture {
    QTemporaryDir root;
    std::shared_ptr<Session> session = std::make_shared<Session>();
    MainWindow window{ WindowSession{ session, "DocumentControl", 0 } };
    ReviewControlAdapter adapter{ [this] { return std::vector<QPointer<MainWindow>>{ &window }; } };
    QString path;
    DocumentControlFixture()
    {
        REQUIRE( root.isValid() );
        path = root.filePath( "source.log" );
        QFile file{ path };
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        REQUIRE( file.write( "explicit content only\n" ) == 22 );
    }
    QJsonObject call( const QString& method, const QJsonObject& params )
    {
        return documentResult( adapter.dispatch( { "document-test", method, params } ) );
    }
    QJsonObject open( const QString& source, const QString& kind = "file" )
    {
        return call( "documents.open", { { "window_id", window.runtimeWindowId() },
                                        { "path", source }, { "kind", kind } } );
    }
    QJsonObject await( const QJsonObject& started )
    {
        QJsonObject outcome;
        REQUIRE( waitUiState( [&] {
            outcome = call( "operation.get", { { "operation_id", started.value( "operation_id" ) } } );
            return outcome.value( "status" ) != "pending";
        } ) );
        return outcome;
    }
};
}

TEST_CASE( "Control capability discovery reports only delivered domains without inspecting content",
           "[control-capabilities]" )
{
    DocumentControlFixture fixture;
    const auto before = fixture.window.inventorySnapshot();
    const auto capabilities = fixture.call( "capabilities.get", {} );
    CHECK( capabilities.value( "protocol_version" ).toInt() == 1 );
    const auto methods = capabilities.value( "methods" ).toArray();
    CHECK( methods.contains( "documents.open" ) );
    CHECK( methods.contains( "documents.close" ) );
    CHECK( methods.contains( "source.read" ) );
    CHECK( methods.contains( "filter.apply" ) );
    REQUIRE( methods.contains( "schema.get" ) );
    REQUIRE( methods.contains( "pane.navigate" ) );
    for ( const auto& method : methods ) {
        const auto schema = fixture.call( "schema.get", { { "method", method } } );
        CHECK( schema.value( "method" ) == method );
        CHECK( schema.value( "params" ).isObject() );
        CHECK( schema.value( "result" ).isObject() );
        const auto response = fixture.adapter.dispatch( { "advertised", method.toString(), {} } );
        if ( const auto* error = std::get_if<klogg::control::RpcError>( &response ) ) {
            CHECK( error->data.toObject().value( "code" ) != "METHOD_NOT_FOUND" );
        } else if ( method == "transaction.begin" ) {
            const auto id = std::get<QJsonValue>( response ).toObject().value( "transaction_id" );
            fixture.call( "transaction.abort", { { "transaction_id", id } } );
        }
    }
    const auto catalog = fixture.call( "schema.get", { { "method", "schema.get" } } )
        .value( "params" ).toObject().value( "properties" ).toObject().value( "method" ).toObject().value( "enum" ).toArray();
    REQUIRE( catalog.size() == methods.size() );
    for ( const auto& name : methods ) CHECK( catalog.contains( name ) );
    const auto features = capabilities.value( "features" ).toObject();
    CHECK( features.value( "explicit_lifecycle_transactions" ).toBool() );
    CHECK_FALSE( features.value( "live_lifecycle" ).toBool( true ) );
    CHECK( features.value( "implicit_filter_kinds" ).toArray() == QJsonArray{ "file", "live", "folder" } );
    CHECK( features.value( "folder_filter_source_policy" ) == "retained-scan" );
    CHECK( features.value( "filter_source_policy" ) == "unchanged-snapshot" );
    CHECK( features.value( "explicit_filter_transactions" ).toBool() );
    CHECK( features.value( "applied_filter_observation" ).toBool() );
    CHECK( features.value( "filter_kept_pane_creation" ).toBool() );
    CHECK( capabilities.value( "limits" ).toObject().value( "read_rows" ).toInt() == 256 );
    CHECK( fixture.window.inventorySnapshot().revision == before.revision );
    CHECK( fixture.window.inventorySnapshot().tabs.empty() );
}

TEST_CASE( "Document control opens privately and closes through native retirement without focus",
           "[control-documents]" )
{
    DocumentControlFixture fixture;
    QWidget editorWindow;
    QLineEdit editor{ &editorWindow };
    editorWindow.show();
    editor.setFocus();
    REQUIRE( waitUiState( [&] { return QApplication::focusWidget() == &editor; } ) );
    const auto pending = fixture.open( fixture.path );
    CHECK( pending.value( "status" ) == "pending" );
    CHECK( fixture.window.inventorySnapshot().tabs.empty() );
    const auto opened = fixture.await( pending );
    REQUIRE( opened.value( "status" ) == "completed" );
    const auto metadata = opened.value( "result" ).toObject();
    CHECK_FALSE( metadata.contains( "text" ) );
    REQUIRE( fixture.window.inventorySnapshot().tabs.size() == 1 );
    CHECK( metadata.value( "tab_id" ).toString() == fixture.window.inventorySnapshot().tabs.front().tabId );
    CHECK( QApplication::focusWidget() == &editor );
    const auto closed = fixture.call( "documents.close", {
        { "window_id", fixture.window.runtimeWindowId() }, { "tab_id", metadata.value( "tab_id" ) } } );
    REQUIRE( closed.value( "status" ) == "completed" );
    CHECK( fixture.window.inventorySnapshot().tabs.empty() );
    CHECK( QApplication::focusWidget() == &editor );
}

TEST_CASE( "Document control cancels preparation and distinguishes missing from empty sources",
           "[control-documents]" )
{
    DocumentControlFixture fixture;
    SECTION( "cancel before the GUI can publish" ) {
        const auto pending = fixture.open( fixture.path );
        fixture.call( "operation.cancel", { { "operation_id", pending.value( "operation_id" ) } } );
        CHECK( fixture.await( pending ).value( "status" ) == "cancelled" );
    }
    SECTION( "missing source" ) {
        const auto outcome = fixture.await( fixture.open( fixture.root.filePath( "missing.log" ) ) );
        CHECK( outcome.value( "status" ) == "failed" );
        CHECK( outcome.value( "error" ).toObject().value( "code" ) == "SOURCE_NOT_FOUND" );
    }
    CHECK( fixture.window.inventorySnapshot().tabs.empty() );
}

TEST_CASE( "Opening an existing document does not reacquire its missing pathname",
           "[control-documents][control-document-existing]" )
{
    DocumentControlFixture fixture;
    const auto first = fixture.await( fixture.open( fixture.path ) );
    REQUIRE( first.value( "status" ) == "completed" );
    REQUIRE( QFile::remove( fixture.path ) );
    const auto again = fixture.open( fixture.path );
    REQUIRE( again.value( "status" ) == "completed" );
    CHECK( again.value( "result" ).toObject().value( "tab_id" )
           == first.value( "result" ).toObject().value( "tab_id" ) );
    CHECK( fixture.window.inventorySnapshot().tabs.size() == 1 );
}

TEST_CASE( "Disabling document control discards unpublished work without joining the worker",
           "[control-documents]" )
{
    DocumentControlFixture fixture;
    fixture.open( fixture.path );
    fixture.adapter.reset();
    REQUIRE( QThreadPool::globalInstance()->waitForDone( 10000 ) );
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    CHECK( fixture.window.inventorySnapshot().tabs.empty() );
}

TEST_CASE( "Expired document work cannot publish after its worker finishes",
           "[control-documents]" )
{
    DocumentControlFixture fixture;
    qint64 clock = 0;
    ReviewControlAdapter adapter{ [&] { return std::vector<QPointer<MainWindow>>{ &fixture.window }; },
                                  [&] { return clock; } };
    const auto pending = documentResult( adapter.dispatch( { "expiry", "documents.open", {
        { "window_id", fixture.window.runtimeWindowId() }, { "path", fixture.path } } } ) );
    clock = 5 * 60000 + 1;
    const auto expired = adapter.dispatch( { "expiry", "operation.get", {
        { "operation_id", pending.value( "operation_id" ) } } } );
    REQUIRE( std::holds_alternative<klogg::control::RpcError>( expired ) );
    CHECK( std::get<klogg::control::RpcError>( expired ).data.toObject().value( "code" ) == "OPERATION_NOT_FOUND" );
    REQUIRE( QThreadPool::globalInstance()->waitForDone( 10000 ) );
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    CHECK( fixture.window.inventorySnapshot().tabs.empty() );
}

TEST_CASE( "Document preparation does not retain or recreate a closed target window",
           "[control-documents]" )
{
    DocumentControlFixture fixture;
    auto owner = std::make_unique<MainWindow>( WindowSession{ fixture.session, "Gone", 1 } );
    QPointer<MainWindow> observed{ owner.get() };
    ReviewControlAdapter adapter{ [&] { return std::vector<QPointer<MainWindow>>{ observed }; } };
    const auto pending = documentResult( adapter.dispatch( { "gone", "documents.open", {
        { "window_id", owner->runtimeWindowId() }, { "path", fixture.path } } } ) );
    owner.reset();
    QJsonObject result;
    REQUIRE( waitUiState( [&] {
        result = documentResult( adapter.dispatch( { "gone", "operation.get", {
            { "operation_id", pending.value( "operation_id" ) } } } ) );
        return result.value( "status" ) != "pending";
    } ) );
    CHECK( result.value( "status" ) == "failed" );
    CHECK( result.value( "error" ).toObject().value( "code" ) == "TARGET_CLOSED" );
    CHECK( fixture.session->openedDocuments().empty() );
}

TEST_CASE( "Explicit lifecycle operations stage native documents without publishing membership",
           "[control-documents][control-staged-documents]" )
{
    DocumentControlFixture fixture;
    auto tx = fixture.call( "transaction.begin", {} ).value( "transaction_id" );
    const auto windowId = fixture.window.runtimeWindowId();
    const bool folder = GENERATE( false, true );
    const auto started = fixture.call( "documents.open", { { "window_id", windowId },
        { "path", folder ? fixture.root.path() : fixture.path }, { "kind", folder ? "folder" : "file" },
        { "transaction_id", tx } } );
    const auto operation = fixture.await( started );
    REQUIRE( operation.value( "status" ) == "completed" );
    const auto tabId = operation.value( "result" ).toObject().value( "tab_id" );
    REQUIRE_FALSE( tabId.toString().isEmpty() );
    CHECK( fixture.window.inventorySnapshot().tabs.empty() );
    CHECK( fixture.session->openedDocuments().empty() );
    SECTION( "abort leaves no membership" ) {
        CHECK( fixture.call( "transaction.abort", { { "transaction_id", tx } } ).value( "status" ) == "aborted" );
        CHECK( fixture.session->openedDocuments().empty() );
        CHECK( fixture.window.inventorySnapshot().tabs.empty() );
    }
    SECTION( "commit adopts the same tab and close waits for its own commit" ) {
        CHECK( fixture.call( "transaction.commit", { { "transaction_id", tx } } ).value( "status" ) == "committed" );
        REQUIRE( fixture.window.inventorySnapshot().tabs.size() == 1 );
        CHECK( fixture.window.inventorySnapshot().tabs.front().tabId == tabId.toString() );
        tx = fixture.call( "transaction.begin", {} ).value( "transaction_id" );
        fixture.call( "documents.close", { { "window_id", windowId }, { "tab_id", tabId }, { "transaction_id", tx } } );
        CHECK( fixture.session->openedDocuments().size() == 1 );
        CHECK( fixture.call( "transaction.commit", { { "transaction_id", tx } } ).value( "status" ) == "committed" );
        CHECK( fixture.session->openedDocuments().empty() );
        CHECK( fixture.window.inventorySnapshot().tabs.empty() );
    }
}

TEST_CASE( "An explicit open never binds to a replacement document or leaves a committable failed stage",
           "[control-documents][control-staged-documents]" )
{
    DocumentControlFixture fixture;
    const auto tx = fixture.call( "transaction.begin", {} ).value( "transaction_id" );
    QJsonObject args{ { "window_id", fixture.window.runtimeWindowId() }, { "path", fixture.path }, { "transaction_id", tx } };
    SECTION( "human opens the same path before the worker is delivered" ) {
        const auto started = fixture.call( "documents.open", args );
        fixture.window.loadFileNonInteractive( fixture.path );
        CHECK( fixture.await( started ).value( "status" ) == "failed" );
        REQUIRE( fixture.window.inventorySnapshot().tabs.size() == 1 );
    }
    SECTION( "human opens the same path after staging" ) {
        REQUIRE( fixture.await( fixture.call( "documents.open", args ) ).value( "status" ) == "completed" );
        fixture.window.loadFileNonInteractive( fixture.path );
    }
    SECTION( "cancellation poisons the prefix" ) {
        const auto started = fixture.call( "documents.open", args );
        fixture.call( "operation.cancel", { { "operation_id", started.value( "operation_id" ) } } );
        CHECK( fixture.await( started ).value( "status" ) == "cancelled" );
    }
    SECTION( "unavailable source poisons the prefix" ) {
        args.insert( "path", fixture.root.filePath( "missing.log" ) );
        CHECK( fixture.await( fixture.call( "documents.open", args ) ).value( "status" ) == "failed" );
    }
    const auto commit = fixture.adapter.dispatch( { "document-test", "transaction.commit", { { "transaction_id", tx } } } );
    CHECK( std::holds_alternative<klogg::control::RpcError>( commit ) );
}

TEST_CASE( "Explicit reuse and close validate the original tab and its group update",
           "[control-documents][control-staged-documents]" )
{
    DocumentControlFixture fixture;
    const auto opened = fixture.await( fixture.open( fixture.path ) );
    REQUIRE( opened.value( "status" ) == "completed" );
    const auto tabId = opened.value( "result" ).toObject().value( "tab_id" );
    const auto tx = fixture.call( "transaction.begin", {} ).value( "transaction_id" );
    const QJsonObject close{ { "window_id", fixture.window.runtimeWindowId() }, { "tab_id", tabId } };
    QString groupId;
    SECTION( "a reused path is not a lease on its next tab" ) {
        fixture.call( "documents.open", { { "transaction_id", tx }, { "window_id", fixture.window.runtimeWindowId() },
                                           { "path", fixture.path } } );
        fixture.call( "documents.close", close );
        const auto replacement = fixture.await( fixture.open( fixture.path ) );
        REQUIRE( replacement.value( "status" ) == "completed" );
        REQUIRE( replacement.value( "result" ).toObject().value( "tab_id" ) != tabId );
    }
    SECTION( "a human grouping the tab after staging is not overwritten" ) {
        auto stagedClose = close;
        stagedClose.insert( "transaction_id", tx );
        fixture.call( "documents.close", stagedClose );
        auto& groups = TabGroupManager::get();
        groups.createGroup( "Human group", Qt::red );
        groupId = groups.groups().last().id;
        groups.addTabToGroup( groupId, fixture.path );
    }
    const auto commit = fixture.adapter.dispatch( { "document-test", "transaction.commit", { { "transaction_id", tx } } } );
    CHECK( std::holds_alternative<klogg::control::RpcError>( commit ) );
    CHECK( fixture.window.inventorySnapshot().tabs.size() == 1 );
    if ( !groupId.isEmpty() ) {
        CHECK( TabGroupManager::get().groupIdForTab( fixture.path ) == groupId );
        TabGroupManager::get().deleteGroup( groupId );
    }
}

TEST_CASE( "Document control opens folders without a modal native open flow",
           "[control-documents]" )
{
    DocumentControlFixture fixture;
    const auto outcome = fixture.await( fixture.open( fixture.root.path(), "folder" ) );
    REQUIRE( outcome.value( "status" ) == "completed" );
    REQUIRE( fixture.window.inventorySnapshot().tabs.size() == 1 );
    CHECK( fixture.window.inventorySnapshot().tabs.front().kind == DocumentKind::Folder );
}
