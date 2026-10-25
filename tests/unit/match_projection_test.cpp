#include <catch2/catch.hpp>

#include "highlighterset.h"
#include "quickfindpattern.h"

namespace {
QStringList highlightedText( const QString& text, const klogg::vector<HighlightedMatch>& spans )
{
    QStringList result;
    for ( const auto& span : spans ) {
        result.append( text.mid( span.startColumn().get(), span.size().get() ) );
    }
    return result;
}
} // namespace

TEST_CASE( "Filter projects library paths using lookbehind or captures", "[match-projection]" )
{
    const QString line = QStringLiteral(
        "install_plt_hook() [plt-installed] /vendor/lib64/egl/libEGL_emulation.so:dlsym@@0x123" );
    const auto pattern
        = GENERATE( R"((?<=\[plt-installed\] )[^:]*)", R"((?:\[plt-installed\] )([^:]+)(?=:))" );
    Highlighter highlighter{ QString::fromLatin1( pattern ), false, true, Qt::black, Qt::yellow };
    klogg::vector<HighlightedMatch> spans;
    REQUIRE( highlighter.matchLine( line, spans ) );
    REQUIRE( highlightedText( line, spans )
             == QStringList{ QStringLiteral( "/vendor/lib64/egl/libEGL_emulation.so" ) } );
}

TEST_CASE( "Noncapturing groups remain part of a full match", "[match-projection]" )
{
    const QString line = QStringLiteral( "prefixwanted prefixwanted" );
    Highlighter highlighter{ QStringLiteral( "(?:prefix)wanted" ), false, true, Qt::black,
                             Qt::yellow };
    klogg::vector<HighlightedMatch> spans;
    REQUIRE( highlighter.matchLine( line, spans ) );
    REQUIRE( highlightedText( line, spans )
             == QStringList{ QStringLiteral( "prefixwanted" ), QStringLiteral( "prefixwanted" ) } );
}

TEST_CASE( "Quick Find projects full matches even with captures", "[match-projection]" )
{
    const QString line = QStringLiteral( "prefixwanted prefixwanted" );
    QuickFindPattern pattern;
    pattern.changeSearchPattern( QStringLiteral( "(?:prefix)(wanted)" ), false, true, false );
    klogg::vector<HighlightedMatch> spans;
    REQUIRE( pattern.matchLine( line, spans ) );
    REQUIRE( highlightedText( line, spans )
             == QStringList{ QStringLiteral( "prefixwanted" ), QStringLiteral( "prefixwanted" ) } );
}

TEST_CASE( "Filter projection follows Unicode word semantics used by the search engine",
           "[match-projection][projection-boundary]" )
{
    const QString line = QString::fromUtf8( "\xCE\xB1\xCE\xB2" );
    Highlighter highlighter{ QStringLiteral( R"((\w+))" ), false, true, Qt::black, Qt::yellow };
    klogg::vector<HighlightedMatch> spans;
    REQUIRE( highlighter.matchLine( line, spans ) );
    REQUIRE( highlightedText( line, spans ) == QStringList{ line } );
}

TEST_CASE( "Filter projection ignores missing and empty captures",
           "[match-projection][projection-boundary]" )
{
    const QString line = QStringLiteral( "b" );
    Highlighter highlighter{ QStringLiteral( "(a)?()(b)" ), false, true, Qt::black, Qt::yellow };
    klogg::vector<HighlightedMatch> spans;
    REQUIRE( highlighter.matchLine( line, spans ) );
    REQUIRE( spans.size() == 1 );
    REQUIRE( highlightedText( line, spans ) == QStringList{ QStringLiteral( "b" ) } );
}

TEST_CASE( "Whole-line highlighters retain zero-width predicate matches", "[match-projection]" )
{
    Highlighter highlighter{ QStringLiteral( "^" ), false, false, Qt::black, Qt::yellow };
    klogg::vector<HighlightedMatch> spans;
    REQUIRE( highlighter.matchLine( QStringLiteral( "whole line" ), spans ) );
}

TEST_CASE( "Zero width matches do not create selectable highlight fragments",
           "[match-projection][projection-boundary]" )
{
    const QString line = QStringLiteral( "abc" );
    klogg::vector<HighlightedMatch> spans;
    SECTION( "Filter" )
    {
        Highlighter highlighter{ QStringLiteral( "(?=a)" ), false, true, Qt::black, Qt::yellow };
        REQUIRE_FALSE( highlighter.matchLine( line, spans ) );
    }
    SECTION( "Quick Find" )
    {
        QuickFindPattern pattern;
        pattern.changeSearchPattern( QStringLiteral( "(?=a)" ), false, true, false );
        REQUIRE_FALSE( pattern.matchLine( line, spans ) );
    }
    REQUIRE( spans.empty() );
}
