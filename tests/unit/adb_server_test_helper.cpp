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

#include <cerrno>
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

struct HelperFailure {
    std::string phase;
    int error{ 0 };
};

enum class HelperSignal : std::uint8_t { Waiting, Ready, Failed, Stopped };

bool appendHelperRecord( const char* path, const std::string& text, std::size_t& bytes,
                         std::size_t maximum, const char* purpose, HelperFailure& failure,
                         bool injectWriteFailure = false )
{
    const auto failed = [ & ]( const char* operation ) {
        failure = { std::string{ purpose } + '-' + operation, errno };
        return false;
    };
    if ( bytes > maximum || text.size() > maximum - bytes ) {
        errno = 0;
        return failed( "limit" );
    }
    errno = 0;
    std::ofstream output( path, std::ios::app | std::ios::binary );
    if ( !output.is_open() ) {
        return failed( "open" );
    }
    if ( injectWriteFailure ) {
        output.setstate( std::ios::badbit );
    }
    output.write( text.data(), static_cast<std::streamsize>( text.size() ) );
    output.flush();
    if ( !output ) {
        return failed( "write" );
    }
    output.close();
    if ( !output ) {
        return failed( "close" );
    }
    bytes += text.size();
    return true;
}

HelperSignal readHelperSignal( const std::string& path, const std::string& expected,
                               HelperFailure& failure )
{
    errno = 0;
    std::ifstream input( path, std::ios::binary );
    if ( !input.is_open() ) {
        if ( errno == ENOENT ) {
            return HelperSignal::Waiting;
        }
        failure = { "control-open", errno };
        return HelperSignal::Failed;
    }
    char bytes[ 65 ]{};
    input.read( bytes, static_cast<std::streamsize>( sizeof( bytes ) ) );
    if ( input.bad() ) {
        failure = { "control-read", errno };
        return HelperSignal::Failed;
    }
    const std::string text( bytes, static_cast<std::size_t>( input.gcount() ) );
    if ( text == expected ) {
        return HelperSignal::Ready;
    }
    // A control-file writer may have opened the file but not committed its
    // complete request yet. Never acknowledge an incomplete request.
    if ( text.size() < expected.size() && expected.compare( 0u, text.size(), text ) == 0 ) {
        return HelperSignal::Waiting;
    }
    failure = { "control-invalid", 0 };
    return HelperSignal::Failed;
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
        std::cerr << "adb-helper phase=configuration error=missing-control-path\n";
        return 3;
    }

    constexpr std::size_t MaximumProgressBytes = std::size_t{ 64u } * 1024u;
    constexpr std::size_t MaximumStatusBytes = 4096u;
    constexpr auto pacing = std::chrono::milliseconds{ 20 };
    std::size_t progressBytes = 0u;
    std::size_t statusBytes = 0u;
    HelperFailure failure;
    const auto status = [ & ]( const std::string& record, bool failWrite = false ) {
        return appendHelperRecord( stoppedPath, record, statusBytes, MaximumStatusBytes, "status",
                                   failure, failWrite );
    };
    const auto fail = [ & ] {
        const auto detail = failure.phase + " errno=" + std::to_string( failure.error );
        std::cerr << "adb-helper phase=" << detail << '\n';
        HelperFailure ignored;
        appendHelperRecord( stoppedPath, "terminal error " + detail + '\n', statusBytes,
                            MaximumStatusBytes, "status", ignored );
        return 4;
    };
    const auto stop = [ & ] {
        if ( !status( "terminal stopped\n", mode == "fail-terminal-write" ) ) {
            return fail();
        }
        return 0;
    };
    const auto waitForFixtureSignal = [ & ]( const char* suffix ) {
        for ( ;; ) {
            const auto signal
                = readHelperSignal( std::string{ heartbeatPath } + suffix, "continue\n", failure );
            if ( signal != HelperSignal::Waiting ) {
                return signal;
            }
            const auto stopping = readHelperSignal( stopPath, "stop", failure );
            if ( stopping == HelperSignal::Ready ) {
                return HelperSignal::Stopped;
            }
            if ( stopping == HelperSignal::Failed ) {
                return stopping;
            }
            std::this_thread::sleep_for( pacing ); // lint-allow: test-timing -- barrier poll
        }
    };
    if ( !status( "phase main-entered\n" ) ) {
        return fail();
    }
    bool requestObserved = false;
    unsigned int acknowledgementFloods = 0u;
    const std::string outputChunk( std::size_t{ 64u } * 1024u, 'x' );
    for ( std::uint64_t heartbeat = 1u;; ++heartbeat ) {
        const auto stopping = readHelperSignal( stopPath, "stop", failure );
        if ( stopping == HelperSignal::Ready ) {
            return stop();
        }
        if ( stopping == HelperSignal::Failed ) {
            return fail();
        }
        const auto partial = mode == "partial-progress" && heartbeat == 2u;
        if ( !appendHelperRecord( heartbeatPath,
                                  std::to_string( heartbeat ) + ( partial ? "" : "\n" ),
                                  progressBytes, MaximumProgressBytes, "progress", failure,
                                  mode == "fail-progress-write" ) ) {
            return fail();
        }
        if ( heartbeat == 1u && !status( "phase streaming\n" ) ) {
            return fail();
        }
        if ( partial ) {
            std::size_t markerBytes = 0u;
            const auto marker = std::string{ heartbeatPath } + ".partial-ready";
            if ( !status( "phase partial-held\n" )
                 || !appendHelperRecord( marker.c_str(), "ready\n", markerBytes, 64u,
                                         "partial-ready", failure ) ) {
                return fail();
            }
        }
        if ( mode == "partial-progress" && heartbeat <= 2u ) {
            const auto signal
                = waitForFixtureSignal( heartbeat == 1u ? ".begin-partial" : ".complete-partial" );
            if ( signal == HelperSignal::Stopped ) {
                return stop();
            }
            if ( signal == HelperSignal::Failed ) {
                return fail();
            }
            if ( partial
                 && !appendHelperRecord( heartbeatPath, "\n", progressBytes, MaximumProgressBytes,
                                         "progress", failure ) ) {
                return fail();
            }
        }
        if ( !requestObserved ) {
            const auto requested
                = readHelperSignal( std::string{ heartbeatPath } + ".after-destruction",
                                    "after-destruction\n", failure );
            if ( requested == HelperSignal::Failed ) {
                return fail();
            }
            if ( requested == HelperSignal::Ready ) {
                requestObserved = true;
                // Two MiB per channel after the request exceeds a pipe buffer;
                // an old heartbeat or one buffered write cannot prove drainage.
                acknowledgementFloods = 32u;
                if ( !status( "phase post-destruction-flood\n" ) ) {
                    return fail();
                }
            }
        }
        std::cout << outputChunk << std::flush;
        std::cerr << outputChunk << std::flush;
        if ( !std::cout || !std::cerr ) {
            failure = { "flood-write", errno };
            return fail();
        }
        if ( acknowledgementFloods != 0u ) {
            --acknowledgementFloods;
            if ( acknowledgementFloods == 0u && !status( "ack after-destruction\n" ) ) {
                return fail();
            }
        }
        std::this_thread::sleep_for( pacing ); // lint-allow: test-timing -- heartbeat pacing
    }
}
