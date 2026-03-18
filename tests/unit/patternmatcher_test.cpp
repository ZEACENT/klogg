/*
 * Copyright (C) 2021 Anton Filimonov and other contributors
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

#include <algorithm>
#include <set>
#include <string>

#include "configuration.h"
#include "containers.h"
#include "regularexpression.h"
#include "test_utils.h"

SCENARIO( "Pattern matcher in boolean mode", "[patternmatcher]" )
{
    std::string_view matchLine = "\"This\" is matching pattern";

    WHEN( "Using single pattern" )
    {
        RegularExpression expression(
            RegularExpressionPattern( "\"matching\"", false, false, true, true ) );
        const auto matcher = expression.createMatcher();
        REQUIRE( matcher->hasMatch( matchLine ) );
    }

    WHEN( "Using complex pattern" )
    {
        RegularExpression expression(
            RegularExpressionPattern( "\"not_match\" | \"match\"", false, false, true, true ) );
        const auto matcher = expression.createMatcher();
        REQUIRE( matcher->hasMatch( matchLine ) );
    }

    WHEN( "Using complex pattern with ()" )
    {
        RegularExpression expression( RegularExpressionPattern(
            "(\"not_match\" | \"match\") & !(\"pattern\")", false, false, true, false ) );
        const auto matcher = expression.createMatcher();
        REQUIRE_FALSE( matcher->hasMatch( matchLine ) );
    }

    WHEN( "Using pattern with escaped quotes" )
    {
        RegularExpression expression(
            RegularExpressionPattern( "\"\\\"This\\\"\"", false, false, true, false ) );
        const auto matcher = expression.createMatcher();
        REQUIRE( matcher->hasMatch( matchLine ) );
    }

    WHEN( "Using pattern with not matched quotes" )
    {
        RegularExpression expression(
            RegularExpressionPattern( "\"not_match\" | \"match", false, false, true, false ) );

        REQUIRE_FALSE( expression.isValid() );
    }
}

TEST_CASE( "Block scan matches per-line scan results", "[patternmatcher]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    // Simple plaintext pattern to search for.
    RegularExpression expression(
        RegularExpressionPattern( QStringLiteral( "ERROR" ), true, false, false, true ) );
    REQUIRE( expression.isValid() );
    const auto matcher = expression.createMatcher();

    // Build a multi-line buffer with a mix of matching and non-matching lines.
    const std::vector<std::string> lines = {
        "INFO: system started",
        "ERROR: disk full",
        "WARN: low memory",
        "ERROR: timeout reached",
        "DEBUG: checkpoint",
        "INFO: recovery complete",
        "ERROR: connection refused",
    };

    // Build a contiguous buffer with newline-separated lines and endOfLines offsets.
    std::string buffer;
    klogg::vector<qint64> endOfLines;
    for ( const auto& line : lines ) {
        buffer += line;
        buffer += '\n';
        endOfLines.push_back( static_cast<qint64>( buffer.size() ) );
    }

    // Per-line scan: run hasMatch on each line individually.
    std::set<size_t> perLineMatches;
    for ( size_t i = 0; i < lines.size(); ++i ) {
        if ( matcher->hasMatch( std::string_view{ lines[ i ] } ) ) {
            perLineMatches.insert( i );
        }
    }

    // Verify that the per-line scan found the expected lines.
    REQUIRE( perLineMatches.count( 1 ) == 1 ); // "ERROR: disk full"
    REQUIRE( perLineMatches.count( 3 ) == 1 ); // "ERROR: timeout reached"
    REQUIRE( perLineMatches.count( 6 ) == 1 ); // "ERROR: connection refused"
    REQUIRE( perLineMatches.size() == 3 );

    // Block scan: if available, it should produce identical results.
    klogg::vector<uint64_t> blockMatches;
    const bool blockScanPerformed = matcher->scanBuffer(
        buffer.data(), static_cast<unsigned int>( buffer.size() ), endOfLines, blockMatches );

    if ( matcher->hasBufferScan() ) {
        REQUIRE( blockScanPerformed );

        std::set<size_t> blockMatchSet;
        for ( const auto idx : blockMatches ) {
            blockMatchSet.insert( static_cast<size_t>( idx ) );
        }
        REQUIRE( blockMatchSet == perLineMatches );
    }
    else {
        // Without vectorscan, scanBuffer should return false.
        REQUIRE_FALSE( blockScanPerformed );
    }
}

TEST_CASE( "Block scan falls back when Vectorscan is unavailable", "[patternmatcher]" )
{
    // Force the Qt regex engine, which does not support block scanning.
    ScopedRegexpEngine scopedEngine( RegexpEngine::QRegularExpression );

    RegularExpression expression(
        RegularExpressionPattern( QStringLiteral( "ERROR" ), true, false, false, true ) );
    REQUIRE( expression.isValid() );
    const auto matcher = expression.createMatcher();

    REQUIRE_FALSE( matcher->hasBufferScan() );

    // scanBuffer should return false when the engine lacks block scan support.
    std::string buffer = "ERROR: something\nINFO: ok\n";
    klogg::vector<qint64> endOfLines = { 17, 26 };
    klogg::vector<uint64_t> matchedIndices;

    const bool result = matcher->scanBuffer( buffer.data(),
                                              static_cast<unsigned int>( buffer.size() ),
                                              endOfLines, matchedIndices );
    REQUIRE_FALSE( result );
    REQUIRE( matchedIndices.empty() );

    // Per-line hasMatch still works correctly as the fallback.
    REQUIRE( matcher->hasMatch( std::string_view{ "ERROR: something" } ) );
    REQUIRE_FALSE( matcher->hasMatch( std::string_view{ "INFO: ok" } ) );
}
