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
    for ( const auto& method : methods ) {
        const auto response = fixture.adapter.dispatch( { "advertised", method.toString(), {} } );
        if ( const auto* error = std::get_if<klogg::control::RpcError>( &response ) ) {
            CHECK( error->data.toObject().value( "code" ) != "METHOD_NOT_FOUND" );
        } else if ( method == "transaction.begin" ) {
            const auto id = std::get<QJsonValue>( response ).toObject().value( "transaction_id" );
            fixture.call( "transaction.abort", { { "transaction_id", id } } );
        }
    }
    const auto features = capabilities.value( "features" ).toObject();
    CHECK_FALSE( features.value( "explicit_lifecycle_transactions" ).toBool( true ) );
    CHECK_FALSE( features.value( "live_lifecycle" ).toBool( true ) );
    CHECK( features.value( "implicit_filter_kinds" ).toArray() == QJsonArray{ "file", "live" } );
    CHECK( features.value( "filter_source_policy" ) == "unchanged-snapshot" );
    CHECK_FALSE( features.value( "explicit_filter_transactions" ).toBool( true ) );
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

TEST_CASE( "Document control opens folders without a modal native open flow",
           "[control-documents]" )
{
    DocumentControlFixture fixture;
    const auto outcome = fixture.await( fixture.open( fixture.root.path(), "folder" ) );
    REQUIRE( outcome.value( "status" ) == "completed" );
    REQUIRE( fixture.window.inventorySnapshot().tabs.size() == 1 );
    CHECK( fixture.window.inventorySnapshot().tabs.front().kind == DocumentKind::Folder );
}
