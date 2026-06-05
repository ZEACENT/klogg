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
#include "matchercache.h"
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

// --- MatcherCache tests ---

TEST_CASE( "MatcherCache returns reusable matchers for the same expression",
           "[matchercache]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    auto expression = std::make_shared<RegularExpression>(
        RegularExpressionPattern( QStringLiteral( "ERROR" ), true, false, false, true ) );
    REQUIRE( expression->isValid() );

    MatcherCache cache;

    // First access: no cached matchers, should create new ones.
    auto m1 = cache.acquire( expression );
    REQUIRE( m1 != nullptr );
    REQUIRE( m1->hasMatch( std::string_view{ "ERROR: crash" } ) );
    REQUIRE_FALSE( m1->hasMatch( std::string_view{ "INFO: ok" } ) );

    // Return the matcher to the pool.
    cache.release( std::move( m1 ) );

    // Second access: should reuse the returned matcher (no new creation).
    auto m2 = cache.acquire( expression );
    REQUIRE( m2 != nullptr );
    REQUIRE( m2->hasMatch( std::string_view{ "ERROR: crash" } ) );
    REQUIRE_FALSE( m2->hasMatch( std::string_view{ "INFO: ok" } ) );
}

TEST_CASE( "MatcherCache evicts matchers when expression changes",
           "[matchercache]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    auto expr1 = std::make_shared<RegularExpression>(
        RegularExpressionPattern( QStringLiteral( "ERROR" ), true, false, false, true ) );
    auto expr2 = std::make_shared<RegularExpression>(
        RegularExpressionPattern( QStringLiteral( "WARN" ), true, false, false, true ) );
    REQUIRE( expr1->isValid() );
    REQUIRE( expr2->isValid() );

    MatcherCache cache;

    auto m1 = cache.acquire( expr1 );
    REQUIRE( m1->hasMatch( std::string_view{ "ERROR: crash" } ) );
    cache.release( std::move( m1 ) );

    // Acquire for a different expression — old matchers should be evicted.
    auto m2 = cache.acquire( expr2 );
    REQUIRE( m2->hasMatch( std::string_view{ "WARN: low mem" } ) );
    REQUIRE_FALSE( m2->hasMatch( std::string_view{ "ERROR: crash" } ) );
}

TEST_CASE( "MatcherCache supports multiple matchers per expression",
           "[matchercache]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    auto expression = std::make_shared<RegularExpression>(
        RegularExpressionPattern( QStringLiteral( "ERROR" ), true, false, false, true ) );
    REQUIRE( expression->isValid() );

    MatcherCache cache;

    // Acquire multiple matchers (simulating TBB parallel threads).
    auto m1 = cache.acquire( expression );
    auto m2 = cache.acquire( expression );
    auto m3 = cache.acquire( expression );
    REQUIRE( m1 != nullptr );
    REQUIRE( m2 != nullptr );
    REQUIRE( m3 != nullptr );

    // All should produce correct results.
    const std::string_view line{ "ERROR: disk full" };
    REQUIRE( m1->hasMatch( line ) );
    REQUIRE( m2->hasMatch( line ) );
    REQUIRE( m3->hasMatch( line ) );

    // Return them.
    cache.release( std::move( m1 ) );
    cache.release( std::move( m2 ) );
    cache.release( std::move( m3 ) );

    // Acquire again — should reuse from pool without creating new ones.
    auto m4 = cache.acquire( expression );
    auto m5 = cache.acquire( expression );
    REQUIRE( m4 != nullptr );
    REQUIRE( m5 != nullptr );
    REQUIRE( m4->hasMatch( line ) );
    REQUIRE( m5->hasMatch( line ) );
}

TEST_CASE( "MatcherCache pooled matchers produce identical results to fresh matchers",
           "[matchercache]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    RegularExpression expression(
        RegularExpressionPattern( QStringLiteral( "ERROR" ), true, false, false, true ) );
    REQUIRE( expression.isValid() );

    MatcherCache cache;
    auto expr = std::make_shared<RegularExpression>(
        RegularExpressionPattern( QStringLiteral( "ERROR" ), true, false, false, true ) );

    // Create a fresh matcher for reference.
    const auto fresh = expression.createMatcher();

    // Get a matcher from the cache (first call creates, then release and re-acquire).
    auto pooled = cache.acquire( expr );
    cache.release( std::move( pooled ) );
    pooled = cache.acquire( expr );

    const std::vector<std::string> testLines = {
        "ERROR: disk full",
        "WARN: low memory",
        "INFO: system started",
        "ERROR: timeout",
        "DEBUG: checkpoint",
    };

    for ( const auto& line : testLines ) {
        REQUIRE( fresh->hasMatch( std::string_view{ line } )
                 == pooled->hasMatch( std::string_view{ line } ) );
    }
}

