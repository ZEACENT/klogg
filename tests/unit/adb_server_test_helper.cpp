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

// Test helper failures are reported through its process exit status.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main( int argc, char* argv[] )
{
    if ( argc != 3 || std::string{ argv[ 1 ] } != "server"
         || std::string{ argv[ 2 ] } != "nodaemon" ) {
        return 2;
    }

    const auto* const heartbeatPath = std::getenv( "KLOGG_ADB_HELPER_HEARTBEAT" );
    const auto* const stopPath = std::getenv( "KLOGG_ADB_HELPER_STOP" );
    if ( heartbeatPath == nullptr || stopPath == nullptr ) {
        return 3;
    }

    const std::string outputChunk( std::size_t{ 64u } * 1024u, 'x' );
    for ( std::uint64_t heartbeat = 1u;; ++heartbeat ) {
        if ( std::ifstream{ stopPath }.good() ) {
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
