/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#include "live_capture_benchmark_core.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined( _WIN32 )
#include <fcntl.h>
#include <io.h>
#endif

namespace {

void configureBinaryStandardStreams()
{
#if defined( _WIN32 )
    if ( _setmode( _fileno( stdout ), _O_BINARY ) == -1
         || _setmode( _fileno( stdin ), _O_BINARY ) == -1 ) {
        throw std::runtime_error( "could not enable binary standard streams" );
    }
#endif
}

struct Options {
    std::string fixturePath;
    bool holdOpen{ false };
};

Options parseOptions( int argc, char* argv[] )
{
    Options options;
    bool streamFixture = false;
    for ( int index = 1; index < argc; ++index ) {
        const std::string argument{ argv[ index ] };
        if ( argument == "--stream-framed-fixture" ) {
            streamFixture = true;
        }
        else if ( argument == "--fixture" && index + 1 < argc ) {
            options.fixturePath = argv[ ++index ];
        }
        else if ( argument == "--hold-open" ) {
            options.holdOpen = true;
        }
        else {
            throw std::invalid_argument( "unexpected fixture producer argument" );
        }
    }
    if ( !streamFixture || options.fixturePath.empty() ) {
        throw std::invalid_argument( "--stream-framed-fixture and --fixture are required" );
    }
    return options;
}

} // namespace

int main( int argc, char* argv[] )
{
    try {
        configureBinaryStandardStreams();
        const auto options = parseOptions( argc, argv );
        std::ifstream fixture( options.fixturePath, std::ios::binary );
        if ( !fixture ) {
            throw std::runtime_error( "could not open the staged fixture" );
        }

        std::array<char, 64u * 1024u> buffer{};
        std::size_t emittedBytes = 0u;
        while ( fixture ) {
            fixture.read( buffer.data(), static_cast<std::streamsize>( buffer.size() ) );
            const auto count = fixture.gcount();
            if ( count <= 0 ) {
                break;
            }
            std::cout.write( buffer.data(), count );
            std::cout.flush();
            if ( !std::cout ) {
                throw std::runtime_error( "fixture output write failed" );
            }
            emittedBytes += static_cast<std::size_t>( count );
        }
        if ( fixture.bad()
             || emittedBytes < klogg::benchmarks::livecapture::FramedRecordHeaderBytes ) {
            throw std::runtime_error( "staged fixture read failed or was truncated" );
        }

        if ( options.holdOpen ) {
            char ignored = '\0';
            while ( std::cin.get( ignored ) ) {
            }
        }
        return 0;
    } catch ( const std::exception& error ) {
        std::cerr << "live_capture_fixture_producer failed: " << error.what() << '\n';
        return 1;
    }
}
