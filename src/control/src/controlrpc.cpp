#include "controlrpc.h"

#include <cmath>
#include <limits>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

#include "controlframing.h"

namespace klogg::control {
namespace {
constexpr int MaxIdentifierUnits = 128;

bool validId( const QString& id )
{
    return !id.isEmpty() && id.size() <= MaxIdentifierUnits;
}

RpcError failure( int code, const QString& message, const QString& symbol,
                  QJsonValue id = QJsonValue( QJsonValue::Null ), bool replyAllowed = true )
{
    return { std::move( id ), code, message,
             QJsonObject{ { QStringLiteral( "code" ), symbol } }, replyAllowed, false };
}

std::variant<QJsonObject, RpcError> parseObject( const QByteArray& payload )
{
    if ( static_cast<uint64_t>( payload.size() ) > MaxFrameBytes ) {
        return failure( -32000, QStringLiteral( "Control message exceeds its size limit" ),
                        QStringLiteral( "RESOURCE_LIMIT" ) );
    }
    // QJsonDocument cannot represent scalar roots on Qt 5. Parse one value
    // through an array so valid JSON scalars remain Invalid Request (-32600),
    // rather than being misreported as invalid JSON (-32700).
    QByteArray wrapped( "[" );
    wrapped.reserve( payload.size() + 2 );
    if ( payload.startsWith( "\xEF\xBB\xBF" ) ) {
        wrapped.append( payload.constData() + 3, payload.size() - 3 );
    }
    else {
        wrapped.append( payload );
    }
    wrapped.append( ']' );
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson( wrapped, &parseError );
    if ( parseError.error != QJsonParseError::NoError || document.array().size() != 1 ) {
        return failure( -32700, QStringLiteral( "Invalid JSON" ), QStringLiteral( "PARSE_ERROR" ) );
    }
    const auto root = document.array().first();
    if ( !root.isObject() ) {
        return failure( -32600, QStringLiteral( "A single JSON-RPC object is required" ),
                        QStringLiteral( "INVALID_REQUEST" ) );
    }
    return root.toObject();
}

std::optional<QByteArray> serialize( const QJsonObject& response )
{
    auto bytes = QJsonDocument( response ).toJson( QJsonDocument::Compact );
    if ( static_cast<uint64_t>( bytes.size() ) > MaxFrameBytes ) {
        return {};
    }
    return bytes;
}
} // namespace

DecodedRequest decodeRequest( const QByteArray& payload )
{
    auto parsed = parseObject( payload );
    if ( const auto* error = std::get_if<RpcError>( &parsed ) ) {
        return *error;
    }
    const auto& object = std::get<QJsonObject>( parsed );
    const auto id = object.value( QStringLiteral( "id" ) );
    const auto method = object.value( QStringLiteral( "method" ) );
    // A notification must never get an error response either. The application
    // dispatcher accepts only RpcRequest, so it cannot execute this rejection.
    const bool supportedVersion
        = object.value( QStringLiteral( "jsonrpc" ) ).toString() == QStringLiteral( "2.0" );
    const bool validMethod = method.isString() && !method.toString().isEmpty()
                             && method.toString().size() <= MaxIdentifierUnits;
    const bool notification = !object.contains( QStringLiteral( "id" ) )
                              && supportedVersion && validMethod;
    const auto responseId = id.isString() && validId( id.toString() )
                                ? id : QJsonValue( QJsonValue::Null );
    if ( !supportedVersion || !id.isString() || !validId( id.toString() ) || !validMethod ) {
        return failure( -32600, QStringLiteral( "Expected JSON-RPC 2.0, string id, and method" ),
                        QStringLiteral( "INVALID_REQUEST" ), responseId, !notification );
    }
    const auto params = object.value( QStringLiteral( "params" ) );
    if ( !params.isUndefined() && !params.isObject() ) {
        return failure( -32602, QStringLiteral( "Control parameters must be a named object" ),
                        QStringLiteral( "INVALID_PARAMS" ), id );
    }
    return RpcRequest{ id.toString(), method.toString(), params.toObject() };
}

DecodedResponse decodeResponse( const QByteArray& payload, const QString& expectedId )
{
    auto parsed = parseObject( payload );
    if ( auto* error = std::get_if<RpcError>( &parsed ) ) {
        error->id = expectedId;
        return *error;
    }
    const auto invalid = [ &expectedId ] {
        return failure( -32600, QStringLiteral( "Invalid or uncorrelated JSON-RPC response" ),
                        QStringLiteral( "INVALID_RESPONSE" ), expectedId );
    };
    const auto& object = std::get<QJsonObject>( parsed );
    const auto id = object.value( QStringLiteral( "id" ) );
    if ( !validId( expectedId ) || !id.isString() || id.toString() != expectedId
         || object.value( QStringLiteral( "jsonrpc" ) ).toString() != QStringLiteral( "2.0" )
         || object.contains( QStringLiteral( "result" ) )
                == object.contains( QStringLiteral( "error" ) ) ) {
        return invalid();
    }
    if ( object.contains( QStringLiteral( "result" ) ) ) {
        return object.value( QStringLiteral( "result" ) );
    }
    const auto errorValue = object.value( QStringLiteral( "error" ) );
    if ( !errorValue.isObject() ) {
        return invalid();
    }
    const auto error = errorValue.toObject();
    const auto codeValue = error.value( QStringLiteral( "code" ) );
    const auto code = codeValue.toDouble();
    if ( !codeValue.isDouble() || !std::isfinite( code ) || std::trunc( code ) != code
         || code < std::numeric_limits<int>::min() || code > std::numeric_limits<int>::max()
         || !error.value( QStringLiteral( "message" ) ).isString() ) {
        return invalid();
    }
    return RpcError{ id, static_cast<int>( code ),
                     error.value( QStringLiteral( "message" ) ).toString(),
                     error.value( QStringLiteral( "data" ) ), true, true };
}

std::optional<QByteArray> encodeSuccess( const QString& id, const QJsonValue& result )
{
    if ( !validId( id ) || result.isUndefined() ) {
        return {};
    }
    return serialize( { { QStringLiteral( "jsonrpc" ), QStringLiteral( "2.0" ) },
                        { QStringLiteral( "id" ), id }, { QStringLiteral( "result" ), result } } );
}

std::optional<QByteArray> encodeError( const RpcError& error )
{
    if ( !error.replyAllowed || ( !error.id.isNull()
                                 && ( !error.id.isString() || !validId( error.id.toString() ) ) ) ) {
        return {};
    }
    QJsonObject detail{ { QStringLiteral( "code" ), error.code },
                        { QStringLiteral( "message" ), error.message } };
    if ( !error.data.isUndefined() ) {
        detail.insert( QStringLiteral( "data" ), error.data );
    }
    return serialize( { { QStringLiteral( "jsonrpc" ), QStringLiteral( "2.0" ) },
                        { QStringLiteral( "id" ), error.id },
                        { QStringLiteral( "error" ), detail } } );
}

} // namespace klogg::control
