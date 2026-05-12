#include <catch2/catch.hpp>

#include "iosdeviceparser.h"

TEST_CASE( "stripAnsiSequences removes CSI color sequences" )
{
    const auto input = QStringLiteral( "\x1b[31mRedText\x1b[0m normal" );
    const auto result = stripAnsiSequences( input );
    REQUIRE( result == QStringLiteral( "RedText normal" ) );
}

TEST_CASE( "stripAnsiSequences removes multi-param CSI sequences" )
{
    const auto input = QStringLiteral( "\x1b[38;2;255;0;0mTrueColor\x1b[39m tail\x1b[K" );
    const auto result = stripAnsiSequences( input );
    REQUIRE( result == QStringLiteral( "TrueColor tail" ) );
}

TEST_CASE( "stripAnsiSequences leaves plain text unchanged" )
{
    const auto input = QStringLiteral( "plain text no escapes" );
    REQUIRE( stripAnsiSequences( input ) == input );
}

TEST_CASE( "parsePymobiledeviceDeviceList extracts device info from JSON" )
{
    const QByteArray json = "[{"
        "\"Identifier\":\"00008030-001C195E36D8802E\","
        "\"DeviceName\":\"Test iPhone\","
        "\"ProductType\":\"iPhone14,2\","
        "\"ProductVersion\":\"17.0\","
        "\"BuildVersion\":\"21A329\","
        "\"ConnectionType\":\"USB\","
        "\"DeviceClass\":\"iPhone\""
        "}]";

    const auto devices = parsePymobiledeviceDeviceList( json );
    REQUIRE( devices.size() == 1 );
    CHECK( devices[ 0 ].udid == QStringLiteral( "00008030-001C195E36D8802E" ) );
    CHECK( devices[ 0 ].productType == QStringLiteral( "iPhone14,2" ) );
    CHECK( devices[ 0 ].productVersion == QStringLiteral( "17.0" ) );
}

TEST_CASE( "parsePymobiledeviceDeviceList formats displayName on a single line" )
{
    const QByteArray json = "[{"
        "\"Identifier\":\"00008030-001C195E36D8802E\","
        "\"DeviceName\":\"Test iPhone\","
        "\"ProductType\":\"iPhone14,2\","
        "\"ProductVersion\":\"17.0\""
        "}]";

    const auto devices = parsePymobiledeviceDeviceList( json );
    REQUIRE( devices.size() == 1 );

    // displayName must be a single line with DeviceName, Identifier, ProductType, ProductVersion
    const auto& name = devices[ 0 ].displayName;
    CHECK_FALSE( name.contains( QLatin1Char( '\n' ) ) );
    CHECK( name.contains( QStringLiteral( "Test iPhone" ) ) );
    CHECK( name.contains( QStringLiteral( "00008030-001C195E36D8802E" ) ) );
    CHECK( name.contains( QStringLiteral( "iPhone14,2" ) ) );
    CHECK( name.contains( QStringLiteral( "17.0" ) ) );
}

TEST_CASE( "parsePymobiledeviceDeviceList excludes BuildVersion ConnectionType DeviceClass from displayName" )
{
    const QByteArray json = "[{"
        "\"Identifier\":\"00008030\","
        "\"DeviceName\":\"Test iPhone\","
        "\"ProductType\":\"iPhone14,2\","
        "\"ProductVersion\":\"17.0\","
        "\"BuildVersion\":\"21A329\","
        "\"ConnectionType\":\"USB\","
        "\"DeviceClass\":\"Smartphone\""
        "}]";

    const auto devices = parsePymobiledeviceDeviceList( json );
    REQUIRE( devices.size() == 1 );

    const auto& name = devices[ 0 ].displayName;
    CHECK_FALSE( name.contains( QStringLiteral( "21A329" ) ) );
    CHECK_FALSE( name.contains( QStringLiteral( "USB" ) ) );
    CHECK_FALSE( name.contains( QStringLiteral( "Smartphone" ) ) );
}

TEST_CASE( "parsePymobiledeviceDeviceList does not promote DeviceClass into displayName" )
{
    const QByteArray json = "[{"
        "\"Identifier\":\"00008030\","
        "\"DeviceName\":\"My Tablet\","
        "\"DeviceClass\":\"AppleTV\","
        "\"ProductVersion\":\"17.0\""
        "}]";

    const auto devices = parsePymobiledeviceDeviceList( json );
    REQUIRE( devices.size() == 1 );
    CHECK_FALSE( devices[ 0 ].displayName.contains( QStringLiteral( "AppleTV" ) ) );
    CHECK( devices[ 0 ].productType.isEmpty() );
}

