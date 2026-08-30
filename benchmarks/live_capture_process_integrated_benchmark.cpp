/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 */

#include "live_capture_benchmark_core.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "capturestore.h"

namespace benchmark = klogg::benchmarks::livecapture;

namespace {

enum class ArmSelection : std::uint8_t { Process, Integrated, Both };

constexpr std::size_t MaximumRecordCount = 1000000u;
constexpr std::size_t MaximumSegmentCount = 1000000u;

struct Options {
    ArmSelection arms{ ArmSelection::Both };
    std::size_t recordCount{ 16u };
    std::size_t segmentCount{ 2u };
    std::uint64_t generation{ 1u };
    std::uint32_t trial{ 0u };
};

std::uint64_t parseUnsigned( const char* text, const char* name )
{
    if ( text == nullptr || *text == '-' ) {
        throw std::invalid_argument( std::string{ name } + " must be an unsigned integer" );
    }
    errno = 0;
    char* end = nullptr;
    const auto value = std::strtoull( text, &end, 10 );
    if ( errno == ERANGE || end == text || *end != '\0' ) {
        throw std::invalid_argument( std::string{ name } + " must be an unsigned integer" );
    }
    return value;
}

std::size_t parseSize( const std::string& text, const char* name )
{
    const auto value = parseUnsigned( text.c_str(), name );
    if ( value > std::numeric_limits<std::size_t>::max() ) {
        throw std::out_of_range( std::string{ name } + " exceeds size_t" );
    }
    return static_cast<std::size_t>( value );
}

Options parseOptions( int argc, char* argv[] )
{
    Options options;
    for ( int index = 1; index < argc; ++index ) {
        const std::string argument{ argv[ index ] };
        if ( argument == "--help" ) {
            std::cout << "Usage: live_capture_process_integrated_benchmark "
                         "[--arm process|integrated|both] [--records COUNT] "
                         "[--segments COUNT] [--generation VALUE] [--trial VALUE]\n";
            std::exit( 0 );
        }
        if ( index + 1 >= argc ) {
            throw std::invalid_argument( "benchmark option requires a value" );
        }
        const std::string value{ argv[ ++index ] };
        if ( argument == "--arm" ) {
            if ( value == "process" ) {
                options.arms = ArmSelection::Process;
            }
            else if ( value == "integrated" ) {
                options.arms = ArmSelection::Integrated;
            }
            else if ( value == "both" ) {
                options.arms = ArmSelection::Both;
            }
            else {
                throw std::invalid_argument( "--arm must be process, integrated, or both" );
            }
        }
        else if ( argument == "--records" ) {
            options.recordCount = parseSize( value, "records" );
        }
        else if ( argument == "--segments" ) {
            options.segmentCount = parseSize( value, "segments" );
        }
        else if ( argument == "--generation" ) {
            options.generation = parseUnsigned( value.c_str(), "generation" );
        }
        else if ( argument == "--trial" ) {
            const auto trial = parseUnsigned( value.c_str(), "trial" );
            if ( trial > std::numeric_limits<std::uint32_t>::max() ) {
                throw std::out_of_range( "trial exceeds uint32" );
            }
            options.trial = static_cast<std::uint32_t>( trial );
        }
        else {
            throw std::invalid_argument( "unexpected benchmark option" );
        }
    }
    if ( options.recordCount == 0u || options.segmentCount == 0u
         || options.segmentCount > options.recordCount ) {
        throw std::invalid_argument(
            "records and segments must be positive; segments cannot exceed records" );
    }
    if ( options.recordCount > MaximumRecordCount || options.segmentCount > MaximumSegmentCount ) {
        throw std::invalid_argument(
            "records or segments exceed the deterministic benchmark bound" );
    }
    return options;
}

benchmark::Bytes payload( std::size_t segment, std::size_t sequence )
{
    const auto text = std::string{ "synthetic-segment-" } + std::to_string( segment ) + "-record-"
                      + std::to_string( sequence );
    return { text.cbegin(), text.cend() };
}

benchmark::FramedFixture fixture( const Options& options )
{
    benchmark::FixturePlan plan;
    plan.binding = { benchmark::FramedRecordVersion, options.generation, options.trial };
    const auto recordsPerSegment = options.recordCount / options.segmentCount;
    const auto extraRecords = options.recordCount % options.segmentCount;
    for ( std::size_t segment = 0u; segment < options.segmentCount; ++segment ) {
        const auto count = recordsPerSegment + ( segment < extraRecords ? 1u : 0u );
        benchmark::FixtureSegment fixtureSegment;
        fixtureSegment.segment = static_cast<std::uint32_t>( segment );
        for ( std::size_t sequence = 0u; sequence < count; ++sequence ) {
            fixtureSegment.payloads.push_back( payload( segment, sequence ) );
        }
        plan.segments.push_back( std::move( fixtureSegment ) );
    }
    return benchmark::makeFramedFixture( plan );
}

std::filesystem::path filesystemPath( const QString& path )
{
    const auto utf8 = path.toUtf8();
    return std::filesystem::u8path( utf8.constData(), utf8.constData() + utf8.size() );
}

struct RealOptions {
    benchmark::RealDeviceArm arm{ benchmark::RealDeviceArm::NativeIntegrated };
    std::filesystem::path baselineExecutable;
    std::filesystem::path nativeStackRoot;
    QString deviceIdentifierFile;
    std::uint64_t durationMilliseconds{ 0u };
    bool ansiEnabled{ false };
    benchmark::RealDeviceNativeService nativeService{
        benchmark::RealDeviceNativeService::LegacySyslog
    };
};

RealOptions parseRealOptions( int argc, char* argv[] )
{
    RealOptions options;
    for ( int index = 2; index < argc; ++index ) {
        const std::string argument{ argv[ index ] };
        if ( index + 1 >= argc ) {
            throw std::invalid_argument( "real-device option requires a value" );
        }
        const std::string value{ argv[ ++index ] };
        if ( argument == "--arm" ) {
            if ( value == "native" ) {
                options.arm = benchmark::RealDeviceArm::NativeIntegrated;
            }
            else if ( value == "baseline" ) {
                options.arm = benchmark::RealDeviceArm::BaselineProcess;
            }
            else {
                throw std::invalid_argument( "real-device arm must be native or baseline" );
            }
        }
        else if ( argument == "--baseline-executable" ) {
            options.baselineExecutable = value;
        }
        else if ( argument == "--native-stack-root" ) {
            options.nativeStackRoot = value;
        }
        else if ( argument == "--device-identifier-file" ) {
            options.deviceIdentifierFile
                = QString::fromUtf8( value.data(), static_cast<int>( value.size() ) );
        }
        else if ( argument == "--duration-ms" ) {
            options.durationMilliseconds = parseUnsigned( value.c_str(), "duration-ms" );
        }
        else if ( argument == "--ansi" ) {
            if ( value != "0" && value != "1" ) {
                throw std::invalid_argument( "--ansi must be 0 or 1" );
            }
            options.ansiEnabled = value == "1";
        }
        else if ( argument == "--native-service" ) {
            if ( value == "auto" ) {
                options.nativeService = benchmark::RealDeviceNativeService::Automatic;
            }
            else if ( value == "syslog" ) {
                options.nativeService = benchmark::RealDeviceNativeService::LegacySyslog;
            }
            else if ( value == "ostrace" ) {
                options.nativeService = benchmark::RealDeviceNativeService::OsTrace;
            }
            else {
                throw std::invalid_argument( "--native-service must be auto, syslog, or ostrace" );
            }
        }
        else {
            throw std::invalid_argument( "unexpected real-device benchmark option" );
        }
    }
    if ( options.deviceIdentifierFile.isEmpty() || options.durationMilliseconds < 500u
         || options.durationMilliseconds > 10000u ) {
        throw std::invalid_argument(
            "real-device identifier file and 500-10000ms duration are required" );
    }
    return options;
}

std::string readPrivateDeviceIdentifier( const QString& path )
{
    const QFileInfo info{ path };
    const auto forbidden = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                           | QFileDevice::ReadOther | QFileDevice::WriteOther
                           | QFileDevice::ExeOther;
    if ( !info.isAbsolute() || !info.isFile() || info.isSymLink()
         || info.canonicalFilePath() != info.absoluteFilePath()
         || ( info.permissions() & forbidden ) != 0 ) {
        throw std::invalid_argument(
            "device identifier file must be an absolute private regular file" );
    }
    QFile file{ path };
    if ( !file.open( QIODevice::ReadOnly ) ) {
        throw std::runtime_error( "device identifier file could not be opened" );
    }
    const auto bytes = file.read( 256 );
    if ( bytes.isEmpty() || !file.atEnd() ) {
        throw std::invalid_argument( "device identifier file is empty or oversized" );
    }
    return bytes.trimmed().toStdString();
}

void runRealDeviceArm( const RealOptions& options )
{
    QTemporaryDir captureRoot{ QStringLiteral( "klogg-real-live-capture-XXXXXX" ) };
    if ( !captureRoot.isValid() ) {
        throw std::runtime_error( "could not create real-device capture root" );
    }
    benchmark::RealDevicePlan plan;
    plan.arm = options.arm;
    plan.baselineExecutable = options.baselineExecutable;
    plan.nativeStackRoot = options.nativeStackRoot;
    plan.deviceIdentifier = readPrivateDeviceIdentifier( options.deviceIdentifierFile );
    plan.captureRoot = filesystemPath( captureRoot.path() );
    plan.durationMilliseconds = options.durationMilliseconds;
    plan.ansiEnabled = options.ansiEnabled;
    plan.nativeService = options.nativeService;
    const auto observation = benchmark::runRealDeviceArm( plan );
    plan.deviceIdentifier.assign( plan.deviceIdentifier.size(), '\0' );
    plan.deviceIdentifier.clear();
    if ( !QDir{ captureRoot.path() }.isEmpty() ) {
        throw std::runtime_error( "real-device CaptureStore artifacts remained" );
    }
    std::cout << benchmark::serializeRealDeviceAggregateJson(
        observation, options.durationMilliseconds, options.ansiEnabled );
}

void emitFailureResult( bool realDevice )
{
    std::cout << "{\"schema_version\":1,\"benchmark\":\""
              << ( realDevice ? "ios-real-live-capture" : "synthetic-live-capture" )
              << "\",\"status\":\"failed\",\"reason_code\":\"benchmark_failed\","
                 "\"message\":\"benchmark execution failed\",\"metrics\":{}}\n";
}

void runArm( benchmark::BenchmarkArm arm, const benchmark::FramedFixture& framedFixture )
{
    QTemporaryDir captureRoot{ QStringLiteral( "klogg-live-capture-benchmark-XXXXXX" ) };
    if ( !captureRoot.isValid() ) {
        throw std::runtime_error( "could not create benchmark capture root" );
    }

    benchmark::SyntheticArmPlan plan;
    plan.arm = arm;
    plan.fixture = framedFixture;
    plan.fragmentSizes = { 1u, 2u, 3u, 5u, 8u, 13u, 21u };
    plan.captureRoot = filesystemPath( captureRoot.path() );
#ifdef KLOGG_LIVE_CAPTURE_FIXTURE_PRODUCER_PATH
    plan.producerExecutable = KLOGG_LIVE_CAPTURE_FIXTURE_PRODUCER_PATH;
#endif

    const auto observation = benchmark::runSyntheticArm( plan );
    if ( !QDir{ captureRoot.path() }.isEmpty() ) {
        throw std::runtime_error( "CaptureStore artifacts remained after benchmark arm" );
    }
    std::cout << benchmark::serializeAggregateJson( benchmark::makeAggregateResult( observation ) );
}

} // namespace

int main( int argc, char* argv[] )
{
    QCoreApplication application( argc, argv );
    const bool realDevice = argc > 1 && std::string{ argv[ 1 ] } == "--real-device";
    try {
        if ( realDevice ) {
            runRealDeviceArm( parseRealOptions( argc, argv ) );
            CaptureStore::shutdownBackgroundWorkers();
            return 0;
        }
        const auto options = parseOptions( argc, argv );
        const auto framedFixture = fixture( options );
        if ( options.arms == ArmSelection::Process || options.arms == ArmSelection::Both ) {
            runArm( benchmark::BenchmarkArm::Process, framedFixture );
        }
        if ( options.arms == ArmSelection::Integrated || options.arms == ArmSelection::Both ) {
            runArm( benchmark::BenchmarkArm::Integrated, framedFixture );
        }
        CaptureStore::shutdownBackgroundWorkers();
        return 0;
    } catch ( const std::exception& error ) {
        CaptureStore::shutdownBackgroundWorkers();
        emitFailureResult( realDevice );
        std::cerr << "live_capture_process_integrated_benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
