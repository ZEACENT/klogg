#include <catch2/catch.hpp>

#include "../helpers/crash_collector_protocol.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace klogg::testing::collector;

namespace {
struct FailStop : std::runtime_error {
    FailStop()
        : std::runtime_error( "fail-stop" )
    {
    }
};

Snapshot successful( Phase phase = Phase::Complete, std::uint32_t bits = 64 )
{
    return { makeHeader( bits ),
             { static_cast<std::uint32_t>( phase ), phase == Phase::Ready ? 0U : captureSequence, 1,
               0, 0, 0 },
             true };
}

Request capture( std::uint64_t pointer = 0x12345678U )
{
    return { static_cast<std::uint32_t>( Operation::Capture ), captureSequence, 42, 0, pointer };
}

ClientState active()
{
    return { true, true, true, false, false };
}

struct FakeOps {
    ChannelWait channelResult{ ChannelWait::Acknowledged };
    Snapshot snapshot{ successful() };
    std::vector<ProcessWait> waits{ ProcessWait::Signaled };
    std::vector<std::string> calls;
    std::size_t nextWait{};
    bool publishSucceeds{ true };
    bool publishThrows{};
    bool terminateSucceeds{ true };
    std::uint32_t error{ 123 };
    std::function<void()> waitingCheckpoint;
    Request published{};

    ChannelWait waitAckOrExit( std::uint32_t timeout )
    {
        calls.emplace_back( "ack.wait." + std::to_string( timeout ) );
        return channelResult;
    }
    Snapshot readAcknowledgement()
    {
        calls.emplace_back( "ack.read" );
        return snapshot;
    }
    bool publishRequest( const Request& request )
    {
        calls.emplace_back( "publish" );
        published = request;
        if ( publishThrows ) {
            throw std::runtime_error( "simulated abnormal handler unwind after publication" );
        }
        return publishSucceeds;
    }
    ProcessWait waitProcess( std::uint32_t timeout )
    {
        calls.emplace_back( "process.wait." + std::to_string( timeout ) );
        if ( nextWait >= waits.size() ) {
            throw std::runtime_error( "unexpected process wait" );
        }
        const auto result = waits[ nextWait++ ];
        if ( result == ProcessWait::TimedOut && waitingCheckpoint ) {
            waitingCheckpoint();
        }
        return result;
    }
    bool terminateCollector()
    {
        calls.emplace_back( "terminate" );
        return terminateSucceeds;
    }
    std::uint32_t lastError() const
    {
        return error;
    }
    [[noreturn]] void failStop( Stage stage, std::uint32_t rawError )
    {
        calls.emplace_back( "fail-stop" );
        CHECK( stage == Stage::ProcessWait );
        CHECK( rawError == error );
        throw FailStop{};
    }
    void reportReap( const ReapResult& result )
    {
        calls.emplace_back( "reaped" );
        if ( result.terminationAttempted ) {
            CHECK( result.terminationSucceeded == terminateSucceeds );
            CHECK( result.terminationError == ( terminateSucceeds ? 0U : error ) );
        }
    }
    void removeUnusedOutput()
    {
        calls.emplace_back( "output.delete" );
    }
    void closeResources()
    {
        calls.emplace_back( "resources.close" );
    }
};
} // namespace

TEST_CASE( "Protocol has a fixed x86 and x64 representation" )
{
    CHECK( sizeof( Header ) == 16 );
    CHECK( sizeof( Request ) == 24 );
    CHECK( sizeof( Acknowledgement ) == 24 );
    CHECK( sizeof( SharedPage ) == 72 );
    CHECK( offsetof( SharedPage, request ) == 16 );
    CHECK( offsetof( SharedPage, acknowledgement ) == 40 );
    CHECK( offsetof( SharedPage, requestPublication ) == 64 );
    CHECK( offsetof( SharedPage, acknowledgementPublication ) == 68 );
    CHECK( std::is_trivially_copyable<SharedPage>::value );
}

TEST_CASE( "Header rejects mismatched version size magic and architecture" )
{
    for ( const auto bits : { 32U, 64U } ) {
        auto header = makeHeader( bits );
        CHECK( validHeader( header, bits ) );
        CHECK_FALSE( validHeader( header, bits == 32 ? 64U : 32U ) );
        CHECK_FALSE( validHeader( header, 16 ) );
        header.magic ^= 1;
        CHECK_FALSE( validHeader( header, bits ) );
        header = makeHeader( bits );
        ++header.version;
        CHECK_FALSE( validHeader( header, bits ) );
        header = makeHeader( bits );
        --header.structureBytes;
        CHECK_FALSE( validHeader( header, bits ) );
    }
}