TEST_CASE( "parsePymobiledeviceDeviceList strips ANSI sequences from JSON values" )
{
    // JSON uses  for the ESC character; pymobiledevice3 may emit
    // ANSI-colored output that ends up embedded in JSON string values.
    const QByteArray json = "[{"
        "\"Identifier\":\"\\u001b[32m00008030\\u001b[0m\","
        "\"DeviceName\":\"\\u001b[33mTest iPhone\\u001b[0m\""
        "}]";

    const auto devices = parsePymobiledeviceDeviceList( json );
    REQUIRE( devices.size() == 1 );
    CHECK( devices[ 0 ].udid == QStringLiteral( "00008030" ) );
    CHECK_FALSE( devices[ 0 ].displayName.contains( QChar( 0x1b ) ) );
}

TEST_CASE( "parsePymobiledeviceDeviceList handles string-array output" )
{
    const QByteArray json = "[\"00008030-001C195E36D8802E\"]";

    const auto devices = parsePymobiledeviceDeviceList( json );
    REQUIRE( devices.size() == 1 );
    CHECK( devices[ 0 ].udid == QStringLiteral( "00008030-001C195E36D8802E" ) );
}

TEST_CASE( "parsePymobiledeviceDeviceList returns empty for invalid JSON" )
{
    const QByteArray invalid = "not json at all";
    REQUIRE( parsePymobiledeviceDeviceList( invalid ).isEmpty() );
}

TEST_CASE( "parsePymobiledeviceSimpleDeviceList falls back to line-by-line parsing with ANSI stripping" )
{
    // Simulates pymobiledevice3 usbmux list --simple output with ANSI codes
    const QByteArray output =
        "\x1b[32m00008030-001C195E36D8802E  Test iPhone  iPhone14,2  17.0\x1b[0m\n"
        "\x1b[34m00008031-002D296F47E9903F  iPad Pro  iPad13,8  17.1\x1b[0m\n";

    const auto devices = parsePymobiledeviceSimpleDeviceList( output );
    REQUIRE( devices.size() == 2 );

    // ANSI sequences must be stripped
    CHECK_FALSE( devices[ 0 ].displayName.contains( QStringLiteral( "\x1b" ) ) );
    CHECK_FALSE( devices[ 1 ].displayName.contains( QStringLiteral( "\x1b" ) ) );

    // displayName must be a single line
    CHECK_FALSE( devices[ 0 ].displayName.contains( QLatin1Char( '\n' ) ) );
    CHECK_FALSE( devices[ 1 ].displayName.contains( QLatin1Char( '\n' ) ) );
}

TEST_CASE( "parsePymobiledeviceSimpleDeviceList tries JSON first" )
{
    const QByteArray json = "[{"
        "\"Identifier\":\"00008030\","
        "\"DeviceName\":\"Test iPhone\","
        "\"ProductType\":\"iPhone14,2\","
        "\"ProductVersion\":\"17.0\""
        "}]";

    const auto devices = parsePymobiledeviceSimpleDeviceList( json );
    REQUIRE( devices.size() == 1 );
    CHECK( devices[ 0 ].udid == QStringLiteral( "00008030" ) );
    CHECK( devices[ 0 ].productType == QStringLiteral( "iPhone14,2" ) );
}

TEST_CASE( "parsePymobiledeviceDeviceList parses real pymobiledevice3 output" )
{
    // Actual output from pymobiledevice3 usbmux list on this machine
    const QByteArray json = R"([{
        "BuildVersion": "23E261",
        "ConnectionType": "USB",
        "DeviceClass": "iPhone",
        "DeviceName": "ZEACENT’s iPhone",
        "Identifier": "00008150-001431410C78401C",
        "ProductType": "iPhone18,3",
        "ProductVersion": "26.4.2",
        "UniqueDeviceID": "00008150-001431410C78401C"
    }])";

    const auto devices = parsePymobiledeviceDeviceList( json );
    REQUIRE( devices.size() == 1 );

    const auto& device = devices[ 0 ];
    CHECK( device.udid == QStringLiteral( "00008150-001431410C78401C" ) );
    CHECK( device.productType == QStringLiteral( "iPhone18,3" ) );
    CHECK( device.productVersion == QStringLiteral( "26.4.2" ) );

    // displayName must be a single line with DeviceName, Identifier, ProductType, ProductVersion
    CHECK_FALSE( device.displayName.contains( QLatin1Char( '\n' ) ) );
    CHECK( device.displayName.contains( QStringLiteral( "ZEACENT" ) ) );
    CHECK( device.displayName.contains( QStringLiteral( "00008150-001431410C78401C" ) ) );
    CHECK( device.displayName.contains( QStringLiteral( "iPhone18,3" ) ) );
    CHECK( device.displayName.contains( QStringLiteral( "26.4.2" ) ) );

    // Should NOT contain BuildVersion or ConnectionType values
    CHECK_FALSE( device.displayName.contains( QStringLiteral( "23E261" ) ) );
    CHECK_FALSE( device.displayName.contains( QStringLiteral( "USB" ) ) );
}

