#include <catch2/catch.hpp>

#include <QJsonArray>
#include "controlschema.h"

using namespace klogg::control;

namespace {
QJsonObject lookup( const QString& method )
{
    const QString requestId( 128, QLatin1Char( 'x' ) ); // Even the largest request ID must fit.
    const auto response = methodSchema( { requestId, "schema.get", { { "method", method } } } );
    REQUIRE( std::holds_alternative<QJsonValue>( response ) );
    const auto value = std::get<QJsonValue>( response );
    REQUIRE( encodeSuccess( requestId, value ) );
    const auto schema = value.toObject();
    CHECK( schema.value( "method" ).toString() == method );
    CHECK_FALSE( schema.value( "description" ).toString().isEmpty() );
    CHECK( schema.value( "params" ).isObject() );
    CHECK( schema.value( "result" ).isObject() );
    return schema;
}
}

TEST_CASE( "Schema lookup returns one method with named parameters and wire types", "[control-schema]" )
{
    const auto schema = lookup( "source.read" );
    const auto params = schema.value( "params" ).toObject();
    CHECK( params.value( "additionalProperties" ) == false );
    const auto fields = params.value( "properties" ).toObject();
    CHECK( fields.value( "count" ).toObject().value( "type" ) == "integer" );
    CHECK( fields.value( "start" ).toObject().value( "type" ) == "string" );
    CHECK( fields.value( "transaction_id" ).toObject().value( "type" ) == "string" );
    const auto required = params.value( "required" ).toArray();
    CHECK( required.contains( "snapshot_id" ) );
    CHECK( required.contains( "start" ) );
    CHECK( required.contains( "count" ) );
    CHECK_FALSE( required.contains( "transaction_id" ) );
    const auto result = schema.value( "result" ).toObject().value( "properties" ).toObject();
    CHECK( result.contains( "operation_id" ) );
    CHECK_FALSE( result.contains( "rows" ) );
}

TEST_CASE( "Every method named by discovery has a frame-bounded individual contract", "[control-schema]" )
{
    const auto params = lookup( "schema.get" ).value( "params" ).toObject();
    const auto names = params.value( "properties" ).toObject().value( "method" ).toObject().value( "enum" ).toArray();
    REQUIRE( names.contains( "schema.get" ) );
    REQUIRE( names.contains( "operation.cancel" ) ); // Can return the largest, content-bearing result shape.
    for ( const auto& name : names ) {
        CAPTURE( name.toString().toStdString() );
        const auto schema = lookup( name.toString() );
        CHECK( schema.size() == 4 ); // No registry, envelope, or unrelated method data.
        CHECK( schema.value( "params" ).toObject().value( "additionalProperties" ) == false );
    }
}

TEST_CASE( "Schema discovery distinguishes explicit staging from direct navigation", "[control-schema]" )
{
    for ( const auto* method : { "documents.open", "documents.close", "filter.apply", "operation.get", "source.capture", "mark.add" } ) {
        CAPTURE( method );
        const auto params = lookup( method ).value( "params" ).toObject();
        CHECK( params.value( "properties" ).toObject().contains( "transaction_id" ) );
        CHECK_FALSE( params.value( "required" ).toArray().contains( "transaction_id" ) );
    }
    const auto navigate = lookup( "pane.navigate" );
    const auto params = navigate.value( "params" ).toObject();
    const auto keys = params.value( "required" ).toArray();
    CHECK( keys.size() == 4 );
    for ( const auto* name : { "tab_id", "pane_id", "snapshot_id", "row" } ) CHECK( keys.contains( name ) );
    CHECK_FALSE( params.value( "properties" ).toObject().contains( "transaction_id" ) );
    CHECK( navigate.value( "result" ).toObject().value( "required" ).toArray().contains( "presented_row" ) );
}

TEST_CASE( "Schema lookup cannot enumerate all methods or bind transaction context", "[control-schema]" )
{
    for ( const auto& params : { QJsonObject{}, QJsonObject{ { "method", QJsonValue::Null } },
             QJsonObject{ { "method", QJsonArray{ "schema.get" } } }, QJsonObject{ { "method", "" } },
             QJsonObject{ { "method", "*" } }, QJsonObject{ { "method", "transaction.apply" } },
             QJsonObject{ { "method", "pane.get" }, { "transaction_id", "private" } } } ) {
        const auto response = methodSchema( { "id", "schema.get", params } );
        REQUIRE( std::holds_alternative<RpcError>( response ) );
        const auto error = std::get<RpcError>( response );
        CHECK( error.id == "id" );
        CHECK( encodeError( error ) );
    }
    const auto params = lookup( "schema.get" ).value( "params" ).toObject();
    CHECK( params.value( "properties" ).toObject().keys() == QStringList{ "method" } );
}