TEST_CASE(
    "Request validates operation sequence and remote native pointer without copying context" )
{
    for ( const auto bits : { 32U, 64U } ) {
        const auto header = makeHeader( bits );
        auto request = capture();
        CHECK( validRequest( header, request, bits ) );
        request.exceptionPointers = 0;
        CHECK_FALSE( validRequest( header, request, bits ) );
        request = capture();
        request.threadId = 0;
        CHECK_FALSE( validRequest( header, request, bits ) );
        request = capture();
        request.sequence = 2;
        CHECK_FALSE( validRequest( header, request, bits ) );
        request = capture();
        request.reserved = 1;
        CHECK_FALSE( validRequest( header, request, bits ) );
        request = capture();
        request.operation = 99;
        CHECK_FALSE( validRequest( header, request, bits ) );
        request = { static_cast<std::uint32_t>( Operation::Stop ), 1, 0, 0, 0 };
        CHECK( validRequest( header, request, bits ) );
        request.exceptionPointers = 1;
        CHECK_FALSE( validRequest( header, request, bits ) );
    }
    CHECK_FALSE( validRequest( makeHeader( 32 ), capture( 0x100000000ULL ), 32 ) );
    CHECK( validRequest( makeHeader( 32 ), capture( 0xffffffffULL ), 32 ) );
    CHECK( validRequest( makeHeader( 64 ), capture( 0x100000000ULL ), 64 ) );
}

TEST_CASE(
    "Acknowledgements reject stale ready wrong sequence uncommitted data and inconsistent status" )
{
    auto snapshot = successful();
    CHECK( validAcknowledgement( snapshot, Phase::Complete, 1, 64 ) );
    CHECK_FALSE( validAcknowledgement( snapshot, Phase::Ready, 0, 64 ) );
    snapshot.acknowledgement.sequence = 2;
    CHECK_FALSE( validAcknowledgement( snapshot, Phase::Complete, 1, 64 ) );
    snapshot = successful();
    snapshot.publicationValid = false;
    CHECK_FALSE( validAcknowledgement( snapshot, Phase::Complete, 1, 64 ) );
    snapshot = successful();
    snapshot.acknowledgement.succeeded = 2;
    CHECK_FALSE( validAcknowledgement( snapshot, Phase::Complete, 1, 64 ) );
    snapshot = successful();
    snapshot.acknowledgement.error = 5;
    CHECK_FALSE( validAcknowledgement( snapshot, Phase::Complete, 1, 64 ) );
    snapshot = successful();
    snapshot.acknowledgement.reserved = 1;
    CHECK_FALSE( validAcknowledgement( snapshot, Phase::Complete, 1, 64 ) );
    snapshot = successful();
    snapshot.acknowledgement.errorDomain = 99;
    CHECK_FALSE( validAcknowledgement( snapshot, Phase::Complete, 1, 64 ) );
}

TEST_CASE( "Inherited handle parsing is bounded hexadecimal only" )
{
    std::uint64_t value = 0;
    CHECK( parseHandleValue( L"abcd", 0xffffffffULL, value ) );
    CHECK( value == 0xabcd );
    CHECK( parseHandleValue( L"ffffffff", 0xffffffffULL, value ) );
    CHECK_FALSE( parseHandleValue( L"100000000", 0xffffffffULL, value ) );
    CHECK( parseHandleValue( L"100000000", std::numeric_limits<std::uint64_t>::max(), value ) );
    CHECK_FALSE( parseHandleValue( L"0", 0xffffffffULL, value ) );
    CHECK_FALSE( parseHandleValue( L"-1", 0xffffffffULL, value ) );
    CHECK_FALSE( parseHandleValue( L"+a", 0xffffffffULL, value ) );
    CHECK_FALSE( parseHandleValue( L" abc", 0xffffffffULL, value ) );
    CHECK_FALSE( parseHandleValue( L"abc/path", 0xffffffffULL, value ) );
    CHECK_FALSE( parseHandleValue( L"fffffffffffffffff", std::numeric_limits<std::uint64_t>::max(),
                                   value ) );
    CHECK_FALSE( parseHandleValue( L"", 0xffffffffULL, value ) );
}

