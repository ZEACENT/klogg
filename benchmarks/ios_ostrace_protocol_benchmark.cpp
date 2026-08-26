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

// Pure C++17 os_trace protocol microbenchmark. It measures the decoder and
// plain/ANSI formatter independently without Qt, a device, or libimobiledevice.
//
// Build: cmake --build build_root --target ios_ostrace_protocol_benchmark
// Run:   ./build_root/output/ios_ostrace_protocol_benchmark --iterations 200000

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "iosostraceprotocol.h"

namespace {
using namespace klogg::livecapture::ios;

constexpr std::size_t HeaderSize = 0x81u;
constexpr std::size_t PidOffset = 9u;

void putLe16( ByteBuffer& bytes, std::size_t offset, std::uint16_t value )
{
    bytes.at( offset ) = static_cast<std::uint8_t>( value & 0xffu );
    bytes.at( offset + 1u ) = static_cast<std::uint8_t>( value >> 8u );
}

void putLe32( ByteBuffer& bytes, std::size_t offset, std::uint32_t value )
{
    for ( std::size_t index = 0u; index < 4u; ++index ) {
        bytes.at( offset + index )
            = static_cast<std::uint8_t>( ( value >> static_cast<unsigned>( index * 8u ) ) & 0xffu );
    }
}

void putLe64( ByteBuffer& bytes, std::size_t offset, std::uint64_t value )
{
    for ( std::size_t index = 0u; index < 8u; ++index ) {
        bytes.at( offset + index )
            = static_cast<std::uint8_t>( ( value >> static_cast<unsigned>( index * 8u ) ) & 0xffu );
    }
}

ByteBuffer appendNul( std::string value )
{
    ByteBuffer bytes( value.begin(), value.end() );
    bytes.push_back( 0u );
    return bytes;
}

ByteBuffer benchmarkPacket()
{
    const auto processPath = appendNul( "/Applications/Runner.app/Runner" );
    const auto imagePath = appendNul( "/System/Library/Frameworks/Network.framework/Network" );
    const auto message
        = appendNul( "request completed status=200 bytes=8192 peer=10.0.0.8 latency_us=734" );
    const auto subsystem = appendNul( "com.example.runner" );
    const auto category = appendNul( "network" );

    ByteBuffer bytes( HeaderSize, 0u );
    bytes.at( 0u ) = 2u;
    putLe32( bytes, 1u, 8u );
    putLe32( bytes, 5u, static_cast<std::uint32_t>( HeaderSize ) );
    putLe32( bytes, PidOffset, 4242u );
    putLe64( bytes, 13u, 4242u );
    putLe16( bytes, 37u, static_cast<std::uint16_t>( processPath.size() ) );
    putLe64( bytes, 55u, 1700000000u );
    putLe32( bytes, 63u, 123456u );
    bytes.at( 68u ) = 0x01u;
    putLe16( bytes, 107u, static_cast<std::uint16_t>( imagePath.size() ) );
    putLe32( bytes, 109u, static_cast<std::uint32_t>( message.size() ) );
    putLe32( bytes, 113u, 0x1234u );
    putLe16( bytes, 117u, static_cast<std::uint16_t>( subsystem.size() ) );
    putLe16( bytes, 121u, static_cast<std::uint16_t>( category.size() ) );

    for ( const auto* field : { &processPath, &imagePath, &message, &subsystem, &category } ) {
        bytes.insert( bytes.end(), field->begin(), field->end() );
    }
    return bytes;
}

std::size_t parseIterations( int argc, char* argv[] )
{
    std::size_t iterations = 200000u;
    for ( int index = 1; index < argc; ++index ) {
        const std::string argument{ argv[ index ] };
        if ( argument == "--help" ) {
            std::cout << "Usage: ios_ostrace_protocol_benchmark [--iterations COUNT]\n";
            std::exit( 0 );
        }
        if ( argument != "--iterations" || index + 1 >= argc ) {
            throw std::invalid_argument( "expected --iterations COUNT" );
        }

        char* end = nullptr;
        const auto parsed = std::strtoull( argv[ ++index ], &end, 10 );
        if ( end == argv[ index ] || *end != '\0' || parsed == 0u
             || parsed > std::numeric_limits<std::size_t>::max() ) {
            throw std::invalid_argument( "iterations must be a positive size_t" );
        }
        iterations = static_cast<std::size_t>( parsed );
    }
    return iterations;
}

template <typename Operation>
std::size_t runCase( const char* name, std::size_t iterations, Operation operation )
{
    constexpr std::size_t WarmupIterations = 1000u;
    std::size_t checksum = 0u;
    for ( std::size_t index = 0u; index < WarmupIterations; ++index ) {
        checksum = checksum * std::size_t{ 1315423911u } + operation( index );
    }

    const auto started = std::chrono::steady_clock::now();
    for ( std::size_t index = 0u; index < iterations; ++index ) {
        checksum = checksum * std::size_t{ 1315423911u } + operation( index );
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto nanoseconds
        = std::chrono::duration_cast<std::chrono::nanoseconds>( elapsed ).count();
    const auto seconds = std::chrono::duration<double>( elapsed ).count();
    const auto operationsPerSecond
        = seconds > 0.0 ? static_cast<double>( iterations ) / seconds : 0.0;

    std::cout << name << ": iterations=" << iterations << " elapsed_ns=" << nanoseconds
              << " ops_per_second=" << operationsPerSecond << " checksum=" << checksum << '\n';
    return checksum;
}

} // namespace

int main( int argc, char* argv[] )
{
    try {
        const auto iterations = parseIterations( argc, argv );
        auto packet = benchmarkPacket();
        const auto decoded = decodeOsTracePacket( packet );
        if ( !decoded.record || decoded.error ) {
            throw std::runtime_error( "benchmark fixture did not decode" );
        }
        auto record = *decoded.record;

        std::size_t checksum = 0u;
        checksum ^= runCase( "decode", iterations, [ &packet ]( std::size_t index ) {
            packet.at( PidOffset ) = static_cast<std::uint8_t>( index & 0xffu );
            const auto result = decodeOsTracePacket( packet );
            if ( !result.record ) {
                return std::size_t{ 0u };
            }
            return result.record->message.value_or( std::string{} ).size()
                   ^ static_cast<std::size_t>( result.record->pid );
        } );
        checksum ^= runCase( "format_plain", iterations, [ &record ]( std::size_t index ) {
            record.pid = static_cast<std::uint32_t>( index & 0xffffu );
            return formatOsTraceRecord( record, OsTraceFormatOptions{ false, true, true } )
                .bytes.size();
        } );
        checksum ^= runCase( "format_ansi", iterations, [ &record ]( std::size_t index ) {
            record.pid = static_cast<std::uint32_t>( index & 0xffffu );
            return formatOsTraceRecord( record, OsTraceFormatOptions{ true, true, true } )
                .bytes.size();
        } );

        return checksum == std::numeric_limits<std::size_t>::max() ? 2 : 0;
    } catch ( const std::exception& error ) {
        std::cerr << "ios_ostrace_protocol_benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
