#include <catch2/catch.hpp>

#include <functional>
#include <QStringView>

#include "wrappedstring.h"

namespace {
// Fixed-width mock: each character is 10px wide
int fixedWidthFn( QStringView s )
{
    return static_cast<int>( s.size() ) * 10;
}

// Mixed-width mock: 'W' is 15px, others are 10px
int mixedWidthFn( QStringView s )
{
    int width = 0;
    for ( auto c : s ) {
        width += ( c == QChar( 'W' ) ) ? 15 : 10;
    }
    return width;
}
} // namespace

// ---------- Existing character-based wrapping (regression) ----------

TEST_CASE( "WrappedString character-based wrapping fits within column count" )
{
    // 20 chars, wrap at 10 columns -> 2 lines
    WrappedString ws( QStringLiteral( "abcdefghijklmnopqrst" ), LineLength{ 10 } );
    REQUIRE( ws.wrappedLinesCount() == 2 );
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "abcdefghij" ) );
    REQUIRE( ws.wrappedLine( 1 ).toString() == QStringLiteral( "klmnopqrst" ) );
}

TEST_CASE( "WrappedString character-based wrapping at word boundary" )
{
    // "hello world" is 11 chars, wrap at 8 columns
    // First 8 chars: "hello wo" -> last space at pos 5 -> wrap at "hello "
    WrappedString ws( QStringLiteral( "hello world test" ), LineLength{ 8 } );
    REQUIRE( ws.wrappedLinesCount() >= 2 );
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "hello " ) );
}

TEST_CASE( "WrappedString character-based empty string" )
{
    WrappedString ws( QString{}, LineLength{ 10 } );
    REQUIRE( ws.wrappedLinesCount() == 1 );
    REQUIRE( ws.wrappedLine( 0 ).isEmpty() );
}

TEST_CASE( "WrappedString character-based short string needs no wrap" )
{
    WrappedString ws( QStringLiteral( "short" ), LineLength{ 10 } );
    REQUIRE( ws.wrappedLinesCount() == 1 );
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "short" ) );
}

// ---------- Pixel-based wrapping ----------

TEST_CASE( "WrappedString pixel wrapping wraps at pixel boundary" )
{
    // 10 chars at 10px each = 100px total. Available width 85px -> fits 8 chars (80px)
    WrappedString ws( QStringLiteral( "abcdefghij" ), 85, fixedWidthFn );
    REQUIRE( ws.wrappedLinesCount() == 2 );
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "abcdefgh" ) ); // 80px <= 85px
    REQUIRE( ws.wrappedLine( 1 ).toString() == QStringLiteral( "ij" ) ); // 20px
}

TEST_CASE( "WrappedString pixel wrapping fills available width" )
{
    // With mixed-width chars: "aWcde" = 10+15+10+10+10 = 55px
    // Available 50px: "aWc" = 10+15+10 = 35px fits, "aWcd" = 45px fits,
    // "aWcde" = 55px exceeds. So max 4 chars on first line.
    WrappedString ws( QStringLiteral( "aWcdef" ), 50, mixedWidthFn );
    REQUIRE( ws.wrappedLinesCount() == 2 );
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "aWcd" ) ); // 45px <= 50px
    REQUIRE( ws.wrappedLine( 1 ).toString() == QStringLiteral( "ef" ) ); // 20px
}

TEST_CASE( "WrappedString pixel wrapping respects word boundary" )
{
    // "hello world" at 10px/char, available 70px -> fits 7 chars max
    // First 7 chars: "hello w" has no space inside (space is at index 5)
    // Should wrap at word boundary "hello " (6 chars, 60px)
    WrappedString ws( QStringLiteral( "hello world test" ), 70, fixedWidthFn );
    REQUIRE( ws.wrappedLinesCount() >= 2 );
    // Word wrap should keep "hello " on first line (60px < 70px)
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "hello " ) );
}

TEST_CASE( "WrappedString pixel wrapping hard-wraps long word" )
{
    // No spaces: "abcdefghij" at 10px/char, available 45px -> fits 4 chars (40px)
    WrappedString ws( QStringLiteral( "abcdefghij" ), 45, fixedWidthFn );
    REQUIRE( ws.wrappedLinesCount() >= 2 );
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "abcd" ) ); // 40px <= 45px
}