TEST_CASE( "MatcherCache evicts stale matchers when prior expression expires",
           "[matchercache]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    auto expr1 = std::make_shared<RegularExpression>(
        RegularExpressionPattern( QStringLiteral( "ERROR" ), true, false, false, true ) );
    REQUIRE( expr1->isValid() );

    MatcherCache cache;

    // Acquire and release a matcher for expr1 — it goes into the pool.
    auto m1 = cache.acquire( expr1 );
    REQUIRE( m1->hasMatch( std::string_view{ "ERROR: crash" } ) );
    cache.release( std::move( m1 ) );

    // Let expr1 go out of scope so the weak_ptr in the cache expires.
    expr1.reset();

    // Now acquire for a different expression.  The pool must be cleared
    // because the cached weak_ptr has expired — returning the stale
    // matcher would produce wrong results.
    auto expr2 = std::make_shared<RegularExpression>(
        RegularExpressionPattern( QStringLiteral( "WARN" ), true, false, false, true ) );
    REQUIRE( expr2->isValid() );

    auto m2 = cache.acquire( expr2 );
    REQUIRE( m2 != nullptr );
    REQUIRE( m2->hasMatch( std::string_view{ "WARN: low mem" } ) );
    REQUIRE_FALSE( m2->hasMatch( std::string_view{ "ERROR: crash" } ) );
}

// --- Lookahead / Lookbehind assertion tests ---

// Helper struct to hold a regex test case.
struct AssertionTestCase {
    std::string description;
    QString pattern;
    std::string testLine;
    bool shouldMatch;
};

// Test lookahead and lookbehind with the given regex engine.
static void runAssertionTests( RegexpEngine engine )
{
    ScopedRegexpEngine scoped( engine );

    const std::string logLine1
        = "2026-06-06 00:10:09.238613 familycircled{Network}[3078] <DEBUG>: endpoint "
          "<private> has associations";

    // Patterns where lookbehind/lookahead are expected to work correctly.
    const std::vector<AssertionTestCase> cases = {
        // --- Positive lookbehind ---
        {
            "positive lookbehind: 'associations' immediately after 'has ' → match",
            QStringLiteral( "(?<=has )associations" ),
            logLine1,
            true,
        },
        {
            "positive lookbehind: 'associations' NOT immediately after 'familycircled' → no match",
            QStringLiteral( "(?<=familycircled)associations" ),
            logLine1,
            false,
        },
        // --- Negative lookbehind ---
        {
            "negative lookbehind: 'associations' NOT preceded by 'missed' → match",
            QStringLiteral( "(?<!missed )associations" ),
            logLine1,
            true,
        },
        {
            "negative lookbehind: 'associations' IS preceded by 'has ' → no match",
            QStringLiteral( "(?<!has )associations" ),
            logLine1,
            false,
        },
        // --- Positive lookahead ---
        {
            "positive lookahead: 'has' followed by ' associations' → match",
            QStringLiteral( "has(?= associations)" ),
            logLine1,
            true,
        },
        {
            "positive lookahead: 'has' NOT followed by ' nothing' → no match",
            QStringLiteral( "has(?= nothing)" ),
            logLine1,
            false,
        },
        // --- Negative lookahead ---
        {
            "negative lookahead: 'has' NOT followed by ' nothing' → match",
            QStringLiteral( "has(?! nothing)" ),
            logLine1,
            true,
        },
        {
            "negative lookahead: 'has' IS followed by ' associations' → no match",
            QStringLiteral( "has(?! associations)" ),
            logLine1,
            false,
        },
        // --- Lookbehind on shorter simple string ---
        {
            "simple lookbehind on 'foo bar' → match",
            QStringLiteral( "(?<=foo )bar" ),
            "foo bar",
            true,
        },
        {
            "simple lookahead on 'foo bar' → match",
            QStringLiteral( "foo(?= bar)" ),
            "foo bar",
            true,
        },
        // --- Pattern with only lookbehind and no trailing literal ---
        {
            "lookbehind with dot-star: 'endpoint' somewhere after 'familycircled' → match",
            QStringLiteral( "(?<=familycircled).*endpoint" ),
            logLine1,
            true,
        },
    };

    for ( const auto& tc : cases ) {
        INFO( "Engine: " << static_cast<int>( engine ) << " — " << tc.description );
        INFO( "Pattern: " << tc.pattern.toStdString() );
        INFO( "Test line: " << tc.testLine );

        RegularExpression expr(
            RegularExpressionPattern( tc.pattern, true, false, false, false ) );
        REQUIRE( expr.isValid() );

        const auto matcher = expr.createMatcher();
        if ( tc.shouldMatch ) {
            REQUIRE( matcher->hasMatch( std::string_view{ tc.testLine } ) );
        }
        else {
            REQUIRE_FALSE( matcher->hasMatch( std::string_view{ tc.testLine } ) );
        }
    }
}

TEST_CASE( "Lookahead and lookbehind assertions with Vectorscan engine",
           "[patternmatcher][lookaround]" )
{
    // Only meaningful when Vectorscan is available; otherwise the ScopedRegexpEngine
    // silently stays on Qt and we still test the QRegularExpression path.
    runAssertionTests( RegexpEngine::Vectorscan );
}