TEST_CASE( "Successful capture returns only after observed collector exit" )
{
    auto state = active();
    FakeOps ops;
    const auto outcome = captureOnce( state, ops, capture(), 64, 100, 10 );
    CHECK( outcome.succeeded );
    CHECK( outcome.acknowledged );
    CHECK( state.captureRequested );
    CHECK( state.collectorReaped );
    CHECK_FALSE( state.ready );
    CHECK( ops.published.exceptionPointers == 0x12345678 );
    CHECK( ops.calls
           == std::vector<std::string>{ "publish", "ack.wait.100", "ack.read", "process.wait.10",
                                        "reaped" } );
}

TEST_CASE( "Request timeout never consumes late success and requires terminate then signaled reap" )
{
    auto state = active();
    FakeOps ops;
    ops.channelResult = ChannelWait::TimedOut;
    ops.waits = { ProcessWait::TimedOut, ProcessWait::TimedOut, ProcessWait::Signaled };
    bool returned = false;
    ops.waitingCheckpoint = [ & ] {
        CHECK_FALSE( returned );
        CHECK_FALSE( state.collectorReaped );
        CHECK( state.captureRequested );
    };
    const auto outcome = captureOnce( state, ops, capture(), 64, 100, 10 );
    returned = true;
    CHECK_FALSE( outcome.succeeded );
    CHECK_FALSE( outcome.acknowledged );
    CHECK( state.collectorReaped );
    CHECK( ops.nextWait == 3 );
    CHECK( ops.calls
           == std::vector<std::string>{ "publish", "ack.wait.100", "process.wait.10", "terminate",
                                        "process.wait.4294967295", "process.wait.4294967295",
                                        "reaped" } );
}

TEST_CASE( "Wrong acknowledgement fails capture and reaps instead of accepting a ready or "
           "unrelated result" )
{
    for ( const auto wrongReady : { false, true } ) {
        auto state = active();
        FakeOps ops;
        if ( wrongReady ) {
            ops.snapshot = successful( Phase::Ready );
        }
        else {
            ops.snapshot.acknowledgement.sequence = 2;
        }
        ops.waits = { ProcessWait::TimedOut, ProcessWait::Signaled };
        const auto outcome = captureOnce( state, ops, capture(), 64, 100, 10 );
        CHECK_FALSE( outcome.succeeded );
        CHECK( outcome.domain == ErrorDomain::Protocol );
        CHECK( state.collectorReaped );
        CHECK( ops.calls[ 4 ] == "terminate" );
    }
}

TEST_CASE( "Collector exit before acknowledgement is failure with no termination retry" )
{
    auto state = active();
    FakeOps ops;
    ops.channelResult = ChannelWait::Exited;
    const auto outcome = captureOnce( state, ops, capture(), 64, 100, 10 );
    CHECK_FALSE( outcome.succeeded );
    CHECK_FALSE( outcome.acknowledged );
    CHECK( state.collectorReaped );
    CHECK( ops.calls
           == std::vector<std::string>{ "publish", "ack.wait.100", "process.wait.10", "reaped" } );
}

TEST_CASE( "Acknowledgement wait failure still terminates and safely reaps" )
{
    auto state = active();
    FakeOps ops;
    ops.channelResult = ChannelWait::Failed;
    ops.waits = { ProcessWait::TimedOut, ProcessWait::Signaled };
    const auto outcome = captureOnce( state, ops, capture(), 64, 100, 10 );
    CHECK_FALSE( outcome.succeeded );
    CHECK( outcome.error == 123 );
    CHECK( state.collectorReaped );
    CHECK( ops.calls[ 3 ] == "terminate" );
}

TEST_CASE( "Publication failure remains one shot and retains output after mandatory reap" )
{
    auto state = active();
    FakeOps ops;
    ops.publishSucceeds = false;
    const auto outcome = captureOnce( state, ops, capture(), 64, 100, 10 );
    CHECK_FALSE( outcome.succeeded );
    CHECK( outcome.stage == Stage::RequestPublish );
    CHECK( state.captureRequested );
    CHECK( state.collectorReaped );
    CHECK_FALSE( shouldRemoveUnusedOutput( state ) );
    CHECK( ops.calls == std::vector<std::string>{ "publish", "process.wait.10", "reaped" } );
}

