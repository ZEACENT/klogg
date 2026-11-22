#include "controltransport.h"
#include "controlframing.h"
#include "controlruntime_pal.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <variant>

namespace klogg::control {
namespace {
constexpr qint64 ReadChunk = 4096;
constexpr qint64 SocketBuffer = MaxFrameBytes + 4;

QByteArray json( const QJsonObject& object )
{
    return QJsonDocument( object ).toJson( QJsonDocument::Compact );
}
QByteArray hello( const InstanceDescriptor& descriptor, bool credential )
{
    QJsonObject value{ { QStringLiteral( "type" ), QStringLiteral( "hello" ) },
                       { QStringLiteral( "protocol" ), ControlProtocolVersion },
                       { QStringLiteral( "instance" ), descriptor.instanceId } };
    if ( credential ) value.insert( QStringLiteral( "token" ), descriptor.token );
    return json( value );
}
bool matchesHello( const QByteArray& payload, const InstanceDescriptor& descriptor, bool credential )
{
    if ( payload.size() > 1024 ) return false;
    const auto value = QJsonDocument::fromJson( payload ).object();
    return value.value( "type" ) == QJsonValue( QStringLiteral( "hello" ) )
           && value.value( "protocol" ) == QJsonValue( ControlProtocolVersion )
           && value.value( "instance" ) == QJsonValue( descriptor.instanceId )
           && ( !credential || value.value( "token" ) == QJsonValue( descriptor.token ) );
}
RpcError internalError( const QString& requestId )
{
    return { requestId, -32603, QStringLiteral( "Control request failed" ),
             QJsonObject{ { QStringLiteral( "code" ), QStringLiteral( "INTERNAL_ERROR" ) } }, true, false };
}
QByteArray requestBytes( const RpcRequest& request )
{
    return json( { { QStringLiteral( "jsonrpc" ), QStringLiteral( "2.0" ) },
                  { QStringLiteral( "id" ), request.id },
                  { QStringLiteral( "method" ), request.method },
                  { QStringLiteral( "params" ), request.params } } );
}
} // namespace

struct ControlServer::Private : std::enable_shared_from_this<ControlServer::Private> {
    struct Session;
    ControlRuntime runtime;
    pal::LocalListener listener;
    Handler handler;
    TransportLimits limits;
    bool running = false;
    std::unordered_map<Session*, std::shared_ptr<Session>> sessions;

    void accept();
    void stop();
};

struct ControlServer::Private::Session : std::enable_shared_from_this<Session> {
    std::weak_ptr<Private> owner;
    std::unique_ptr<QLocalSocket> socket;
    QTimer deadline;
    FrameDecoder decoder;
    bool authenticated = false;
    bool pumping = false;
    bool closed = false;

