#include <catch2/catch.hpp>

#include "ansicolorprocessor.h"

TEST_CASE( "ANSI processor strips CSI sequences without rendering colors" )
{
    const auto processed = processAnsiSequences(
        QStringLiteral( "plain \x1b[31mred\x1b[0m tail\x1b[K" ), AnsiProcessingMode::Strip );

    REQUIRE( processed.text == QStringLiteral( "plain red tail" ) );
    REQUIRE( processed.colorSpans.empty() );
}

TEST_CASE( "ANSI processor renders SGR color ranges on stripped text" )
{
    const auto processed = processAnsiSequences(
        QStringLiteral( "a\x1b[31mred\x1b[0m b\x1b[38;2;1;2;3mtrue\x1b[39m" ),
        AnsiProcessingMode::Render );

    REQUIRE( processed.text == QStringLiteral( "ared btrue" ) );
    REQUIRE( processed.colorSpans.size() == 2 );

    REQUIRE( processed.colorSpans[ 0 ].startColumn == 1_lcol );
    REQUIRE( processed.colorSpans[ 0 ].length == 3_length );
    REQUIRE( processed.colorSpans[ 0 ].foreground == 0xde382b );

    REQUIRE( processed.colorSpans[ 1 ].startColumn == 6_lcol );
    REQUIRE( processed.colorSpans[ 1 ].length == 4_length );
    REQUIRE( processed.colorSpans[ 1 ].foreground == 0x010203 );
}

TEST_CASE( "ANSI processor leaves escape text untouched in plain mode" )
{
    const auto source = QStringLiteral( "\x1b[32mgreen" );
    const auto processed = processAnsiSequences( source, AnsiProcessingMode::Plain );

    REQUIRE( processed.text == source );
    REQUIRE( processed.colorSpans.empty() );
}
