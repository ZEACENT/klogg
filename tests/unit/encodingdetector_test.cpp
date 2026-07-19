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

// EncodingDetector behavior tests.
//
// Background: folder search calls detectEncoding once per file per search.
// Historically every call ran uchardet under a process-global unique lock,
// which serialized the folder scan thread pool and dominated the search time
// on many-small-files trees (~1.1s for 208 files / 31 MiB on a dev machine).
// The fix: a cheap up-front check that recognizes BOM-less, NUL-free, valid
// UTF-8 (the overwhelming majority of log files) and returns the UTF-8 codec
// without touching uchardet; everything else (BOMs, NUL bytes, invalid UTF-8
// = candidate legacy encodings) still runs the full uchardet +
// codecForUtfText pipeline. These tests pin both the fast path and the
// preserved legacy behavior.

#include <catch2/catch.hpp>

#include "encodingdetector.h"

#include <QTextCodec>

#include <array>
#include <atomic>
#include <string_view>
#include <thread>
#include <vector>

namespace {

klogg::vector<char> toVec( std::string_view sv )
{
    return klogg::vector<char>( sv.begin(), sv.end() );
}

} // namespace

TEST_CASE( "EncodingDetector skips uchardet for clear UTF-8", "[encoding][detector]" )
{
    auto& detector = EncodingDetector::getInstance();
    auto& uchardetCalls = EncodingDetector::uchardetInvocationsForTesting();

    SECTION( "pure ASCII is detected as UTF-8-compatible without uchardet" )
    {
        const auto before = uchardetCalls.load();
        QTextCodec* codec = detector.detectEncoding(
            toVec( "plain ascii log line 12345\nsecond line with (brackets) [and] {json}\n" ) );
        REQUIRE( codec != nullptr );
        REQUIRE( EncodingParameters{ codec }.isUtf8Compatible );
        REQUIRE( uchardetCalls.load() == before );
    }

    SECTION( "multibyte UTF-8 is detected without uchardet" )
    {
        const auto before = uchardetCalls.load();
        QTextCodec* codec = detector.detectEncoding(
            toVec( u8"日志行：启动完成 ✓ résumé café naïve\n第二行 βγδ\n" ) );
        REQUIRE( codec != nullptr );
        REQUIRE( EncodingParameters{ codec }.isUtf8Compatible );
        REQUIRE( uchardetCalls.load() == before );
    }

    SECTION( "legacy single-byte encoding still runs full detection" )
    {
        // "café ok" in windows-1252/latin-1: a lone 0xE9 is invalid UTF-8.
        const std::array<char, 8> latin1Bytes
            = { 'c', 'a', 'f', static_cast<char>( 0xE9 ), ' ', 'o', 'k', '\n' };
        const auto before = uchardetCalls.load();
        QTextCodec* codec = detector.detectEncoding(
            toVec( std::string_view( latin1Bytes.data(), latin1Bytes.size() ) ) );
        REQUIRE( codec != nullptr );
        REQUIRE( uchardetCalls.load() > before );
    }

    SECTION( "NUL-containing samples (UTF-16/binary) still run full detection" )
    {
        // "line\n" in UTF-16LE without BOM.
        const std::array<char, 10> utf16le
            = { 'l', 0, 'i', 0, 'n', 0, 'e', 0, '\n', 0 };
        const auto before = uchardetCalls.load();
        QTextCodec* codec = detector.detectEncoding(
            toVec( std::string_view( utf16le.data(), utf16le.size() ) ) );
        REQUIRE( codec != nullptr );
        REQUIRE( uchardetCalls.load() > before );
        // Pins CURRENT behavior for BOM-less UTF-16: detection falls back to a
        // single-byte codec here (a long-standing weak case). The UTF-8 fast
        // path must not change it — NUL bytes keep this sample on the legacy
        // pipeline either way.
        REQUIRE( EncodingParameters{ codec }.lineFeedWidth == 1 );
    }

    SECTION( "UTF-16LE with BOM is detected" )
    {
        const std::array<char, 8> bomUtf16le
            = { static_cast<char>( 0xFF ), static_cast<char>( 0xFE ), 'l', 0, 'i', 0, '\n', 0 };
        QTextCodec* codec = detector.detectEncoding(
            toVec( std::string_view( bomUtf16le.data(), bomUtf16le.size() ) ) );
        REQUIRE( codec != nullptr );
        REQUIRE( EncodingParameters{ codec }.lineFeedWidth == 2 );
    }

    SECTION( "empty input yields a usable codec" )
    {
        QTextCodec* codec = detector.detectEncoding( klogg::vector<char>{} );
        REQUIRE( codec != nullptr );
    }
}

TEST_CASE( "EncodingDetector is safe under concurrent detection", "[encoding][detector]" )
{
    // Guards the lock-free fast path AND the legacy path: a shared mutex must
    // no longer be needed (all state is per-call), so hammer the detector from
    // several threads with both UTF-8 and legacy samples and require correct,
    // consistent results. Must hold before and after the fast-path change.
    auto& detector = EncodingDetector::getInstance();

    const auto utf8 = toVec( u8"并发测试 log line with unicode 内容 ✓\n" );
    const std::array<char, 8> latin1Bytes
        = { 'c', 'a', 'f', static_cast<char>( 0xE9 ), ' ', 'o', 'k', '\n' };
    const auto latin1 = toVec( std::string_view( latin1Bytes.data(), latin1Bytes.size() ) );

    constexpr int threadCount = 4;
    constexpr int iterations = 50;
    std::atomic<int> failures{ 0 };

    auto worker = [ & ]( bool useUtf8 ) {
        const auto& sample = useUtf8 ? utf8 : latin1;
        for ( int i = 0; i < iterations; ++i ) {
            QTextCodec* codec = detector.detectEncoding( sample );
            if ( codec == nullptr ) {
                ++failures;
                continue;
            }
            const EncodingParameters params{ codec };
            if ( useUtf8 != params.isUtf8Compatible ) {
                ++failures;
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve( threadCount );
    for ( int t = 0; t < threadCount; ++t ) {
        pool.emplace_back( worker, ( t % 2 ) == 0 );
    }
    for ( auto& th : pool ) {
        th.join();
    }

    REQUIRE( failures.load() == 0 );
}
