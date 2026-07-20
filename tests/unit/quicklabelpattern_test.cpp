#include <catch2/catch.hpp>

#include <QRegularExpression>

#include "highlighterset.h"
#include "quicklabelpattern.h"

namespace {

bool hasMatch( const QuickLabelEntry& entry, const QString& line )
{
    QRegularExpression::PatternOptions options
        = QRegularExpression::UseUnicodePropertiesOption;
    if ( entry.ignoreCase ) {
        options |= QRegularExpression::CaseInsensitiveOption;
    }

    const QRegularExpression regex( quicklabel::pattern( entry ), options );
    REQUIRE( regex.isValid() );
    return regex.match( line ).hasMatch();
}

} // namespace

TEST_CASE( "Whole-word quick label matches punctuation-bounded selections",
           "[colorlabels][quicklabel]" )
{
    const QuickLabelEntry bracketedError{ QStringLiteral( "[ERROR]" ), false, true };
    CHECK( hasMatch( bracketedError, QStringLiteral( "2026-03-05 [ERROR] boot failed" ) ) );
    CHECK_FALSE( hasMatch( bracketedError, QStringLiteral( "2026-03-05 A[ERROR] boot failed" ) ) );
    CHECK_FALSE( hasMatch( bracketedError, QStringLiteral( "2026-03-05 [ERROR]A boot failed" ) ) );

    const QuickLabelEntry trailingPunctuation{ QStringLiteral( "foo:" ), false, true };
    CHECK( hasMatch( trailingPunctuation, QStringLiteral( "foo: value=1" ) ) );
    CHECK_FALSE( hasMatch( trailingPunctuation, QStringLiteral( "xfoo: value=1" ) ) );
}

TEST_CASE( "Whole-word quick label still matches plain word tokens",
           "[colorlabels][quicklabel]" )
{
    const QuickLabelEntry plainWord{ QStringLiteral( "WARN" ), true, true };
    CHECK( hasMatch( plainWord, QStringLiteral( "warn disk pressure high" ) ) );
    CHECK_FALSE( hasMatch( plainWord, QStringLiteral( "prewarning from daemon" ) ) );
}

TEST_CASE( "A quick label entry never matches across line boundaries",
           "[colorlabels][quicklabel]" )
{
    // Highlighters match one log line at a time, so an entry whose text
    // contains a line feed can never match anything. This pins why multi-line
    // selections must be split into per-line entries by the controller/manager
    // (storing the LF-joined blob was the multi-line color-label defect).
    const QuickLabelEntry multiLineBlob{ QStringLiteral( "line a\nline b" ), false, false };
    CHECK_FALSE( hasMatch( multiLineBlob, QStringLiteral( "line a" ) ) );
    CHECK_FALSE( hasMatch( multiLineBlob, QStringLiteral( "line b" ) ) );
    CHECK_FALSE( hasMatch( multiLineBlob, QStringLiteral( "x line a" ) ) );
}