TEST_CASE( "A claimed or unready collector cannot publish another request" )
{
    for ( const auto alreadyRequested : { false, true } ) {
        auto state = active();
        state.ready = alreadyRequested;
        state.captureRequested = alreadyRequested;
        FakeOps ops;
        const auto outcome = captureOnce( state, ops, capture(), 64, 100, 10 );
        CHECK_FALSE( outcome.succeeded );
        CHECK( ops.calls.empty() );
    }
}

TEST_CASE( "A dump HRESULT remains raw failure evidence rather than a claimed capture" )
{
    auto state = active();
    FakeOps ops;
    ops.snapshot.acknowledgement.succeeded = 0;
    ops.snapshot.acknowledgement.errorDomain
        = static_cast<std::uint32_t>( ErrorDomain::DumpHResult );
    ops.snapshot.acknowledgement.error = 0x80070005U;
    const auto outcome = captureOnce( state, ops, capture(), 64, 100, 10 );
    CHECK_FALSE( outcome.succeeded );
    CHECK( outcome.acknowledged );
    CHECK( outcome.domain == ErrorDomain::DumpHResult );
    CHECK( outcome.error == 0x80070005U );
    CHECK_FALSE( shouldRemoveUnusedOutput( state ) );
}

TEST_CASE( "Successful capture acknowledgement followed by exit timeout reports failure after "
           "mandatory reap" )
{
    auto state = active();
    FakeOps ops;
    ops.waits = { ProcessWait::TimedOut, ProcessWait::Signaled };
    const auto outcome = captureOnce( state, ops, capture(), 64, 100, 10 );
    CHECK_FALSE( outcome.succeeded );
    CHECK( outcome.acknowledged );
    CHECK( outcome.stage == Stage::ProcessWait );
    CHECK( outcome.domain == ErrorDomain::Win32 );
    CHECK( outcome.error == waitTimeoutError );
    CHECK( state.collectorReaped );
    CHECK_FALSE( shouldRemoveUnusedOutput( state ) );
    CHECK( ops.calls[ 4 ] == "terminate" );
    CHECK( ops.calls.back() == "reaped" );
}

TEST_CASE( "Successful stop acknowledgement followed by exit timeout reports failure after "
           "mandatory reap" )
{
    auto state = active();
    FakeOps ops;
    ops.snapshot = successful( Phase::Stopped );
    ops.waits = { ProcessWait::TimedOut, ProcessWait::Signaled };
    const auto stopped = shutdownCollector( state, ops, 64, 50, 10 );
    CHECK_FALSE( stopped.succeeded );
    CHECK( stopped.acknowledged );
    CHECK( stopped.stage == Stage::ProcessWait );
    CHECK( stopped.domain == ErrorDomain::Win32 );
    CHECK( stopped.error == waitTimeoutError );
    CHECK( state.collectorReaped );
    CHECK_FALSE( state.captureRequested );
    CHECK( ops.calls
           == std::vector<std::string>{ "publish", "ack.wait.50", "ack.read", "process.wait.10",
                                        "terminate", "process.wait.4294967295", "reaped",
                                        "output.delete", "resources.close" } );
}

TEST_CASE( "Exit timeout preserves earlier capture and stop API or protocol failures" )
{
    for ( const auto captureRequest : { false, true } ) {
        for ( const auto protocolFailure : { false, true } ) {
            auto state = active();
            FakeOps ops;
            ops.snapshot = successful( captureRequest ? Phase::Complete : Phase::Stopped );
            const auto primaryDomain = protocolFailure ? ErrorDomain::Protocol
                                                       : ( captureRequest ? ErrorDomain::DumpHResult
                                                                          : ErrorDomain::Win32 );
            const auto primaryError
                = protocolFailure ? invalidProtocol : ( captureRequest ? 0x80070005U : 5U );
            if ( protocolFailure ) {
                ops.snapshot.acknowledgement.sequence = 2;
            }
            else {
                ops.snapshot.acknowledgement.succeeded = 0;
                ops.snapshot.acknowledgement.errorDomain
                    = static_cast<std::uint32_t>( primaryDomain );
                ops.snapshot.acknowledgement.error = primaryError;
            }
            ops.waits = { ProcessWait::TimedOut, ProcessWait::Signaled };
            const auto outcome = captureRequest ? captureOnce( state, ops, capture(), 64, 100, 10 )
                                                : shutdownCollector( state, ops, 64, 50, 10 );
            CHECK_FALSE( outcome.succeeded );
            CHECK( outcome.acknowledged == !protocolFailure );
            CHECK(
                outcome.stage
                == ( captureRequest && !protocolFailure ? Stage::Dump : Stage::Acknowledgement ) );
            CHECK( outcome.domain == primaryDomain );
            CHECK( outcome.error == primaryError );
            CHECK( state.collectorReaped );
        }
    }
}