    Session( const std::shared_ptr<Private>& state, QLocalSocket* connection )
        : owner( state ), socket( connection )
    {
        socket->setParent( nullptr );
        socket->setReadBufferSize( SocketBuffer );
    }
    ~Session()
    {
        // QLocalSocket emits disconnected from its own read-notifier stack.
        // Defer QObject destruction until that stack has unwound.
        socket.release()->deleteLater();
    }
    void close()
    {
        if ( closed ) return;
        closed = true;
        deadline.stop();
        socket->abort();
        if ( const auto state = owner.lock() ) state->sessions.erase( this );
    }
    void arm()
    {
        const auto weak = weak_from_this();
        QObject::connect( socket.get(), &QLocalSocket::readyRead, socket.get(), [weak] {
            if ( const auto self = weak.lock() ) self->pump();
        } );
        QObject::connect( socket.get(), &QLocalSocket::disconnected, socket.get(), [weak] {
            if ( const auto self = weak.lock() ) self->close();
        } );
        deadline.setSingleShot( true );
        deadline.setTimerType( Qt::PreciseTimer );
        QObject::connect( &deadline, &QTimer::timeout, socket.get(), [weak] {
            if ( const auto self = weak.lock() ) self->close();
        } );
        if ( const auto state = owner.lock() ) deadline.start( state->limits.handshakeTimeoutMs );
        schedule();
    }
    void schedule()
    {
        const auto weak = weak_from_this();
        QTimer::singleShot( 0, Qt::PreciseTimer, socket.get(), [weak] {
            if ( const auto self = weak.lock() ) self->pump();
        } );
    }
    bool send( const std::optional<QByteArray>& payload )
    {
        const auto state = owner.lock();
        const auto frame = payload ? encodeFrame( *payload ) : std::nullopt;
        if ( closed || !state || !state->running || !frame
             || socket->bytesToWrite() + frame->size() > state->limits.maxQueuedWriteBytes
             || socket->write( *frame ) != frame->size() ) {
            close();
            return false;
        }
        return true;
    }
    void pump()
    {
        const auto state = owner.lock();
        if ( pumping || closed || !state || !state->running ) return;
        pumping = true;
        const auto result = decoder.feed( socket->read( ReadChunk ) );
        if ( result.error != FrameError::None ) { close(); return; }
        for ( const auto& payload : result.messages ) {
            if ( closed || !state->running ) break;
            if ( !authenticated ) {
                if ( !pal::sameUserPeer( socket->socketDescriptor(), true )
                     || !matchesHello( payload, state->runtime.descriptor(), true ) ) { close(); break; }
                authenticated = true;
                deadline.stop();
                if ( !send( hello( state->runtime.descriptor(), false ) ) ) break;
                continue;
            }
            const auto decoded = decodeRequest( payload );
            if ( const auto* error = std::get_if<RpcError>( &decoded ) ) {
                if ( error->replyAllowed && !send( encodeError( *error ) ) ) break;
                continue;
            }
            const auto& request = std::get<RpcRequest>( decoded );
            DecodedResponse response = internalError( request.id );
            // Keep a local handler copy and shared state/session lifetimes: the
            // callback can stop, restart, or destroy the public server object.
            const auto callback = state->handler;
            try {
                response = callback( request );
            }
            catch ( ... ) {
                response = internalError( request.id );
            }
            if ( closed || !state->running ) break;
            std::optional<QByteArray> encoded;
            if ( const auto* error = std::get_if<RpcError>( &response ) ) {
                auto correlated = *error;
                correlated.id = request.id;
                correlated.replyAllowed = true;
                encoded = encodeError( correlated );
            }
            else encoded = encodeSuccess( request.id, std::get<QJsonValue>( response ) );
            if ( !encoded ) encoded = encodeError( internalError( request.id ) );
            if ( !send( encoded ) ) break;
        }
        pumping = false;
        if ( !closed && state->running && socket->bytesAvailable() > 0 ) schedule();
    }
};

void ControlServer::Private::accept()
{
    // Bound both live sessions and the work done by one incoming signal.
    int accepted = 0;
    while ( running && listener.hasPendingConnections() && accepted++ < limits.maxConnections ) {
        auto* socket = listener.nextPendingConnection();
        if ( !socket ) break;
        if ( sessions.size() >= static_cast<std::size_t>( limits.maxConnections ) ) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        auto session = std::make_shared<Session>( shared_from_this(), socket );
        sessions.emplace( session.get(), session );
        session->arm();
    }
    if ( running && listener.hasPendingConnections() ) {
        const auto weak = weak_from_this();
        QTimer::singleShot( 0, Qt::PreciseTimer, &listener, [weak] {
            if ( const auto state = weak.lock() ) state->accept();
        } );
    }
}
void ControlServer::Private::stop()
{
    running = false;
    listener.close();
    const auto connections = std::move( sessions );
    for ( const auto& item : connections ) item.second->close();
    handler = {};
    runtime.clear();
}
ControlServer::ControlServer( QObject* parent ) : QObject( parent ) {}
ControlServer::~ControlServer() { stop(); }
bool ControlServer::start( const RuntimeOptions& options, Handler handler, QString* error, TransportLimits limits )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    stop();
    if ( error ) error->clear();
    if ( !handler || limits.maxConnections < 1 || limits.maxConnections > 256
         || limits.handshakeTimeoutMs < 1 || limits.handshakeTimeoutMs > 60000
         || limits.maxQueuedWriteBytes < 1024 || limits.maxQueuedWriteBytes > 8 * SocketBuffer ) {
        if ( error ) *error = QStringLiteral( "Invalid control transport limits" );
        return false;
    }
    auto state = std::make_shared<Private>();
    state->limits = limits;
    state->handler = std::move( handler );
    if ( !state->runtime.prepare( options, error ) ) return false;
    state->listener.setMaxPendingConnections( limits.maxConnections );
    const auto descriptor = state->runtime.createListener( error );
    if ( !state->listener.open( state->runtime.descriptor().endpoint, descriptor ) ) {
        ControlRuntime::closeListener( descriptor );
        if ( error && error->isEmpty() ) *error = QStringLiteral( "Cannot listen on control endpoint" );
        return false;
    }
    if ( !state->runtime.publish( error ) ) { state->stop(); return false; }
    state->running = true;
    const std::weak_ptr<Private> weak = state;
    QObject::connect( &state->listener, &QLocalServer::newConnection, &state->listener, [weak] {
        if ( const auto active = weak.lock() ) active->accept();
    } );
    d_ = std::move( state );
    return true;
}
void ControlServer::stop()
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( d_ ) d_->stop();
    d_.reset();
}
bool ControlServer::isRunning() const { return d_ && d_->running; }
std::size_t ControlServer::connectionCount() const { return d_ ? d_->sessions.size() : 0; }
InstanceDescriptor ControlServer::descriptor() const { return d_ ? d_->runtime.descriptor() : InstanceDescriptor{}; }
QString ControlServer::descriptorPath() const { return d_ ? d_->runtime.descriptorPath() : QString{}; }