TEST_CASE( "Lookahead and lookbehind assertions with Qt engine",
           "[patternmatcher][lookaround]" )
{
    runAssertionTests( RegexpEngine::QRegularExpression );
}

// Verify the exact user-reported scenario with explicit annotation.
TEST_CASE( "User-reported lookbehind scenario", "[patternmatcher][lookaround]" )
{
    // Configure the product-like engine (Vectorscan when available).
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    const std::string logLine
        = "2026-06-06 00:10:09.238613 familycircled{Network}[3078] <DEBUG>: endpoint "
          "<private> has associations";

    SECTION( "Pattern that should match: (?<=has )associations" )
    {
        RegularExpression expr( RegularExpressionPattern(
            QStringLiteral( "(?<=has )associations" ), true, false, false, false ) );
        REQUIRE( expr.isValid() );
        const auto matcher = expr.createMatcher();
        REQUIRE( matcher->hasMatch( std::string_view{ logLine } ) );
    }

    SECTION( "Pattern that should NOT match: (?<=familycircled)associations" )
    {
        // (?<=familycircled)associations means: match "associations" only when
        // IMMEDIATELY preceded by "familycircled".  In the log line above,
        // "associations" is preceded by "has ", NOT by "familycircled".
        // Therefore this correctly does NOT match.
        RegularExpression expr( RegularExpressionPattern(
            QStringLiteral( "(?<=familycircled)associations" ), true, false, false, false ) );
        REQUIRE( expr.isValid() );
        const auto matcher = expr.createMatcher();
        REQUIRE_FALSE( matcher->hasMatch( std::string_view{ logLine } ) );
    }

    SECTION( "Pattern familycircled.*associations matches (no lookbehind)" )
    {
        // If the user wants to find lines containing "familycircled" followed
        // (anywhere) by "associations", a simple .* pattern works:
        RegularExpression expr( RegularExpressionPattern(
            QStringLiteral( "familycircled.*associations" ), true, false, false, false ) );
        REQUIRE( expr.isValid() );
        const auto matcher = expr.createMatcher();
        REQUIRE( matcher->hasMatch( std::string_view{ logLine } ) );
    }

    SECTION( "Variable-length lookbehind: (?<=familycircled.*)associations — KNOWN LIMITATION" )
    {
        // Variable-length lookbehind: asserts that "familycircled" appears
        // somewhere before "associations" in the same line. The match itself
        // is only "associations" — "familycircled" and the characters in
        // between are NOT part of the match.
        //
        // BUG: QRegularExpression rejects .* (or any variable-length quantifier)
        // inside a lookbehind assertion.  The pattern is marked invalid and
        // the expression will never match.
        RegularExpression expr( RegularExpressionPattern(
            QStringLiteral( "(?<=familycircled.*)associations" ), true, false, false, false ) );
        // QRegularExpression considers this pattern INVALID.
        REQUIRE_FALSE( expr.isValid() );
    }

    SECTION( "\\K reset: familycircled.*\\Kassociations — match only 'associations'" )
    {
        // \K resets the match start after "familycircled.*" has been consumed.
        // The match text is only "associations".
        RegularExpression expr( RegularExpressionPattern(
            QStringLiteral( "familycircled.*\\Kassociations" ), true, false, false, false ) );
        REQUIRE( expr.isValid() );
        const auto matcher = expr.createMatcher();
        REQUIRE( matcher->hasMatch( std::string_view{ logLine } ) );

        // Also verify with QRegularExpression directly to check match position.
        QRegularExpression re( QStringLiteral( "familycircled.*\\Kassociations" ),
                               QRegularExpression::UseUnicodePropertiesOption );
        REQUIRE( re.isValid() );
        auto match = re.match( QString::fromStdString( logLine ) );
        REQUIRE( match.hasMatch() );
        REQUIRE( match.captured() == QStringLiteral( "associations" ) );
    }

    SECTION( "Lookahead anchor: ^(?=.*familycircled).*?\\Kassociations" )
    {
        // Anchored at start, lookahead asserts "familycircled" exists,
        // then consume to "associations" and \K reset.
        RegularExpression expr( RegularExpressionPattern(
            QStringLiteral( "^(?=.*familycircled).*?\\Kassociations" ), true, false, false,
            false ) );
        REQUIRE( expr.isValid() );
        const auto matcher = expr.createMatcher();
        REQUIRE( matcher->hasMatch( std::string_view{ logLine } ) );
    }

    SECTION( "Boolean filter: \"familycircled\" & \"associations\"" )
    {
        // klogg's boolean filter syntax keeps the two patterns independent.
        RegularExpression expr( RegularExpressionPattern(
            QStringLiteral( "\"familycircled\" & \"associations\"" ), true, false, true, false ) );
        REQUIRE( expr.isValid() );
        const auto matcher = expr.createMatcher();
        REQUIRE( matcher->hasMatch( std::string_view{ logLine } ) );
    }
}
