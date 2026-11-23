#include <catch2/catch.hpp>

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSysInfo>
#include <QElapsedTimer>
#include <QProcess>
#include <stdexcept>
#include <cstdlib>
#include <QProcessEnvironment>
#include <functional>

#include "controlcli.h"
#include "controlframing.h"
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <thread>

#include "controltransport.h"

using namespace klogg::control;

namespace {
RuntimeOptions roots( const QTemporaryDir& directory )
{
    const auto path = QFileInfo( directory.path() ).canonicalFilePath();
    return { path + QStringLiteral( "/discovery" ), {},
             QStringLiteral( "test-install" ), QStringLiteral( "test-profile" ) };
}

ClientResult invoke( const InstanceDescriptor& instance, const RpcRequest& request,
                     int timeoutMs = 2000 )
{
    QEventLoop loop;
    ClientResult result;
    std::thread client( [&] {
        result = callLocal( instance, request, timeoutMs );
        QMetaObject::invokeMethod( &loop, "quit", Qt::QueuedConnection );
    } );
    loop.exec();
    client.join();
    return result;
}
QByteArray pack( const QJsonObject& object )
{
    return *encodeFrame( QJsonDocument( object ).toJson( QJsonDocument::Compact ) );
}
QJsonObject greeting( const InstanceDescriptor& descriptor )
{
    return { { "type", "hello" }, { "protocol", 1 }, { "instance", descriptor.instanceId },
             { "token", descriptor.token } };
}
QByteArray applicationRequest()
{
    return pack( { { "jsonrpc", "2.0" }, { "id", "request" }, { "method", "inspect" } } );
}
std::optional<QByteArray> receive( QLocalSocket& socket )
{
    FrameDecoder decoder;
    QElapsedTimer budget;
    budget.start();
    while ( budget.elapsed() < 2000 ) {
        const int remaining = static_cast<int>( 2000 - budget.elapsed() );
        if ( socket.bytesAvailable() == 0 && !socket.waitForReadyRead( remaining ) ) return {};
        const auto frames = decoder.feed( socket.read( 4096 ) );
        if ( frames.error != FrameError::None ) return {};
        if ( !frames.messages.empty() ) return frames.messages.front();
    }
    return {};
}
bool flush( QLocalSocket& socket, const QByteArray& bytes )
{
    if ( socket.write( bytes ) != bytes.size() ) return false;
    while ( socket.bytesToWrite() > 0 ) {
        if ( !socket.waitForBytesWritten( 2000 ) ) return false;
    }
    return true;
}
bool closed( QLocalSocket& socket )
{
    return socket.state() == QLocalSocket::UnconnectedState || socket.waitForDisconnected( 2000 );
}
bool peer( const InstanceDescriptor& descriptor, const std::function<bool( QLocalSocket& )>& action )
{
    bool result = false;
    QEventLoop loop;
    std::thread client( [&] {
        QLocalSocket socket;
        socket.connectToServer( descriptor.endpoint );
        result = socket.waitForConnected( 2000 ) && action( socket );
        QMetaObject::invokeMethod( &loop, "quit", Qt::QueuedConnection );
    } );
    loop.exec();
    client.join();
    return result;
}
bool authenticate( QLocalSocket& socket, const InstanceDescriptor& descriptor )
{
    return flush( socket, pack( greeting( descriptor ) ) ) && receive( socket ).has_value();
}
DecodedResponse okay( const RpcRequest& ) { return QJsonValue( true ); }

struct UnresponsivePeer {
    ControlRuntime runtime;
    QLocalServer listener;
    int connections = 0;
    int requests = 0;
    bool sendHello = true;
    QString wrongInstance;
    int helloVersion = 1;