TEST_CASE( "WrappedString pixel wrapping single char per line" )
{
    // Available 10px, each char 10px -> 1 char per line
    WrappedString ws( QStringLiteral( "abc" ), 10, fixedWidthFn );
    REQUIRE( ws.wrappedLinesCount() == 3 );
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "a" ) );
    REQUIRE( ws.wrappedLine( 1 ).toString() == QStringLiteral( "b" ) );
    REQUIRE( ws.wrappedLine( 2 ).toString() == QStringLiteral( "c" ) );
}

TEST_CASE( "WrappedString pixel wrapping empty string" )
{
    WrappedString ws( QString{}, 100, fixedWidthFn );
    REQUIRE( ws.wrappedLinesCount() == 1 );
    REQUIRE( ws.wrappedLine( 0 ).isEmpty() );
}

TEST_CASE( "WrappedString pixel wrapping short string needs no wrap" )
{
    // "short" = 50px, available 100px
    WrappedString ws( QStringLiteral( "short" ), 100, fixedWidthFn );
    REQUIRE( ws.wrappedLinesCount() == 1 );
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "short" ) );
}

TEST_CASE( "WrappedString pixel wrapping exact fit" )
{
    // 8 chars at 10px = 80px, available 80px -> fits exactly
    WrappedString ws( QStringLiteral( "abcdefgh" ), 80, fixedWidthFn );
    REQUIRE( ws.wrappedLinesCount() == 1 );
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "abcdefgh" ) );
}

TEST_CASE( "WrappedString pixel wrapping one pixel over causes wrap" )
{
    // 9 chars at 10px = 90px, available 89px -> 8 chars (80px) fit, 9th exceeds
    WrappedString ws( QStringLiteral( "abcdefghi" ), 89, fixedWidthFn );
    REQUIRE( ws.wrappedLinesCount() == 2 );
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "abcdefgh" ) );
    REQUIRE( ws.wrappedLine( 1 ).toString() == QStringLiteral( "i" ) );
}

TEST_CASE( "WrappedString pixel wrapping with mixed width fills more than char count" )
{
    // With char-width wrapping at nbVisibleCols=4, "aWcd" would be the first line
    // because it's exactly 4 chars. But with pixel wrapping at 50px,
    // "aWcde" = 10+15+10+10+10 = 55px > 50px, so "aWcd" (45px) fits.
    // Then "efgh" = 10+10+10+10 = 40px fits on the next line.
    // Key point: pixel wrapping should NOT use char count as the limit.
    // It should use the actual pixel width to determine the break point.
    WrappedString ws( QStringLiteral( "aWcdefghij" ), 50, mixedWidthFn );
    // First line: "aWcd" = 45px <= 50px, "aWcde" = 55px > 50px -> wrap at "aWcd"
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "aWcd" ) );
}

TEST_CASE( "WrappedString pixel wrapping word boundary earlier than pixel limit" )
{
    // "abc def" at 10px/char, available 70px
    // "abc def" = 70px exactly. But "abc de" = 60px has space at pos 3
    // "abc def" fits in 70px, so no wrap needed
    WrappedString ws( QStringLiteral( "abc def" ), 70, fixedWidthFn );
    REQUIRE( ws.wrappedLinesCount() == 1 );
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "abc def" ) );
}

TEST_CASE( "WrappedString pixel wrapping prefers word break over char break" )
{
    // "hello world test" at 10px/char, available 90px
    // "hello worl" = 100px > 90px, so "hello wor" = 90px fits, but has space at pos 5
    // Should wrap at "hello " (60px) since word boundary is preferred
    WrappedString ws( QStringLiteral( "hello world test" ), 90, fixedWidthFn );
    REQUIRE( ws.wrappedLinesCount() >= 2 );
    REQUIRE( ws.wrappedLine( 0 ).toString() == QStringLiteral( "hello " ) );
}

// ---------- Pixel wrapping: each line's pixel width <= available width ----------

TEST_CASE( "WrappedString pixel wrapping no wrapped line exceeds available width" )
{
    // Complex case: mixed-width chars, verify each line fits
    WrappedString ws( QStringLiteral( "aWWbc defWWghi jklWW" ), 55, mixedWidthFn );

    for ( size_t i = 0; i < ws.wrappedLinesCount(); ++i ) {
        int linePx = mixedWidthFn( ws.wrappedLine( i ) );
        REQUIRE( linePx <= 55 );
    }
}