namespace {
class BlockingExchange {
  public:
    QLocalSocket socket;
    QElapsedTimer clock;
    FrameDecoder decoder;
    int timeout;
    explicit BlockingExchange( int timeoutMs ) : timeout( timeoutMs )
    {
        clock.start();
        socket.setReadBufferSize( SocketBuffer );
    }
    int remaining() const
    {
        return static_cast<int>( std::max<qint64>( 0, timeout - clock.elapsed() ) );
    }
    bool send( const QByteArray& frame )
    {
        if ( remaining() <= 0 || socket.write( frame ) != frame.size() ) return false;
        while ( socket.bytesToWrite() > 0 ) {
            if ( remaining() <= 0 || !socket.waitForBytesWritten( remaining() ) ) return false;
        }
        return true;
    }
    std::optional<QByteArray> receive()
    {
        while ( remaining() > 0 ) {
            if ( socket.bytesAvailable() == 0 && !socket.waitForReadyRead( remaining() ) ) return {};
            const auto frames = decoder.feed( socket.read( ReadChunk ) );
            if ( frames.error != FrameError::None || frames.messages.size() > 1 ) return {};
            if ( !frames.messages.empty() ) return frames.messages.front();
        }
        return {};
    }
};
} // namespace
ClientResult callLocal( const InstanceDescriptor& instance, const RpcRequest& request, int timeoutMs )
{
    ClientResult result;
    result.error = QStringLiteral( "Control request was not sent" );
    if ( timeoutMs < 1 || instance.descriptorVersion != 1
         || instance.protocolMin > ControlProtocolVersion || instance.protocolMax < ControlProtocolVersion
         || instance.instanceId.size() != 32 || instance.token.size() != 64
         || instance.endpoint.isEmpty() || instance.endpoint.contains( QChar( 0 ) )
         || !pal::privateEndpoint( instance.endpoint, instance.instanceId ) ) return result;
    const auto bytes = requestBytes( request );
    if ( !std::holds_alternative<RpcRequest>( decodeRequest( bytes ) ) ) return result;
    const auto requestFrame = encodeFrame( bytes );
    const auto helloFrame = encodeFrame( hello( instance, true ) );
    if ( !requestFrame || !helloFrame ) return result;
    BlockingExchange exchange( timeoutMs );
    exchange.socket.connectToServer( instance.endpoint );
    if ( exchange.remaining() <= 0 || !exchange.socket.waitForConnected( exchange.remaining() )
         || !pal::sameUserPeer( exchange.socket.socketDescriptor(), false )
         || !exchange.send( *helloFrame ) ) return result;
    const auto greeting = exchange.receive();
    if ( !greeting || !matchesHello( *greeting, instance, false ) || exchange.remaining() <= 0 ) return result;
    // Once submitted, even a write failure may have delivered a prefix/full
    // mutation. Never replay; only a correlated response resolves ambiguity.
    result.status = ClientStatus::OutcomeUnknown;
    result.error = QStringLiteral( "Control request outcome is unknown; do not blindly retry" );
    if ( !exchange.send( *requestFrame ) ) return result;
    const auto response = exchange.receive();
    if ( !response ) return result;
    auto decoded = decodeResponse( *response, request.id );
    if ( const auto* error = std::get_if<RpcError>( &decoded ) ) {
        if ( !error->remote ) return result;
        result.status = ClientStatus::Rejected;
    }
    else result.status = ClientStatus::Success;
    result.response = std::move( decoded );
    result.error.clear();
    return result;
}
} // namespace klogg::control
