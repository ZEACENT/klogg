#include <catch2/catch.hpp>

#include <algorithm>
#include <cstring>

#include <QBuffer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "controlcli.h"
#include "controlframing.h"
#include "qtcontainerindex.h"

using namespace klogg::control;

namespace {
InstanceDescriptor instance( const QString& id )
{
    InstanceDescriptor result;
    result.instanceId = id;
    result.installationId = "installation";
    result.profileId = "profile";
    result.endpoint = "/private/endpoint";
    result.token = "private-handshake-nonce";
    return result;
}

struct Fixture {
    QBuffer input;
    DiscoveryResult discovery{ { instance( "one" ) }, {}, 0 };
    ClientResult reply{ ClientStatus::Success, DecodedResponse{ QJsonObject{ { "count", "18446744073709551615" } } }, {} };
    int discoveries = 0;
    int calls = 0;
    int timeout = 0;
    QString selected;
    RpcRequest request;
    CliServices services{
        [this]( const RuntimeOptions& ) { ++discoveries; return discovery; },
        [this]( const InstanceDescriptor& target, const RpcRequest& value, int budget ) {
            ++calls;
            selected = target.instanceId;
            request = value;
            timeout = budget;
            return reply;
        }
    };

    Fixture() { REQUIRE( input.open( QIODevice::ReadOnly ) ); }
    void setInput( const QByteArray& bytes )
    {
        input.close();
        input.setData( bytes );
        REQUIRE( input.open( QIODevice::ReadOnly ) );
    }
    CliResult run( const QStringList& arguments )
    {
        return runControlCli( arguments, input, "test-version", services );
    }
};

QJsonObject object( const CliResult& result )
{
    const auto parsed = QJsonDocument::fromJson( result.standardOutput );
    REQUIRE( parsed.isObject() );
    REQUIRE( result.standardOutput.endsWith( '\n' ) );
    CHECK( result.standardError.isEmpty() );
    return parsed.object();
}

QString errorCode( const CliResult& result )
{
    const auto root = object( result );
    CHECK_FALSE( root.contains( "result" ) );
    return root.value( "error" ).toObject().value( "data" ).toObject().value( "code" ).toString();
}
} // namespace

TEST_CASE( "Control CLI help and version are offline", "[controlcli]" )
{
    Fixture f;
    const auto help = f.run( { "--help" } );
    CHECK( help.exit == CliExit::Success );
    CHECK( help.standardOutput.contains( "--params-file" ) );
    CHECK( help.standardOutput.contains( "Outcome" ) );
    CHECK( help.standardError.isEmpty() );
    const auto version = f.run( { "--version" } );
    CHECK( version.exit == CliExit::Success );
    CHECK( object( version ).value( "result" ).toObject().value( "version" ) == "test-version" );
    CHECK( f.discoveries == 0 );
    CHECK( f.calls == 0 );
}

TEST_CASE( "Control CLI discovery lists only public metadata", "[controlcli]" )
{
    Fixture f;
    f.discovery.instances.push_back( instance( "two" ) );
    const auto result = f.run( { "instance", "list" } );
    CHECK( result.exit == CliExit::Success );
    const auto instances = object( result ).value( "result" ).toObject().value( "instances" ).toArray();
    REQUIRE( instances.size() == 2 );
    CHECK( instances.first().toObject().value( "instance_id" ) == "one" );
    CHECK( instances.first().toObject().size() == 5 );
    CHECK_FALSE( result.standardOutput.contains( "private-handshake-nonce" ) );
    CHECK_FALSE( result.standardOutput.contains( "token" ) );
    CHECK_FALSE( result.standardOutput.contains( "/private/endpoint" ) );
    CHECK( f.calls == 0 );
}

