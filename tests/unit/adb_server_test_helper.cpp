/*
 * Copyright (C) 2026 ZEACENT and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr auto ExpectedServerSocket = "tcp:5037";
constexpr auto FatalSocketDiagnostic
    = "fatal: ADB_SERVER_SOCKET must use tcp:<port>; numeric host syntax is unsupported";
constexpr auto FatalCrashDiagnostic = "fatal: packaged ADB crashed while installing listener";
constexpr auto FatalLargeOutputDiagnostic
    = "fatal: packaged ADB listener failed after verbose startup output";

std::string environmentValue( const char* name )
{
    const auto* const value = std::getenv( name );
    return value == nullptr ? "<unset>" : value;
}

void writeLaunchObservation( const char* path, int argc, char* argv[] )
{
    std::ofstream observation( path, std::ios::trunc );
    observation << "argc=" << argc - 1 << '\n';
    for ( int index = 1; index < argc; ++index ) {
        observation << "argv" << index - 1 << '=' << argv[ index ] << '\n';
    }
    for ( const auto* const name :
          { "ADB_SERVER_SOCKET", "ADB_SERVER_PORT", "ANDROID_ADB_SERVER_PORT",
            "ANDROID_ADB_SERVER_ADDRESS", "ADB_VENDOR_KEYS", "ADB_TRACE" } ) {
        observation << name << '=' << environmentValue( name ) << '\n';
    }
}

bool hasConflictingEndpointEnvironment()
{
    return std::getenv( "ADB_SERVER_PORT" ) != nullptr
           || std::getenv( "ANDROID_ADB_SERVER_PORT" ) != nullptr
           || std::getenv( "ANDROID_ADB_SERVER_ADDRESS" ) != nullptr
           || std::getenv( "ADB_VENDOR_KEYS" ) != nullptr;
}

} // namespace

// Test helper failures are reported through its process exit status.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main( int argc, char* argv[] )
{
    if ( argc != 3 || std::string{ argv[ 1 ] } != "server"
         || std::string{ argv[ 2 ] } != "nodaemon" ) {
        return 2;
    }

    const auto mode = environmentValue( "KLOGG_ADB_HELPER_MODE" );
    if ( mode == "inspect-launch" ) {
        const auto* const observationPath = std::getenv( "KLOGG_ADB_HELPER_OBSERVATION" );
        if ( observationPath == nullptr ) {
            return 3;
        }
        writeLaunchObservation( observationPath, argc, argv );
        if ( environmentValue( "ADB_SERVER_SOCKET" ) != ExpectedServerSocket
             || hasConflictingEndpointEnvironment() ) {
            std::cerr << FatalSocketDiagnostic << '\n';
            return 6;
        }
        return 0;
    }
    if ( mode == "fatal-exit" ) {
        std::cerr << FatalSocketDiagnostic << '\n';
        return 6;
    }
    if ( mode == "fatal-crash" ) {
        std::cerr << FatalCrashDiagnostic << '\n';
        std::abort();
    }
    if ( mode == "large-stderr" ) {
        const std::string outputChunk( std::size_t{ 64u } * 1024u, 'x' );
        for ( int chunk = 0; chunk < 32; ++chunk ) {
            std::cerr << outputChunk;
        }
        std::cerr << '\n' << FatalLargeOutputDiagnostic << '\n';
        return 6;
    }

    const auto* const heartbeatPath = std::getenv( "KLOGG_ADB_HELPER_HEARTBEAT" );
    const auto* const stopPath = std::getenv( "KLOGG_ADB_HELPER_STOP" );
    const auto* const stoppedPath = std::getenv( "KLOGG_ADB_HELPER_STOPPED" );
    if ( heartbeatPath == nullptr || stopPath == nullptr || stoppedPath == nullptr ) {
        return 3;
    }

    const std::string outputChunk( std::size_t{ 64u } * 1024u, 'x' );
    for ( std::uint64_t heartbeat = 1u;; ++heartbeat ) {
        if ( std::ifstream{ stopPath }.good() ) {
            std::ofstream stopped( stoppedPath, std::ios::trunc );
            stopped << "stopped";
            return 0;
        }
        {
            std::ofstream output( heartbeatPath, std::ios::trunc );
            output << heartbeat;
        }
        std::cout << outputChunk << std::flush;
        std::cerr << outputChunk << std::flush;
        std::this_thread::sleep_for( std::chrono::milliseconds{ 20 } );
    }
}
