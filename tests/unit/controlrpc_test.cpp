#include <catch2/catch.hpp>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include "controlrpc.h"
#include "controlframing.h"

using namespace klogg::control;

TEST_CASE( "Control RPC preserves string IDs and 64-bit string parameters", "[control-rpc]" )
{
    const auto decoded = decodeRequest( R"({"jsonrpc":"2.0","id":"request-1","method":"source.read","params":{"line":"18446744073709551615"}})" );
    REQUIRE( std::holds_alternative<RpcRequest>( decoded ) );
    const auto& request = std::get<RpcRequest>( decoded );
    CHECK( request.id == QStringLiteral( "request-1" ) );
    CHECK( request.method == QStringLiteral( "source.read" ) );
    CHECK( request.params.value( QStringLiteral( "line" ) ).toString()
           == QStringLiteral( "18446744073709551615" ) );
    const auto noParams = decodeRequest( R"({"jsonrpc":"2.0","id":"x","method":"state.get"})" );
    REQUIRE( std::holds_alternative<RpcRequest>( noParams ) );
    CHECK( std::get<RpcRequest>( noParams ).params.isEmpty() );
    const auto withBom = decodeRequest( QByteArray::fromHex( "efbbbf" )
        + R"({"jsonrpc":"2.0","id":"x","method":"state.get"})" );
    CHECK( std::holds_alternative<RpcRequest>( withBom ) );
}

TEST_CASE( "Control RPC rejects malformed requests instead of coercing parameters",
           "[control-rpc]" )
{
    const auto payload = GENERATE(
        QByteArray( "[" ), QByteArray( "[]" ), QByteArray( "null" ),
        QByteArray( R"({"jsonrpc":"1.0","id":"x","method":"tab.list"})" ),
        QByteArray( R"({"jsonrpc":"2.0","id":42,"method":"tab.list"})" ),
        QByteArray( R"({"jsonrpc":"2.0","id":"","method":"tab.list"})" ),
        QByteArray( R"({"jsonrpc":"2.0","id":"x","method":false})" ),
        QByteArray( R"({"jsonrpc":"2.0","id":"x","method":"tab.list","params":[]})" ),
        QByteArray( R"({"jsonrpc":"2.0","id":"x","method":"tab.list","params":null})" ) );
    const auto decoded = decodeRequest( payload );
    REQUIRE( std::holds_alternative<RpcError>( decoded ) );
    CHECK_FALSE( std::get<RpcError>( decoded ).remote );
}

TEST_CASE( "Notifications cannot execute a control mutation or generate a reply",
           "[control-rpc]" )
{
    const auto decoded = decodeRequest( R"({"jsonrpc":"2.0","method":"transaction.apply","params":{}})" );
    REQUIRE( std::holds_alternative<RpcError>( decoded ) );
    const auto& error = std::get<RpcError>( decoded );
    CHECK_FALSE( error.replyAllowed );
    CHECK_FALSE( encodeError( error ) );
}

TEST_CASE( "Control RPC uses standard parse and parameter error envelopes", "[control-rpc]" )
{
    const auto broken = decodeRequest( "{" );
    REQUIRE( std::holds_alternative<RpcError>( broken ) );
    const auto& parseError = std::get<RpcError>( broken );
    CHECK( parseError.code == -32700 );
    CHECK( parseError.id.isNull() );

    const auto badParams = decodeRequest( R"({"jsonrpc":"2.0","id":"x","method":"tab.list","params":[]})" );
    REQUIRE( std::holds_alternative<RpcError>( badParams ) );
    const auto& paramsError = std::get<RpcError>( badParams );
    CHECK( paramsError.code == -32602 );
    CHECK( paramsError.id.toString() == QStringLiteral( "x" ) );
    const auto encoded = encodeError( paramsError );
    REQUIRE( encoded );
    const auto response = QJsonDocument::fromJson( *encoded ).object();
    CHECK( response.value( "jsonrpc" ).toString() == QStringLiteral( "2.0" ) );
    CHECK( response.contains( "error" ) );
    CHECK_FALSE( response.contains( "result" ) );
}