TEST_CASE( "Control CLI selection never falls back from a missing explicit instance", "[controlcli]" )
{
    Fixture f;
    SECTION( "one can auto-select" ) {
        CHECK( f.run( { "state", "get" } ).exit == CliExit::Success );
        CHECK( f.selected == "one" );
        CHECK( f.timeout == 5000 );
    }
    SECTION( "explicit missing even with one" ) {
        const auto result = f.run( { "--instance", "missing", "state", "get" } );
        CHECK( result.exit == CliExit::Unavailable );
        CHECK( errorCode( result ) == "INSTANCE_NOT_FOUND" );
        CHECK( f.calls == 0 );
    }
    SECTION( "multiple require selection" ) {
        f.discovery.instances.push_back( instance( "two" ) );
        const auto result = f.run( { "state", "get" } );
        CHECK( result.exit == CliExit::Unavailable );
        CHECK( errorCode( result ) == "AMBIGUOUS_INSTANCE" );
        CHECK( f.calls == 0 );
        CHECK( f.run( { "state", "get", "--instance", "two" } ).exit == CliExit::Success );
        CHECK( f.selected == "two" );
    }
    SECTION( "duplicate IDs remain ambiguous" ) {
        f.discovery.instances.push_back( instance( "one" ) );
        CHECK( errorCode( f.run( { "--instance", "one", "state", "get" } ) ) == "AMBIGUOUS_INSTANCE" );
        CHECK( f.calls == 0 );
    }
    SECTION( "none available" ) {
        f.discovery.instances.clear();
        CHECK( errorCode( f.run( { "state", "get" } ) ) == "INSTANCE_NOT_FOUND" );
        CHECK( f.calls == 0 );
    }
    SECTION( "discovery diagnostics cannot echo secrets" ) {
        f.discovery.error = "private-handshake-nonce";
        const auto result = f.run( { "instance", "list" } );
        CHECK( result.exit == CliExit::Unavailable );
        CHECK_FALSE( result.standardOutput.contains( "private-handshake-nonce" ) );
        CHECK( f.calls == 0 );
    }
}

TEST_CASE( "Control CLI aliases use current named parameter contracts", "[controlcli]" )
{
    struct Case { QStringList args; QString method; QJsonObject params; };
    const std::vector<Case> cases{
        { { "state", "get" }, "state.get", {} },
        { { "pane", "get", "--tab", "t", "--pane", "main" }, "pane.get", { { "tab_id", "t" }, { "pane", "main" } } },
        { { "pane", "get", "--tab", "t", "--pane", "results" }, "pane.get", { { "tab_id", "t" }, { "pane", "results" } } },
        { { "tab", "get", "--tab", "t" }, "tab.get", { { "tab_id", "t" } } },
        { { "label", "list", "--tab", "t", "--transaction", "x" }, "label.list", { { "tab_id", "t" }, { "transaction_id", "x" } } },
        { { "tab", "get", "--tab", "t", "--transaction", "x" }, "tab.get", { { "tab_id", "t" }, { "transaction_id", "x" } } },
        { { "transaction", "begin" }, "transaction.begin", {} },
        { { "transaction", "get", "--transaction", "x" }, "transaction.get", { { "transaction_id", "x" } } },
        { { "transaction", "commit", "--transaction", "x" }, "transaction.commit", { { "transaction_id", "x" } } },
        { { "transaction", "abort", "--transaction", "x" }, "transaction.abort", { { "transaction_id", "x" } } }
    };
    for ( const auto& item : cases ) {
        Fixture f;
        CHECK( f.run( item.args ).exit == CliExit::Success );
        CHECK( f.calls == 1 );
        CHECK( f.request.method == item.method );
        CHECK( f.request.params == item.params );
        CHECK_FALSE( f.request.id.isEmpty() );
    }
}

TEST_CASE( "Control CLI raw JSON preserves strings Unicode and shell metacharacters", "[controlcli]" )
{
    Fixture f;
    const auto text = QString::fromUtf8( "caf\xC3\xA9 $(touch /never) ; |" );
    const QJsonObject params{ { "line", "18446744073709551615" }, { "text", text } };
    const auto result = f.run( { "call", "label.set", "--params",
        QString::fromUtf8( QJsonDocument( params ).toJson( QJsonDocument::Compact ) ),
        "--timeout-ms", "1234" } );
    CHECK( result.exit == CliExit::Success );
    CHECK( f.request.params == params );
    CHECK( f.timeout == 1234 );
    CHECK( object( result ).value( "result" ).toObject().value( "count" ) == "18446744073709551615" );
    CHECK( object( result ).value( "id" ).toString() == f.request.id );
}

TEST_CASE( "Control CLI rejects malformed or non-object params before discovery", "[controlcli]" )
{
    for ( const auto* value : { "", "{", "[]", "null", "false", "1", "\"secret\"", "{} {}" } ) {
        Fixture f;
        const auto result = f.run( { "call", "label.set", "--params", value } );
        CHECK( result.exit == CliExit::InvalidArguments );
        CHECK( errorCode( result ) == "INVALID_ARGUMENTS" );
        CHECK( f.discoveries == 0 );
        CHECK_FALSE( result.standardOutput.contains( "secret" ) );
    }
}

