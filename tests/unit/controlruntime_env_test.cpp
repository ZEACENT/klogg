#include <catch2/catch.hpp>

#include <QFileInfo>
#include <QTemporaryDir>

#include "controltransport.h"

using namespace klogg::control;

TEST_CASE( "Control discovery override isolates executable integration without relaxing permissions",
           "[control-runtime]" )
{
    constexpr auto variable = "KLOGG_CONTROL_DESCRIPTOR_ROOT";
    struct Restore {
        const char* name;
        bool set;
        QByteArray value;
        ~Restore() { if ( set ) qputenv( name, value ); else qunsetenv( name ); }
    } restore{ variable, qEnvironmentVariableIsSet( variable ), qgetenv( variable ) };
    QTemporaryDir directory;
    REQUIRE( directory.isValid() );
    const auto root = QFileInfo( directory.path() ).canonicalFilePath();
    RuntimeOptions options;
    options.descriptorRoot = root + "/discovery";
    ControlServer server;
    QString error;
    REQUIRE( server.start( options, []( const RpcRequest& ) -> DecodedResponse {
        return QJsonObject{};
    }, &error ) );
    REQUIRE( qputenv( variable, options.descriptorRoot.toUtf8() ) );
    const auto discovered = discoverInstances();
    REQUIRE( discovered.error.isEmpty() );
    REQUIRE( discovered.instances.size() == 1 );
    CHECK( discovered.instances.front().instanceId == server.descriptor().instanceId );
    RuntimeOptions other;
    other.descriptorRoot = root + "/absent";
    CHECK( discoverInstances( other ).instances.empty() );
    server.stop();
    CHECK( discoverInstances().instances.empty() );
    REQUIRE( qputenv( variable, QByteArray( "relative/unsafe" ) ) );
    CHECK_FALSE( server.start( {}, []( const RpcRequest& ) -> DecodedResponse {
        return QJsonObject{};
    }, &error ) );
    CHECK_FALSE( error.isEmpty() );
}