TEST_CASE( "Control RPC response decoding validates correlation and exclusive outcomes",
           "[control-rpc]" )
{
    const auto success = encodeSuccess( QStringLiteral( "x" ), QJsonValue( QJsonValue::Null ) );
    REQUIRE( success );
    const auto decoded = decodeResponse( *success, QStringLiteral( "x" ) );
    REQUIRE( std::holds_alternative<QJsonValue>( decoded ) );
    CHECK( std::get<QJsonValue>( decoded ).isNull() );
    CHECK_FALSE( encodeSuccess( QStringLiteral( "x" ), QJsonValue( QJsonValue::Undefined ) ) );

    const auto malformed = GENERATE(
        QByteArray( R"({"jsonrpc":"2.0","id":"wrong","result":{}})" ),
        QByteArray( R"({"jsonrpc":"2.0","id":"x","result":{},"error":{"code":-1,"message":"bad"}})" ),
        QByteArray( R"({"jsonrpc":"2.0","id":"x"})" ),
        QByteArray( R"({"jsonrpc":"2.0","id":"x","error":{"code":1.5,"message":"bad"}})" ),
        QByteArray( R"({"jsonrpc":"2.0","id":"x","error":{"code":1e30,"message":"bad"}})" ),
        QByteArray( R"({"jsonrpc":"2.0","id":"x","error":{"code":-1,"message":false}})" ) );
    const auto rejected = decodeResponse( malformed, QStringLiteral( "x" ) );
    REQUIRE( std::holds_alternative<RpcError>( rejected ) );
    CHECK_FALSE( std::get<RpcError>( rejected ).remote );
}

TEST_CASE( "Control RPC preserves remote error data without interpreting it as commands",
           "[control-rpc]" )
{
    const auto decoded = decodeResponse(
        R"({"jsonrpc":"2.0","id":"x","error":{"code":-32000,"message":"Conflict","data":{"code":"REVISION_CONFLICT","revision":"18446744073709551615"}}})",
        QStringLiteral( "x" ) );
    REQUIRE( std::holds_alternative<RpcError>( decoded ) );
    const auto& error = std::get<RpcError>( decoded );
    CHECK( error.remote );
    CHECK( error.code == -32000 );
    CHECK( error.data.toObject().value( "revision" ).toString()
           == QStringLiteral( "18446744073709551615" ) );
    const auto encoded = encodeError( error );
    REQUIRE( encoded );
    CHECK( std::get<RpcError>( decodeResponse( *encoded, "x" ) ).data == error.data );
}

TEST_CASE( "Malformed objects are not mistaken for silent notifications", "[control-rpc]" )
{
    const auto payload = GENERATE( QByteArray( "{}" ),
        QByteArray( R"({"jsonrpc":"1.0","method":"state.get"})" ) );
    const auto decoded = decodeRequest( payload );
    REQUIRE( std::holds_alternative<RpcError>( decoded ) );
    const auto& error = std::get<RpcError>( decoded );
    CHECK( error.code == -32600 );
    CHECK( error.replyAllowed );
    CHECK( encodeError( error ).has_value() );
}

TEST_CASE( "Valid JSON scalar roots are invalid RPC requests rather than parse errors",
           "[control-rpc]" )
{
    const auto payload = GENERATE( QByteArray( "null" ), QByteArray( "true" ),
                                 QByteArray( "42" ), QByteArray( "\"text\"" ) );
    const auto decoded = decodeRequest( payload );
    REQUIRE( std::holds_alternative<RpcError>( decoded ) );
    CHECK( std::get<RpcError>( decoded ).code == -32600 );
}

TEST_CASE( "Control RPC bounds payloads and identifiers at both serialization boundaries",
           "[control-rpc]" )
{
    const auto oversized = decodeRequest( QByteArray( static_cast<int>( MaxFrameBytes ) + 1, ' ' ) );
    REQUIRE( std::holds_alternative<RpcError>( oversized ) );
    CHECK( std::get<RpcError>( oversized ).data.toObject().value( "code" ).toString()
           == QStringLiteral( "RESOURCE_LIMIT" ) );
    CHECK_FALSE( encodeSuccess( QString( 129, QLatin1Char( 'x' ) ), QJsonObject{} ) );
    CHECK_FALSE( encodeSuccess( "id", QString( static_cast<int>( MaxFrameBytes ), QLatin1Char( 'x' ) ) ) );
    RpcError large;
    large.message = QString( static_cast<int>( MaxFrameBytes ), QLatin1Char( 'x' ) );
    CHECK_FALSE( encodeError( large ) );
}