TEST_CASE( "Control CLI rejects excess unknown missing duplicate and overflowing arguments", "[controlcli]" )
{
    const std::vector<QStringList> cases{
        {}, { "state" }, { "state", "get", "extra" }, { "state", "get", "--unknown" },
        { "--help", "extra" }, { "--version", "--instance", "one" },
        { "state", "get", "--instance" }, { "state", "get", "--instance", "" },
        { "state", "get", "--instance", "one", "--instance", "two" },
        { "tab", "get" }, { "label", "list" }, { "transaction", "commit" },
        { "pane", "get", "--tab", "t" }, { "pane", "get", "--pane", "main" },
        { "pane", "get", "--tab", "t", "--pane", "other" },
        { "pane", "get", "--tab", "t", "--pane", "main", "--transaction", "x" },
        { "tab", "get", "--tab", "t", "--pane", "main" },
        { "transaction", "begin", "--tab", "t" }, { "state", "get", "--transaction", "x" },
        { "call", "" }, { "call", "state.get", "--tab", "x" },
        { "call", "state.get", "--params", "{}", "--params-file", "-" },
        { "call", "state.get", "--params", "{}", "--params", "{}" },
        { "state", "get", "--params", "{}" }, { "instance", "list", "--instance", "one" },
        { "state", "get", "--timeout-ms", "-1" }, { "state", "get", "--timeout-ms", "0" },
        { "state", "get", "--timeout-ms", "2147483648" },
        { "state", "get", "--timeout-ms", "99999999999999999999999" },
        { "state", "get", "--timeout-ms", "1.5" }, { "state", "get", "--timeout-ms", "+2" }
    };
    for ( const auto& args : cases ) {
        INFO( args.join( ' ' ).toStdString() );
        Fixture f;
        const auto result = f.run( args );
        CHECK( result.exit == CliExit::InvalidArguments );
        CHECK( errorCode( result ) == "INVALID_ARGUMENTS" );
        CHECK( f.discoveries == 0 );
        CHECK( f.calls == 0 );
    }
}

TEST_CASE( "Control CLI reads explicit bounded file and stdin objects", "[controlcli]" )
{
    Fixture f;
    QTemporaryDir directory;
    REQUIRE( directory.isValid() );
    const auto path = directory.filePath( QString::fromUtf8( "params caf\xC3\xA9.json" ) );
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly ) );
    const QByteArray payload = "{\"line\":\"18446744073709551615\"}";
    REQUIRE( file.write( payload ) == payload.size() );
    file.close();
    SECTION( "file including Unicode and spaces" ) {
        CHECK( f.run( { "call", "label.set", "--params-file", path } ).exit == CliExit::Success );
        CHECK( f.request.params.value( "line" ) == "18446744073709551615" );
    }
    SECTION( "stdin" ) {
        f.setInput( payload );
        CHECK( f.run( { "call", "label.set", "--params-file", "-" } ).exit == CliExit::Success );
        CHECK( f.request.params.value( "line" ) == "18446744073709551615" );
    }
    SECTION( "missing file" ) {
        CHECK( f.run( { "call", "label.set", "--params-file", path + ".missing" } ).exit == CliExit::InvalidArguments );
        CHECK( f.discoveries == 0 );
    }
    SECTION( "unselected stdin is untouched" ) {
        f.setInput( "not JSON" );
        CHECK( f.run( { "call", "state.get" } ).exit == CliExit::Success );
        CHECK( f.input.pos() == 0 );
    }
    SECTION( "oversized stdin is bounded" ) {
        f.setInput( QByteArray( static_cast<int>( MaxFrameBytes ) + 100, ' ' ) );
        CHECK( f.run( { "call", "label.set", "--params-file", "-" } ).exit == CliExit::InvalidArguments );
        CHECK( f.input.pos() <= MaxFrameBytes + 1 );
        CHECK( f.discoveries == 0 );
    }
    SECTION( "oversized file" ) {
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        REQUIRE( file.resize( MaxFrameBytes + 1 ) );
        file.close();
        CHECK( f.run( { "call", "label.set", "--params-file", path } ).exit == CliExit::InvalidArguments );
        CHECK( f.discoveries == 0 );
    }
    SECTION( "oversized inline input" ) {
        CHECK( f.run( { "call", "label.set", "--params", QString( static_cast<int>( MaxFrameBytes ) + 1, ' ' ) } ).exit == CliExit::InvalidArguments );
        CHECK( f.discoveries == 0 );
    }
    SECTION( "exact cap of whitespace padded object is accepted" ) {
        QByteArray bytes( static_cast<int>( MaxFrameBytes ) - 2, ' ' );
        bytes += "{}";
        f.setInput( bytes );
        CHECK( f.run( { "call", "state.get", "--params-file", "-" } ).exit == CliExit::Success );
    }
    SECTION( "valid params still must fit request envelope" ) {
        const QString text( static_cast<int>( MaxFrameBytes ) - 12, 'a' );
        const QString params = "{\"text\":\"" + text + "\"}";
        CHECK( f.run( { "call", "label.set", "--params", params } ).exit == CliExit::InvalidArguments );
        CHECK( f.discoveries == 0 );
    }
}