TEST_CASE(
    "Termination failure still requires signaled exit for already-exited and live collector" )
{
    for ( const auto stillLive : { false, true } ) {
        auto state = active();
        FakeOps ops;
        ops.channelResult = ChannelWait::TimedOut;
        ops.terminateSucceeds = false;
        ops.waits = stillLive
                        ? std::vector<ProcessWait>{ ProcessWait::TimedOut, ProcessWait::TimedOut,
                                                    ProcessWait::Signaled }
                        : std::vector<ProcessWait>{ ProcessWait::TimedOut, ProcessWait::Signaled };
        const auto outcome = captureOnce( state, ops, capture(), 64, 100, 10 );
        CHECK_FALSE( outcome.succeeded );
        CHECK( state.collectorReaped );
        CHECK( ops.nextWait == ops.waits.size() );
        CHECK( ops.calls.back() == "reaped" );
    }
}

TEST_CASE( "Unexpected process wait failure fail-stops without releasing original pointers" )
{
    for ( const auto afterTermination : { false, true } ) {
        auto state = active();
        FakeOps ops;
        ops.channelResult = ChannelWait::TimedOut;
        ops.waits = afterTermination
                        ? std::vector<ProcessWait>{ ProcessWait::TimedOut, ProcessWait::Failed }
                        : std::vector<ProcessWait>{ ProcessWait::Failed };
        CHECK_THROWS_AS( captureOnce( state, ops, capture(), 64, 100, 10 ), FailStop );
        CHECK_FALSE( state.collectorReaped );
        CHECK( state.captureRequested );
        CHECK( ops.calls.back() == "fail-stop" );
    }
}

TEST_CASE( "Output cleanup removes only owned unused output after collector exit or no launch" )
{
    ClientState state;
    CHECK_FALSE( shouldRemoveUnusedOutput( state ) );
    state.outputOwned = true;
    CHECK( shouldRemoveUnusedOutput( state ) );
    state.collectorStarted = true;
    CHECK_FALSE( shouldRemoveUnusedOutput( state ) );
    state.collectorReaped = true;
    CHECK( shouldRemoveUnusedOutput( state ) );
    state.captureRequested = true;
    CHECK_FALSE( shouldRemoveUnusedOutput( state ) );
    state.outputOwned = false;
    CHECK_FALSE( shouldRemoveUnusedOutput( state ) );
}

TEST_CASE( "Ready handshake validates readiness and never calls it dump capture" )
{
    FakeOps ops;
    ops.snapshot = successful( Phase::Ready );
    const auto ready = awaitAcknowledgement( ops, Phase::Ready, 0, 64, 50 );
    CHECK( ready.succeeded );
    CHECK( ready.acknowledged );
    CHECK( ops.calls == std::vector<std::string>{ "ack.wait.50", "ack.read" } );
    ops.snapshot.header.pointerBits = 32;
    const auto mismatch = awaitAcknowledgement( ops, Phase::Ready, 0, 64, 50 );
    CHECK_FALSE( mismatch.succeeded );
    CHECK( mismatch.domain == ErrorDomain::Protocol );
}

TEST_CASE( "Idle shutdown stops reaps deletes owned unused file then closes resources" )
{
    auto state = active();
    FakeOps ops;
    ops.snapshot = successful( Phase::Stopped );
    const auto stopped = shutdownCollector( state, ops, 64, 50, 10 );
    CHECK( stopped.succeeded );
    CHECK( state.collectorReaped );
    CHECK_FALSE( state.captureRequested );
    CHECK_FALSE( state.ready );
    CHECK( ops.published.operation == static_cast<std::uint32_t>( Operation::Stop ) );
    CHECK( ops.published.exceptionPointers == 0 );
    CHECK( ops.calls
           == std::vector<std::string>{ "publish", "ack.wait.50", "ack.read", "process.wait.10",
                                        "reaped", "output.delete", "resources.close" } );
}