    bool start( const RuntimeOptions& options )
    {
        if ( !runtime.prepare( options, nullptr ) ) return false;
        const auto fd = runtime.createListener( nullptr );
        listener.setSocketOptions( QLocalServer::UserAccessOption );
        if ( !( fd >= 0 ? listener.listen( fd ) : listener.listen( runtime.descriptor().endpoint ) ) ) return false;
        QObject::connect( &listener, &QLocalServer::newConnection, &listener, [&] {
            auto* socket = listener.nextPendingConnection();
            ++connections;
            auto decoder = std::make_shared<FrameDecoder>();
            auto greeted = std::make_shared<bool>( false );
            QObject::connect( socket, &QLocalSocket::readyRead, socket, [=] {
                const auto frames = decoder->feed( socket->read( 4096 ) );
                for ( const auto& frame : frames.messages ) {
                    if ( !*greeted ) {
                        *greeted = true;
                        if ( sendHello ) {
                            auto value = QJsonDocument::fromJson( frame ).object();
                            value.remove( "token" );
                            value.insert( "protocol", helloVersion );
                            if ( !wrongInstance.isEmpty() ) value.insert( "instance", wrongInstance );
                            socket->write( pack( value ) );
                        }
                    }
                    else ++requests;
                }
            } );
        } );
        return true;
    }
};

void rewriteDescriptor( const QString& path, const std::function<void( QJsonObject& )>& change )
{
    QFile original( path );
    REQUIRE( original.open( QIODevice::ReadOnly ) );
    auto object = QJsonDocument::fromJson( original.readAll() ).object();
    original.close();
    change( object );
    REQUIRE( original.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( original.write( QJsonDocument( object ).toJson() ) > 0 );
}
} // namespace

TEST_CASE( "Local control discovers and handshakes before a typed round trip", "[controltransport]" )
{
    QTemporaryDir directory( QDir::tempPath() + QStringLiteral( "/kt-XXXXXX" ) );
    REQUIRE( directory.isValid() );
    ControlServer server;
    QString error;
    int calls = 0;
    const bool started = server.start( roots( directory ), [&]( const RpcRequest& request ) -> DecodedResponse {
        CHECK( QThread::currentThread() == server.thread() );
        CHECK( request.method == QStringLiteral( "inspect" ) );
        ++calls;
        return request.params;
    }, &error );
    INFO( error.toStdString() );
    REQUIRE( started );
    const auto discovery = discoverInstances( roots( directory ) );
    REQUIRE( discovery.error.isEmpty() );
    REQUIRE( discovery.instances.size() == 1 );
    const auto result = invoke( discovery.instances.front(),
                               { QStringLiteral( "one" ), QStringLiteral( "inspect" ),
                                 { { QStringLiteral( "answer" ), 42 } } } );
    REQUIRE( result.status == ClientStatus::Success );
    REQUIRE( result.response );
    CHECK( std::get<QJsonValue>( *result.response ).toObject().value( "answer" ).toInt() == 42 );
    CHECK( calls == 1 );
    server.stop();
    CHECK( discoverInstances( roots( directory ) ).instances.empty() );
}

TEST_CASE( "Local control rejects an unauthenticated peer without dispatch", "[controltransport]" )
{
    QTemporaryDir directory;
    ControlServer server;
    int calls = 0;
    REQUIRE( server.start( roots( directory ), [&]( const RpcRequest& ) -> DecodedResponse { ++calls; return true; } ) );
    const auto descriptor = server.descriptor();
    auto invalid = greeting( descriptor );
    SECTION( "wrong token" ) { invalid.insert( "token", QString( 64, '0' ) ); }
    SECTION( "wrong instance" ) { invalid.insert( "instance", QString( 32, '0' ) ); }
    SECTION( "wrong protocol" ) { invalid.insert( "protocol", 2 ); }
    SECTION( "application before hello" ) { invalid = { { "jsonrpc", "2.0" }, { "id", "one" }, { "method", "inspect" } }; }
    REQUIRE( peer( descriptor, [&]( QLocalSocket& socket ) {
        return flush( socket, pack( invalid ) + applicationRequest() ) && closed( socket );
    } ) );
    CHECK( calls == 0 );
    CHECK( server.connectionCount() == 0 );
}

TEST_CASE( "Local control bounds framing and isolates malformed RPC", "[controltransport]" )
{
    QTemporaryDir directory;
    ControlServer server;
    int calls = 0;
    REQUIRE( server.start( roots( directory ), [&]( const RpcRequest& ) -> DecodedResponse { ++calls; return true; } ) );
    const auto descriptor = server.descriptor();
    SECTION( "fragmented hello and request" ) {
        REQUIRE( peer( descriptor, [&]( QLocalSocket& socket ) {
            const auto handshake = pack( greeting( descriptor ) );
            for ( const char byte : handshake ) if ( !flush( socket, QByteArray( 1, byte ) ) ) return false;
            if ( !receive( socket ) ) return false;
            const auto request = applicationRequest();
            for ( const char byte : request ) if ( !flush( socket, QByteArray( 1, byte ) ) ) return false;
            const auto response = receive( socket );
            return response && std::holds_alternative<QJsonValue>( decodeResponse( *response, "request" ) );
        } ) );
        CHECK( calls == 1 );
    }
    SECTION( "malformed JSON is an RPC error and never reaches callback" ) {
        REQUIRE( peer( descriptor, [&]( QLocalSocket& socket ) {
            if ( !authenticate( socket, descriptor ) || !flush( socket, *encodeFrame( "{" ) ) ) return false;
            const auto response = receive( socket );
            return response && QJsonDocument::fromJson( *response ).object().value( "error" ).toObject().value( "code" ).toInt() == -32700;
        } ) );
        CHECK( calls == 0 );
    }
    SECTION( "oversize header closes without reading payload" ) {
        REQUIRE( peer( descriptor, [&]( QLocalSocket& socket ) {
            return authenticate( socket, descriptor ) && flush( socket, QByteArray::fromHex( "00100001" ) ) && closed( socket );
        } ) );
        CHECK( calls == 0 );
    }
    SECTION( "zero frame closes" ) {
        REQUIRE( peer( descriptor, [&]( QLocalSocket& socket ) {
            return flush( socket, QByteArray( 4, '\0' ) ) && closed( socket );
        } ) );
        CHECK( calls == 0 );
    }
    SECTION( "truncated request cannot execute" ) {
        REQUIRE( peer( descriptor, [&]( QLocalSocket& socket ) {
            if ( !authenticate( socket, descriptor ) || !flush( socket, applicationRequest().left( 8 ) ) ) return false;
            socket.disconnectFromServer();
            return closed( socket );
        } ) );
        CHECK( calls == 0 );
    }
}

TEST_CASE( "Local control contains callback failure and correlates rejection", "[controltransport]" )
{
    QTemporaryDir directory;
    ControlServer server;
    REQUIRE( server.start( roots( directory ), []( const RpcRequest& ) -> DecodedResponse {
        throw std::runtime_error( "SECRET EXCEPTION CONTENT" );
    } ) );
    const auto result = invoke( server.descriptor(), { "one", "inspect", {} } );
    REQUIRE( result.status == ClientStatus::Rejected );
    REQUIRE( result.response );
    const auto error = std::get<RpcError>( *result.response );
    CHECK( error.code == -32603 );
    CHECK( error.message == QStringLiteral( "Control request failed" ) );
    CHECK( error.remote );
}

TEST_CASE( "Stopping or destroying local control cancels already queued dispatch", "[controltransport]" )
{
    QTemporaryDir directory;
    auto server = std::make_unique<ControlServer>();
    int calls = 0;
    bool destroy = false;
    SECTION( "stop during callback" ) {}
    SECTION( "destroy during callback" ) { destroy = true; }
    REQUIRE( server->start( roots( directory ), [&]( const RpcRequest& ) -> DecodedResponse {
        ++calls;
        if ( destroy ) server.reset();
        else server->stop();
        return true;
    } ) );
    const auto descriptor = server->descriptor();
    const auto path = server->descriptorPath();
    REQUIRE( peer( descriptor, [&]( QLocalSocket& socket ) {
        return authenticate( socket, descriptor )
               && flush( socket, applicationRequest() + applicationRequest() ) && closed( socket );
    } ) );
    CHECK( calls == 1 );
    CHECK_FALSE( QFileInfo::exists( path ) );
    CHECK( invoke( descriptor, { "later", "inspect", {} } ).status == ClientStatus::NotSent );
    CHECK( calls == 1 );
}

TEST_CASE( "Local control epochs rotate and disabled servers publish nothing", "[controltransport]" )
{
    QTemporaryDir directory;
    ControlServer server;
    CHECK_FALSE( server.isRunning() );
    CHECK( discoverInstances( roots( directory ) ).instances.empty() );
    REQUIRE( server.start( roots( directory ), okay ) );
    const auto old = server.descriptor();
    server.stop();
    CHECK_FALSE( server.isRunning() );
    CHECK( invoke( old, { "one", "inspect", {} } ).status == ClientStatus::NotSent );
    REQUIRE( server.start( roots( directory ), okay ) );
    CHECK( server.descriptor().instanceId != old.instanceId );
    CHECK( server.descriptor().token != old.token );
    CHECK( discoverInstances( roots( directory ) ).instances.size() == 1 );
}

TEST_CASE( "Local control bounds connections, handshake time, and queued output", "[controltransport]" )
{
    QTemporaryDir directory;
    ControlServer server;
    int calls = 0;
    TransportLimits limits;
    limits.maxConnections = 1;
    limits.maxQueuedWriteBytes = 1024;
    const ControlServer::Handler handler = [&]( const RpcRequest& ) -> DecodedResponse {
        ++calls;
        return QString( 2048, 'x' );
    };
    REQUIRE( server.start( roots( directory ), handler, nullptr, limits ) );
    const auto descriptor = server.descriptor();
    SECTION( "idle handshake reaches the server deadline" ) {
        limits.handshakeTimeoutMs = 40;
        REQUIRE( server.start( roots( directory ), handler, nullptr, limits ) );
        REQUIRE( peer( server.descriptor(), [&]( QLocalSocket& socket ) { return closed( socket ); } ) );
        CHECK( server.connectionCount() == 0 );
        CHECK( calls == 0 );
    }
    SECTION( "a second live connection is rejected" ) {
        REQUIRE( peer( descriptor, [&]( QLocalSocket& first ) {
            if ( !authenticate( first, descriptor ) ) return false;
            QLocalSocket second;
            second.connectToServer( descriptor.endpoint );
            return second.waitForConnected( 2000 ) && closed( second );
        } ) );
        CHECK( calls == 0 );
    }
    SECTION( "output limit closes instead of unbounded queueing or retry" ) {
        const auto result = invoke( descriptor, { "one", "inspect", {} } );
        CHECK( result.status == ClientStatus::OutcomeUnknown );
        CHECK( calls == 1 );
    }
}

TEST_CASE( "Blocking client verifies the server hello before submitting application work", "[controltransport]" )
{
    QTemporaryDir directory;
    UnresponsivePeer remote;
    SECTION( "wrong instance in server hello" ) { remote.wrongInstance = QString( 32, 'a' ); }
    SECTION( "wrong protocol in server hello" ) { remote.helloVersion = 2; }
    SECTION( "silent handshake exhausts finite timeout" ) { remote.sendHello = false; }
    REQUIRE( remote.start( roots( directory ) ) );
    const auto result = invoke( remote.runtime.descriptor(), { "one", "inspect", {} }, 2000 );
    CHECK( result.status == ClientStatus::NotSent );
    CHECK( remote.requests == 0 );
    CHECK( remote.connections == 1 );
}

TEST_CASE( "Blocking client reports an unknown outcome without replay after request timeout", "[controltransport]" )
{
    QTemporaryDir directory;
    UnresponsivePeer remote;
    REQUIRE( remote.start( roots( directory ) ) );
    const auto result = invoke( remote.runtime.descriptor(), { "one", "mutate", {} }, 2000 );
    CHECK( result.status == ClientStatus::OutcomeUnknown );
    CHECK( remote.requests == 1 );
    CHECK( remote.connections == 1 );
}

TEST_CASE( "Blocking client refuses substituted socket parent permissions", "[controltransport]" )
{
    if ( QSysInfo::kernelType() == QStringLiteral( "winnt" ) ) return;
    QTemporaryDir directory;
    ControlServer server;
    int calls = 0;
    REQUIRE( server.start( roots( directory ), [&]( const RpcRequest& ) -> DecodedResponse { ++calls; return true; } ) );
    const auto descriptor = server.descriptor();
    const auto socketDirectory = QFileInfo( descriptor.endpoint ).path();
    const auto privateMode = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    REQUIRE( QFile::setPermissions( socketDirectory, privateMode | QFile::ReadOther | QFile::ExeOther ) );
    const auto result = invoke( descriptor, { "one", "inspect", {} } );
    // Restore private mode before assertions so resource cleanup remains safe.
    REQUIRE( QFile::setPermissions( socketDirectory, privateMode ) );
    CHECK( result.status == ClientStatus::NotSent );
    CHECK( calls == 0 );
}

TEST_CASE( "Control discovery rejects descriptor substitution and invalid identities", "[controltransport]" )
{
    QTemporaryDir directory;
    ControlServer server;
    REQUIRE( server.start( roots( directory ), okay ) );
    const auto path = server.descriptorPath();
    SECTION( "wrong descriptor version" ) { rewriteDescriptor( path, []( QJsonObject& o ) { o.insert( "descriptorVersion", 2 ); } ); }
    SECTION( "wrong protocol version" ) { rewriteDescriptor( path, []( QJsonObject& o ) { o.insert( "protocolMax", 2 ); } ); }
    SECTION( "instance disagrees with filename" ) { rewriteDescriptor( path, []( QJsonObject& o ) { o.insert( "instanceId", QString( 32, 'a' ) ); } ); }
    SECTION( "remote or unrelated endpoint" ) { rewriteDescriptor( path, []( QJsonObject& o ) { o.insert( "endpoint", "https://example.invalid/" ); } ); }
    SECTION( "invalid token" ) { rewriteDescriptor( path, []( QJsonObject& o ) { o.insert( "token", "not-a-token" ); } ); }
    SECTION( "oversized descriptor" ) {
        QFile file( path );
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
        REQUIRE( file.write( QByteArray( 8193, ' ' ) ) == 8193 );
    }
    const auto discovery = discoverInstances( roots( directory ) );
    CHECK( discovery.instances.empty() );
    CHECK( discovery.rejectedDescriptors == 1 );
}

TEST_CASE( "Control test child abandons its published instance", "[.control-crash-publisher]" )
{
    const auto root = qEnvironmentVariable( "KLOGG_TEST_ABANDON_CONTROL_ROOT" );
    const auto sockets = qEnvironmentVariable( "KLOGG_TEST_ABANDON_SOCKET_ROOT" );
    REQUIRE_FALSE( root.isEmpty() );
    REQUIRE_FALSE( sockets.isEmpty() );
    ControlServer child;
    REQUIRE( child.start( { root, sockets, "test-install", "test-profile" }, okay ) );
    // Like a killed GUI, the OS closes its handles without running any application cleanup.
    std::_Exit( EXIT_SUCCESS );
}

TEST_CASE( "Abandoned control epochs never hide the only live GUI or accumulate on restart",
           "[controltransport][control-abandoned]" )
{
    QTemporaryDir directory;
    auto options = roots( directory );
    // Darwin's deep per-user TMPDIR can exceed the AF_UNIX pathname limit.
    const auto shortRoot = QSysInfo::kernelType() == QStringLiteral( "winnt" )
        ? QDir::tempPath() : QFileInfo( QStringLiteral( "/tmp" ) ).canonicalFilePath();
    QTemporaryDir sockets( shortRoot + QStringLiteral( "/kt-XXXXXX" ) );
    REQUIRE( sockets.isValid() );
    options.socketRoot = QFileInfo( sockets.path() ).canonicalFilePath();
    ControlServer live;
    REQUIRE( live.start( options, okay ) );
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert( "KLOGG_TEST_ABANDON_CONTROL_ROOT", options.descriptorRoot );
    environment.insert( "KLOGG_TEST_ABANDON_SOCKET_ROOT", options.socketRoot );
    const auto countRegistrations = [&] {
        return QFileInfo( live.descriptorPath() ).absoluteDir().entryList( QDir::Files | QDir::Hidden ).size();
    };
    for ( int attempt = 0; attempt < 3; ++attempt ) {
        QProcess child;
        child.setProcessEnvironment( environment );
        child.start( QCoreApplication::applicationFilePath(), { "Control test child abandons its published instance" } );
        REQUIRE( child.waitForFinished( 30000 ) );
        REQUIRE( child.exitStatus() == QProcess::NormalExit );
        REQUIRE( child.exitCode() == 0 );
        // Do not depend on intervening CLI calls for garbage collection.
        CHECK( countRegistrations() == 2 );
        if ( QSysInfo::kernelType() != QStringLiteral( "winnt" ) ) {
            CHECK( QDir( options.socketRoot ).entryList( QDir::Dirs | QDir::NoDotAndDotDot ).size() == 2 );
        }
    }
    const auto discovered = discoverInstances( options );
    REQUIRE( discovered.error.isEmpty() );
    REQUIRE( discovered.instances.size() == 1 );
    CHECK( discovered.instances.front().instanceId == live.descriptor().instanceId );
    // Exercise the CLI default selector and real transport. Only the calling
    // thread differs from a separate CLI so this test's GUI thread can serve.
    CliServices cli;
    cli.call = invoke;
    QBuffer input;
    REQUIRE( input.open( QIODevice::ReadOnly ) );
    CHECK( runControlCli( { "state", "get" }, input, "test", cli, options ).exit == CliExit::Success );
}

TEST_CASE( "Control discovery isolates installation and profile identities", "[controltransport]" )
{
    QTemporaryDir directory;
    ControlServer first;
    ControlServer second;
    auto other = roots( directory );
    other.installationId = "another-installation";
    REQUIRE( first.start( roots( directory ), okay ) );
    REQUIRE( second.start( other, okay ) );
    CHECK( discoverInstances( roots( directory ) ).instances.size() == 1 );
    auto all = roots( directory );
    all.installationId.clear();
    CHECK( discoverInstances( all ).instances.size() == 2 );
    first.stop();
    CHECK( discoverInstances( other ).instances.size() == 1 );
}

TEST_CASE( "A valid copy cannot impersonate a published control epoch or authorize reclamation",
           "[controltransport][control-abandoned]" )
{
    QTemporaryDir directory;
    const auto options = roots( directory );
    ControlServer original;
    REQUIRE( original.start( options, okay ) );
    const auto path = original.descriptorPath();
    REQUIRE( QFile::rename( path, path + ".original" ) );
    original.stop();
    REQUIRE( QFile::copy( path + ".original", path ) );
    ControlServer replacement;
    REQUIRE( replacement.start( options, okay ) );
    const auto found = discoverInstances( options );
    REQUIRE( found.error.isEmpty() );
    REQUIRE( found.instances.size() == 1 );
    CHECK( found.instances.front().instanceId == replacement.descriptor().instanceId );
    CHECK( found.rejectedDescriptors == 1 );
    CHECK( QFileInfo::exists( path ) );
}

TEST_CASE( "An overfull discovery registry fails closed without deleting unknown files",
           "[controltransport][control-abandoned]" )
{
    QTemporaryDir directory;
    const auto options = roots( directory );
    ControlServer live;
    REQUIRE( live.start( options, okay ) );
    const auto registry = QFileInfo( live.descriptorPath() ).absoluteDir();
    for ( int index = 0; index < 515; ++index ) {
        QFile unknown( registry.filePath( QString::number( index ) ) );
        REQUIRE( unknown.open( QIODevice::WriteOnly | QIODevice::NewOnly ) );
    }
    const auto found = discoverInstances( options );
    CHECK_FALSE( found.error.isEmpty() );
    CHECK( found.instances.empty() );
    ControlServer another;
    CHECK_FALSE( another.start( options, okay ) );
    CHECK( registry.entryList( QDir::Files | QDir::Hidden ).size() == 516 );
}

TEST_CASE( "Control resource cleanup does not remove replacement descriptors", "[controltransport]" )
{
    QTemporaryDir directory;
    ControlServer server;
    REQUIRE( server.start( roots( directory ), okay ) );
    const auto path = server.descriptorPath();
    REQUIRE( QFile::rename( path, path + ".original" ) );
    QFile replacement( path );
    REQUIRE( replacement.open( QIODevice::WriteOnly ) );
    REQUIRE( replacement.write( "replacement" ) == 11 );
    replacement.close();
    server.stop();
    CHECK( QFileInfo::exists( path ) );
    CHECK( QFileInfo::exists( path + ".original" ) );
}

TEST_CASE( "Failed Unix bind never claims ownership of a preexisting endpoint", "[controltransport]" )
{
    if ( QSysInfo::kernelType() == QStringLiteral( "winnt" ) ) return;
    QTemporaryDir directory;
    ControlRuntime runtime;
    REQUIRE( runtime.prepare( roots( directory ), nullptr ) );
    const auto endpoint = runtime.descriptor().endpoint;
    QFile replacement( endpoint );
    REQUIRE( replacement.open( QIODevice::WriteOnly ) );
    REQUIRE( replacement.write( "unowned" ) == 7 );
    replacement.close();
    CHECK( runtime.createListener( nullptr ) == -1 );
    runtime.clear();
    CHECK( QFileInfo::exists( endpoint ) );
    REQUIRE( QFile::remove( endpoint ) );
    REQUIRE( QDir().rmdir( QFileInfo( endpoint ).path() ) );
}

TEST_CASE( "macOS extended ACL grants cannot bypass private control mode bits", "[controltransport]" )
{
    if ( QSysInfo::kernelType() != QStringLiteral( "darwin" ) ) return;
    QTemporaryDir directory;
    const auto options = roots( directory );
    ControlServer server;
    REQUIRE( server.start( options, okay ) );
    SECTION( "directory ACL grants" ) {
        server.stop();
        REQUIRE( QProcess::execute( QStringLiteral( "/bin/chmod" ),
                                    { "+a", "everyone allow list,search", options.descriptorRoot } ) == 0 );
        CHECK_FALSE( server.start( options, okay ) );
        CHECK_FALSE( discoverInstances( options ).error.isEmpty() );
        REQUIRE( QProcess::execute( QStringLiteral( "/bin/chmod" ), { "-N", options.descriptorRoot } ) == 0 );
    }
    SECTION( "descriptor ACL grants" ) {
        const auto path = server.descriptorPath();
        REQUIRE( QProcess::execute( QStringLiteral( "/bin/chmod" ), { "+a", "everyone allow read", path } ) == 0 );
        CHECK( discoverInstances( options ).rejectedDescriptors == 1 );
        REQUIRE( QProcess::execute( QStringLiteral( "/bin/chmod" ), { "-N", path } ) == 0 );
    }
}

TEST_CASE( "Unix control roots and descriptors reject insecure modes and links", "[controltransport]" )
{
    if ( QSysInfo::kernelType() == QStringLiteral( "winnt" ) ) return;
    QTemporaryDir directory;
    const auto options = roots( directory );
    ControlServer server;
    REQUIRE( server.start( options, okay ) );
    const auto path = server.descriptorPath();
    const auto endpoint = server.descriptor().endpoint;
    const auto privateMode = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    CHECK( ( QFileInfo( options.descriptorRoot ).permissions() & ( QFile::ReadOther | QFile::WriteOther | QFile::ExeOther ) ) == 0 );
    CHECK( ( QFileInfo( path ).permissions() & ( QFile::ReadOther | QFile::WriteOther ) ) == 0 );
    CHECK( ( QFileInfo( QFileInfo( endpoint ).path() ).permissions() & ( QFile::ReadOther | QFile::WriteOther | QFile::ExeOther ) ) == 0 );
    SECTION( "public root fails closed without repairing permissions" ) {
        server.stop();
        REQUIRE( QFile::setPermissions( options.descriptorRoot, privateMode | QFile::ReadOther | QFile::ExeOther ) );
        CHECK_FALSE( server.start( options, okay ) );
        CHECK_FALSE( discoverInstances( options ).error.isEmpty() );
        CHECK( QFileInfo( options.descriptorRoot ).permission( QFile::ReadOther ) );
    }
    SECTION( "symlink root fails closed" ) {
        server.stop();
        const auto moved = options.descriptorRoot + ".moved";
        REQUIRE( QDir().rename( options.descriptorRoot, moved ) );
        REQUIRE( QFile::link( moved, options.descriptorRoot ) );
        CHECK_FALSE( server.start( options, okay ) );
        CHECK_FALSE( discoverInstances( options ).error.isEmpty() );
    }
    SECTION( "world-readable credential is never discovered" ) {
        REQUIRE( QFile::setPermissions( path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther ) );
        CHECK( discoverInstances( options ).rejectedDescriptors == 1 );
    }
    SECTION( "descriptor link is never followed" ) {
        REQUIRE( QFile::rename( path, path + ".moved" ) );
        REQUIRE( QFile::link( path + ".moved", path ) );
        CHECK( discoverInstances( options ).rejectedDescriptors == 1 );
        server.stop();
        CHECK( QFileInfo( path ).isSymLink() );
    }
    SECTION( "socket replacement survives stop" ) {
        REQUIRE( QFile::rename( endpoint, endpoint + ".moved" ) );
        QFile replacement( endpoint );
        REQUIRE( replacement.open( QIODevice::WriteOnly ) );
        replacement.close();
        server.stop();
        CHECK( QFileInfo::exists( endpoint ) );
        REQUIRE( QFile::remove( endpoint ) );
        REQUIRE( QFile::remove( endpoint + ".moved" ) );
        REQUIRE( QDir().rmdir( QFileInfo( endpoint ).path() ) );
    }
}