TEST_CASE( "parsePymobiledeviceDeviceList strips ANSI codes wrapping JSON before parsing" )
{
    // pymobiledevice3 may emit ANSI-colored output where escape codes appear
    // outside the JSON structure, e.g. \x1b[32m[...]\x1b[0m
    // Without pre-stripping, QJsonDocument::fromJson fails and the fallback
    // parser treats each line of pretty-printed JSON as a separate device.
    const QByteArray jsonWithAnsiPrefix
        = "\x1b[32m[{"
          "\"Identifier\":\"00008150-001431410C78401C\","
          "\"DeviceName\":\"Test iPhone\","
          "\"ProductType\":\"iPhone18,3\","
          "\"ProductVersion\":\"26.4.2\""
          "}]\x1b[0m";

    const auto devices = parsePymobiledeviceDeviceList( jsonWithAnsiPrefix );
    REQUIRE( devices.size() == 1 );
    CHECK( devices[ 0 ].udid == QStringLiteral( "00008150-001431410C78401C" ) );
    CHECK( devices[ 0 ].productType == QStringLiteral( "iPhone18,3" ) );
    CHECK( devices[ 0 ].productVersion == QStringLiteral( "26.4.2" ) );

    // displayName must be a single line
    CHECK_FALSE( devices[ 0 ].displayName.contains( QLatin1Char( '\n' ) ) );
    CHECK( devices[ 0 ].displayName.contains( QStringLiteral( "Test iPhone" ) ) );
}

TEST_CASE( "parsePymobiledeviceDeviceList handles ANSI codes around pretty-printed JSON" )
{
    // Real pymobiledevice3 output: pretty-printed JSON with ANSI color
    const QByteArray prettyJsonWithAnsi
        = "\x1b[1m[\x1b[0m\n"
          "\x1b[1m  {\x1b[0m\n"
          "    \x1b[33m\"Identifier\"\x1b[0m: \x1b[36m\"00008150\"\x1b[0m,\n"
          "    \x1b[33m\"DeviceName\"\x1b[0m: \x1b[36m\"My iPhone\"\x1b[0m,\n"
          "    \x1b[33m\"ProductType\"\x1b[0m: \x1b[36m\"iPhone18,3\"\x1b[0m,\n"
          "    \x1b[33m\"ProductVersion\"\x1b[0m: \x1b[36m\"26.4\"\x1b[0m\n"
          "  }\n"
          "]\n";

    const auto devices = parsePymobiledeviceDeviceList( prettyJsonWithAnsi );
    REQUIRE( devices.size() == 1 );
    CHECK( devices[ 0 ].udid == QStringLiteral( "00008150" ) );
    CHECK( devices[ 0 ].displayName.contains( QStringLiteral( "My iPhone" ) ) );
    CHECK( devices[ 0 ].displayName.contains( QStringLiteral( "iPhone18,3" ) ) );
    CHECK_FALSE( devices[ 0 ].displayName.contains( QLatin1Char( '\n' ) ) );
}

TEST_CASE( "parsePymobiledeviceDeviceList parses --simple output as UDID-only" )
{
    // Actual --simple output: ["UDID"]
    const QByteArray json = R"(["00008150-001431410C78401C"])";

    const auto devices = parsePymobiledeviceDeviceList( json );
    REQUIRE( devices.size() == 1 );
    CHECK( devices[ 0 ].udid == QStringLiteral( "00008150-001431410C78401C" ) );
    CHECK( devices[ 0 ].productType.isEmpty() );
    CHECK( devices[ 0 ].productVersion.isEmpty() );
}
