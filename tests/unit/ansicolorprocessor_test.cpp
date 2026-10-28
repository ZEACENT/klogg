#include <catch2/catch.hpp>

#include "ansicolorprocessor.h"
#include "searchablelogdata.h"

TEST_CASE( "Raw text colors and UTF8 search share per-row normalization",
           "[text-normalization][normalized-backends]" )
{
    SearchableLogData::RawLines lines;
    const QByteArray bytes{ "\x1b[31mprefix ERROR\x1b[0m\r\nprefix NEXT\r\n" };
    lines.buffer.assign( bytes.begin(), bytes.end() );
    lines.endOfLines = { bytes.indexOf( '\n' ) + 1, bytes.size() };
    lines.textDecoder = TextCodecHolder{ QTextCodec::codecForName( "UTF-8" ) }.makeDecoder();
    lines.prefilterPattern = QRegularExpression{ QStringLiteral( "^prefix " ) };
    lines.ansiProcessingMode = AnsiProcessingMode::Render;
    SECTION( "Color coordinates follow removed prefixes" )
    {
        const auto colors = lines.decodeLineAnsiColors();
        REQUIRE( colors.size() == 2 );
        REQUIRE( colors.front().size() == 1 );
        REQUIRE( colors.front().front().length == 5_length );
    }
    SECTION( "Optimized search normalizes each record instead of the whole block" )
    {
        const auto decoded = lines.buildUtf8View();
        REQUIRE( decoded.size() == 2 );
        REQUIRE( decoded.front() == "ERROR" );
        REQUIRE( decoded.back() == "NEXT" );
    }
}

TEST_CASE( "Shared normalization remaps ANSI colors after prefix removal", "[text-normalization]" )
{
    const auto processed = processLogLine(
        QStringLiteral( "\x1b[31mprefix ERROR\x1b[0m" ), AnsiProcessingMode::Render,
        QRegularExpression{ QStringLiteral( "^prefix " ) } );
    REQUIRE( processed.text == QStringLiteral( "ERROR" ) );
    REQUIRE( processed.colorSpans.size() == 1 );
    REQUIRE( processed.colorSpans.front().startColumn == 0_lcol );
    REQUIRE( processed.colorSpans.front().length == 5_length );
    REQUIRE( processed.colorSpans.front().foreground == 0xde382b );
}

TEST_CASE( "Shared normalization drops removed colors and preserves surviving coordinates",
           "[text-normalization]" )
{
    const auto processed = processLogLine(
        QStringLiteral( "a\x1b[31mred\x1b[0m-\x1b[32mgreen\x1b[0m" ),
        AnsiProcessingMode::Render, QRegularExpression{ QStringLiteral( "red-|re" ) } );
    REQUIRE( processed.text == QStringLiteral( "agen" ) );
    REQUIRE( processed.colorSpans.size() == 1 );
    REQUIRE( processed.colorSpans.front().startColumn == 1_lcol );
    REQUIRE( processed.colorSpans.front().length == 3_length );
}

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