TEST_CASE( "Startup failure reaps before deleting only its owned unused output" )
{
    for ( const auto launched : { false, true } ) {
        ClientState state;
        state.outputOwned = true;
        state.collectorStarted = launched;
        FakeOps ops;
        ops.waits = { ProcessWait::TimedOut, ProcessWait::Signaled };
        static_cast<void>( shutdownCollector( state, ops, 64, 50, 10 ) );
        if ( launched ) {
            CHECK( state.collectorReaped );
            CHECK( ops.calls
                   == std::vector<std::string>{ "process.wait.10", "terminate",
                                                "process.wait.4294967295", "reaped",
                                                "output.delete", "resources.close" } );
        }
        else {
            CHECK( ops.calls == std::vector<std::string>{ "output.delete", "resources.close" } );
        }
    }
}

TEST_CASE( "Requested failure cleanup preserves partial output and collision owns nothing" )
{
    for ( const auto requested : { false, true } ) {
        ClientState state;
        state.outputOwned = requested;
        state.collectorStarted = requested;
        state.captureRequested = requested;
        FakeOps ops;
        static_cast<void>( shutdownCollector( state, ops, 64, 50, 10 ) );
        if ( requested ) {
            CHECK( ops.calls
                   == std::vector<std::string>{ "process.wait.10", "reaped", "resources.close" } );
        }
        else {
            CHECK( ops.calls == std::vector<std::string>{ "resources.close" } );
        }
    }
}

TEST_CASE(
    "Abnormal handler unwind after publication quiesces collector before releasing pointers" )
{
    auto state = active();
    FakeOps ops;
    ops.publishThrows = true;
    ops.waits = { ProcessWait::TimedOut, ProcessWait::Signaled };
    CHECK_THROWS_AS( captureOnce( state, ops, capture(), 64, 100, 10 ), std::runtime_error );
    CHECK( state.captureRequested );
    CHECK_FALSE( state.collectorReaped );
    quiesceRequestedCollector( state, ops, 10 );
    CHECK( state.collectorReaped );
    CHECK_FALSE( state.ready );
    CHECK( ops.calls
           == std::vector<std::string>{ "publish", "process.wait.10", "terminate",
                                        "process.wait.4294967295", "reaped" } );
    quiesceRequestedCollector( state, ops, 10 );
    CHECK( ops.nextWait == 2 );
}

TEST_CASE( "Actual capture seam preserves x86 and x64 pointer values and rejects invalid request "
           "before publication" )
{
    for ( const auto bits : { 32U, 64U } ) {
        auto state = active();
        FakeOps ops;
        ops.snapshot = successful( Phase::Complete, bits );
        const auto pointer = bits == 32 ? 0xfedcba98ULL : 0x1234fedcba98ULL;
        const auto result = captureOnce( state, ops, capture( pointer ), bits, 100, 10 );
        CHECK( result.succeeded );
        CHECK( ops.published.exceptionPointers == pointer );
        CHECK( ops.published.threadId == 42 );
    }
    auto state = active();
    FakeOps ops;
    const auto rejected = captureOnce( state, ops, capture( 0x100000000ULL ), 32, 100, 10 );
    CHECK_FALSE( rejected.succeeded );
    CHECK( rejected.error == invalidCaptureRequest );
    CHECK( state.captureRequested );
    CHECK( state.collectorReaped );
    CHECK( ops.calls == std::vector<std::string>{ "process.wait.10", "reaped" } );
}

TEST_CASE( "Idle stop timeout still reaps before deleting unused output and never reports stopped "
           "success" )
{
    auto state = active();
    FakeOps ops;
    ops.channelResult = ChannelWait::TimedOut;
    ops.waits = { ProcessWait::TimedOut, ProcessWait::Signaled };
    const auto stopped = shutdownCollector( state, ops, 64, 50, 10 );
    CHECK_FALSE( stopped.succeeded );
    CHECK( state.collectorReaped );
    CHECK_FALSE( state.captureRequested );
    CHECK( ops.calls
           == std::vector<std::string>{ "publish", "ack.wait.50", "process.wait.10", "terminate",
                                        "process.wait.4294967295", "reaped", "output.delete",
                                        "resources.close" } );
}

TEST_CASE( "Cleanup process-wait failure fail-stops before deleting or closing owned resources" )
{
    auto state = active();
    state.ready = false;
    FakeOps ops;
    ops.waits = { ProcessWait::Failed };
    CHECK_THROWS_AS( shutdownCollector( state, ops, 64, 50, 10 ), FailStop );
    CHECK_FALSE( state.collectorReaped );
    CHECK( ops.calls == std::vector<std::string>{ "process.wait.10", "fail-stop" } );
}