TEST_CASE( "Control CLI drains short sequential reads until EOF not availability", "[controlcli]" )
{
    class ChunkedInput final : public QIODevice {
      public:
        QByteArray pending = "{\"line\":\"18446744073709551615\"}";
        bool failAtEnd = false;
        bool isSequential() const override { return true; }
        // Sequential devices may report no currently buffered data before EOF.
        bool atEnd() const override { return true; }
        qint64 readData( char* data, qint64 maximum ) override
        {
            if ( pending.isEmpty() ) return failAtEnd ? -1 : 0;
            const auto size = std::min( { maximum, qint64{ 5 }, static_cast<qint64>( pending.size() ) } );
            std::memcpy( data, pending.constData(), static_cast<size_t>( size ) );
            pending.remove( 0, static_cast<klogg::ContainerIndex>( size ) );
            return size;
        }
        qint64 writeData( const char*, qint64 ) override { return -1; }
    } input;
    REQUIRE( input.open( QIODevice::ReadOnly | QIODevice::Unbuffered ) );
    Fixture f;
    SECTION( "drain every fragment" ) {
        const auto result = runControlCli( { "call", "label.set", "--params-file", "-" }, input,
                                           "test-version", f.services );
        CHECK( result.exit == CliExit::Success );
        CHECK( f.request.params.value( "line" ) == "18446744073709551615" );
        CHECK( input.pending.isEmpty() );
    }
    SECTION( "read error after a valid object still prevents dispatch" ) {
        input.failAtEnd = true;
        const auto result = runControlCli( { "call", "label.set", "--params-file", "-" }, input,
                                           "test-version", f.services );
        CHECK( result.exit == CliExit::InvalidArguments );
        CHECK( f.calls == 0 );
    }
}

TEST_CASE( "Control CLI distinguishes rejection unsent and unknown without retry", "[controlcli]" )
{
    Fixture f;
    SECTION( "remote rejection retains stable code but not arbitrary diagnostics" ) {
        RpcError error;
        error.remote = true;
        error.code = -32000;
        error.message = "private-handshake-nonce and sensitive payload";
        error.data = QJsonObject{ { "code", "REVISION_CONFLICT" }, { "details", "private-handshake-nonce" } };
        f.reply = { ClientStatus::Rejected, DecodedResponse{ error }, {} };
        const auto result = f.run( { "transaction", "commit", "--transaction", "tx" } );
        CHECK( result.exit == CliExit::Rejected );
        CHECK( errorCode( result ) == "REVISION_CONFLICT" );
        CHECK_FALSE( result.standardOutput.contains( "private-handshake-nonce" ) );
        CHECK_FALSE( result.standardOutput.contains( "sensitive payload" ) );
    }
    SECTION( "unknown warns against retry" ) {
        f.reply = { ClientStatus::OutcomeUnknown, {}, "private-handshake-nonce" };
        const auto result = f.run( { "transaction", "commit", "--transaction", "tx" } );
        CHECK( result.exit == CliExit::OutcomeUnknown );
        CHECK( errorCode( result ) == "OUTCOME_UNKNOWN" );
        CHECK( result.standardOutput.contains( "not automatically retry" ) );
        CHECK_FALSE( result.standardOutput.contains( "private-handshake-nonce" ) );
    }
    SECTION( "not sent" ) {
        f.reply = { ClientStatus::NotSent, {}, "private-handshake-nonce" };
        const auto result = f.run( { "state", "get" } );
        CHECK( result.exit == CliExit::Unavailable );
        CHECK( errorCode( result ) == "NOT_SENT" );
        CHECK_FALSE( result.standardOutput.contains( "private-handshake-nonce" ) );
    }
    CHECK( f.calls == 1 );
}
