/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <catch2/catch.hpp>

#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include "configuration.h"
#include "highlighterset.h"
#include "test_utils.h"

namespace {

klogg::vector<RegularExpressionPattern> makeRegressionPatterns()
{
    klogg::vector<RegularExpressionPattern> patterns;

    RegularExpressionPattern errorPattern( QStringLiteral( R"(\bERROR\b)" ), true, false, false,
                                           false );
    errorPattern.isPrefilter = true;
    patterns.emplace_back( errorPattern );

    RegularExpressionPattern urlPattern( QStringLiteral( R"(https?://[A-Za-z0-9._~:/?#\[\]@!$&'()*+,;=%-]+)" ),
                                         true, false, false, false );
    urlPattern.isPrefilter = true;
    patterns.emplace_back( urlPattern );

    return patterns;
}

void writeHighlighterSet( QSettings& settings,
                          const klogg::vector<RegularExpressionPattern>& patterns )
{
    settings.beginGroup( QStringLiteral( "HighlighterSet" ) );
    settings.setValue( QStringLiteral( "version" ), 3 );
    settings.setValue( QStringLiteral( "name" ), QStringLiteral( "Vectorscan Regression" ) );
    settings.setValue( QStringLiteral( "id" ), QStringLiteral( "vectorscan-regression" ) );
    settings.beginWriteArray( QStringLiteral( "highlighters" ) );

    int arrayIndex = 0;
    for ( const auto& pattern : patterns ) {
        settings.setArrayIndex( arrayIndex++ );
        settings.setValue( QStringLiteral( "regexp" ), pattern.pattern );
        settings.setValue( QStringLiteral( "ignore_case" ), !pattern.isCaseSensitive );
        settings.setValue( QStringLiteral( "match_only" ), true );
        settings.setValue( QStringLiteral( "use_regex" ), true );
        settings.setValue( QStringLiteral( "variate_colors" ), false );
        settings.setValue( QStringLiteral( "color_variance" ), 15 );
        settings.setValue( QStringLiteral( "fore_colour" ),
                           QColor( Qt::black ).name( QColor::HexArgb ) );
        settings.setValue( QStringLiteral( "back_colour" ),
                           QColor( Qt::yellow ).name( QColor::HexArgb ) );
    }

    settings.endArray();
    settings.endGroup();
}

HighlighterSet loadHighlighterSet( const QString& settingsPath,
                                   const klogg::vector<RegularExpressionPattern>& patterns )
{
    QSettings settings( settingsPath, QSettings::IniFormat );
    settings.clear();
    writeHighlighterSet( settings, patterns );
    settings.sync();

    HighlighterSet set;
    set.retrieveFromStorage( settings );
    return set;
}

HighlighterSetCollection loadHighlighterSetCollection(
    const QString& settingsPath, const klogg::vector<RegularExpressionPattern>& patterns )
{
    auto set = loadHighlighterSet( settingsPath + QStringLiteral( ".set.ini" ), patterns );

    QSettings settings( settingsPath, QSettings::IniFormat );
    settings.clear();
    settings.beginGroup( QStringLiteral( "HighlighterSetCollection" ) );
    settings.setValue( QStringLiteral( "version" ), 2 );
    settings.setValue( QStringLiteral( "active_sets" ),
                       QStringList{ QStringLiteral( "vectorscan-regression" ) } );
    settings.beginWriteArray( QStringLiteral( "sets" ) );
    settings.setArrayIndex( 0 );
    set.saveToStorage( settings );
    settings.endArray();
    settings.endGroup();
    settings.sync();

    HighlighterSetCollection collection;
    collection.retrieveFromStorage( settings );
    return collection;
}

} // namespace

TEST_CASE( "Configured regex backend matches the built product", "[vectorscan][backend]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

#ifdef KLOGG_HAS_VECTORSCAN
    REQUIRE( config.regexpEngine() == RegexpEngine::Vectorscan );
#else
    REQUIRE( config.regexpEngine() == RegexpEngine::QRegularExpression );
#endif
}

TEST_CASE( "MultiRegularExpression matches representative highlighter patterns",
           "[vectorscan][backend]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    const auto patterns = makeRegressionPatterns();
    MultiRegularExpression expression( patterns );

    REQUIRE( expression.isValid() );

    const auto matcher = expression.createMatcher();
    const auto matches = matcher->match( std::string_view{ "ERROR https://example.com" } );

    REQUIRE( matches.size() == patterns.size() );
    REQUIRE( matches[ 0 ].second );
    REQUIRE( matches[ 1 ].second );
}

TEST_CASE( "HighlighterSet compile and match follow the product backend",
           "[vectorscan][highlighter]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    QTemporaryDir tempDir( QDir::tempPath() + QStringLiteral( "/vectorscan-unit-XXXXXX" ) );
    REQUIRE( tempDir.isValid() );

    auto set = loadHighlighterSet( tempDir.filePath( QStringLiteral( "highlighter.ini" ) ),
                                   makeRegressionPatterns() );
    REQUIRE_FALSE( set.isEmpty() );

    set.compile();

    HighlightedMatchRanges matches;
    const auto matchType = set.matchLine( QStringLiteral( "ERROR https://example.com" ), matches );

    REQUIRE( matchType == HighlighterMatchType::WordMatch );
    REQUIRE_FALSE( matches.empty() );
}

TEST_CASE( "HighlighterSetCollection restore compiles the active set",
           "[vectorscan][highlighter]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    QTemporaryDir tempDir( QDir::tempPath() + QStringLiteral( "/vectorscan-collection-XXXXXX" ) );
    REQUIRE( tempDir.isValid() );

    auto collection = loadHighlighterSetCollection(
        tempDir.filePath( QStringLiteral( "collection.ini" ) ), makeRegressionPatterns() );

    REQUIRE( collection.hasSet( QStringLiteral( "vectorscan-regression" ) ) );

    HighlightedMatchRanges matches;
    const auto matchType = collection.currentActiveSet().matchLine(
        QStringLiteral( "ERROR https://example.com" ), matches );

    REQUIRE( matchType == HighlighterMatchType::WordMatch );
    REQUIRE_FALSE( matches.empty() );
}

TEST_CASE( "HighlighterSet matchLine produces matches regardless of selection context",
           "[vectorscan][highlighter]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    QTemporaryDir tempDir( QDir::tempPath() + QStringLiteral( "/vectorscan-selection-XXXXXX" ) );
    REQUIRE( tempDir.isValid() );

    auto collection = loadHighlighterSetCollection(
        tempDir.filePath( QStringLiteral( "collection.ini" ) ), makeRegressionPatterns() );

    const auto& set = collection.currentActiveSet();

    // matchLine should always return matches -- it has no knowledge of selection state
    const auto testLine = QStringLiteral( "ERROR at https://example.com/path?q=1" );

    HighlightedMatchRanges matches1;
    const auto type1 = set.matchLine( testLine, matches1 );
    REQUIRE( type1 == HighlighterMatchType::WordMatch );
    REQUIRE( matches1.matches().size() == 2 );

    // Call again -- results should be consistent
    HighlightedMatchRanges matches2;
    const auto type2 = set.matchLine( testLine, matches2 );
    REQUIRE( type2 == type1 );
    REQUIRE( matches2.matches().size() == 2 );
}
