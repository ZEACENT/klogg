#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace klogg::testing::collector {

constexpr std::uint32_t protocolMagic = 0x4b435443U;
constexpr std::uint32_t protocolVersion = 1;
constexpr std::uint32_t infiniteWait = 0xffffffffU;
constexpr std::uint32_t captureSequence = 1;
constexpr std::uint32_t invalidProtocol = 1;
constexpr std::uint32_t collectorUnavailable = 2;
constexpr std::uint32_t invalidCaptureRequest = 3;
constexpr std::uint32_t collectorExitedBeforeAck = 4;
constexpr std::uint32_t waitTimeoutError = 258;

enum class Operation : std::uint32_t { Capture = 1, Stop = 2 };
enum class Phase : std::uint32_t { Ready = 1, Complete = 2, Stopped = 3 };
enum class ErrorDomain : std::uint32_t { None = 0, Win32 = 1, DumpHResult = 2, Protocol = 3 };
enum class Stage : std::uint32_t {
    None,
    ReadyWait,
    RequestPublish,
    Acknowledgement,
    Dump,
    ProcessWait
};
enum class ChannelWait { Acknowledged, Exited, TimedOut, Failed };
enum class ProcessWait { Signaled, TimedOut, Failed };

// These are fixed-width wire fields, not native pointers or compiler-sized enum storage.
// All native-width conversions occur only after validating the header and numeric bound.
struct Header {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t structureBytes{};
    std::uint32_t pointerBits{};
};
struct Request {
    std::uint32_t operation{};
    std::uint32_t sequence{};
    std::uint32_t threadId{};
    std::uint32_t reserved{};
    std::uint64_t exceptionPointers{};
};
struct Acknowledgement {
    std::uint32_t phase{};
    std::uint32_t sequence{};
    std::uint32_t succeeded{};
    std::uint32_t errorDomain{};
    std::uint32_t error{};
    std::uint32_t reserved{};
};
struct alignas( 8 ) SharedPage {
    Header header{};
    Request request{};
    Acknowledgement acknowledgement{};
    alignas( 4 ) std::int32_t requestPublication{};
    alignas( 4 ) std::int32_t acknowledgementPublication{};
};
struct Snapshot {
    Header header{};
    Acknowledgement acknowledgement{};
    bool publicationValid{};
};
struct Outcome {
    Stage stage{ Stage::None };
    ErrorDomain domain{ ErrorDomain::None };
    std::uint32_t error{};
    bool acknowledged{};
    bool succeeded{};
};
struct ReapResult {
    bool terminationAttempted{};
    bool terminationSucceeded{};
    std::uint32_t terminationError{};
};
struct ClientState {
    bool ready{};
    bool outputOwned{};
    bool collectorStarted{};
    bool collectorReaped{};
    bool captureRequested{};
};

static_assert( std::is_standard_layout<SharedPage>::value );
static_assert( std::is_trivially_copyable<SharedPage>::value );
static_assert( sizeof( Header ) == 16 && sizeof( Request ) == 24
               && sizeof( Acknowledgement ) == 24 );
static_assert( sizeof( SharedPage ) == 72 && alignof( SharedPage ) == 8 );
static_assert( offsetof( SharedPage, request ) == 16
               && offsetof( SharedPage, acknowledgement ) == 40 );
static_assert( offsetof( SharedPage, requestPublication ) == 64 );
static_assert( offsetof( SharedPage, acknowledgementPublication ) == 68 );

inline Header makeHeader( std::uint32_t bits ) noexcept
{
    return { protocolMagic, protocolVersion, static_cast<std::uint32_t>( sizeof( SharedPage ) ),
             bits };
}

inline bool validHeader( const Header& header, std::uint32_t nativeBits ) noexcept
{
    return ( nativeBits == 32 || nativeBits == 64 ) && header.magic == protocolMagic
           && header.version == protocolVersion && header.structureBytes == sizeof( SharedPage )
           && header.pointerBits == nativeBits;
}

inline bool validRequest( const Header& header, const Request& request,
                          std::uint32_t nativeBits ) noexcept
{
    if ( !validHeader( header, nativeBits ) || request.sequence != captureSequence
         || request.reserved != 0 ) {
        return false;
    }
    if ( request.operation == static_cast<std::uint32_t>( Operation::Stop ) ) {
        return request.threadId == 0 && request.exceptionPointers == 0;
    }
    return request.operation == static_cast<std::uint32_t>( Operation::Capture )
           && request.threadId != 0 && request.exceptionPointers != 0
           && ( nativeBits == 64
                || request.exceptionPointers <= ( std::numeric_limits<std::uint32_t>::max )() );
}

inline bool validAcknowledgement( const Snapshot& snapshot, Phase expectedPhase,
                                  std::uint32_t sequence, std::uint32_t nativeBits ) noexcept
{
    const auto& ack = snapshot.acknowledgement;
    if ( !snapshot.publicationValid || !validHeader( snapshot.header, nativeBits )
         || ack.phase != static_cast<std::uint32_t>( expectedPhase ) || ack.sequence != sequence
         || ack.reserved != 0 || ack.succeeded > 1
         || ack.errorDomain > static_cast<std::uint32_t>( ErrorDomain::Protocol ) ) {
        return false;
    }
    return ack.succeeded != 0 ? ack.errorDomain == static_cast<std::uint32_t>( ErrorDomain::None )
                                    && ack.error == 0
                              : ack.errorDomain != static_cast<std::uint32_t>( ErrorDomain::None );
}

// Command-line tokens name inherited capabilities only. No signs, paths, or truncation.
inline bool parseHandleValue( std::wstring_view token, std::uint64_t maximum,
                              std::uint64_t& value ) noexcept
{
    if ( token.empty() || token.size() > 16 ) {
        return false;
    }
    std::uint64_t parsed = 0;
    for ( const auto character : token ) {
        std::uint64_t digit = 0;
        if ( character >= L'0' && character <= L'9' ) {
            digit = static_cast<std::uint64_t>( character - L'0' );
        }
        else if ( character >= L'a' && character <= L'f' ) {
            digit = static_cast<std::uint64_t>( character - L'a' ) + 10;
        }
        else if ( character >= L'A' && character <= L'F' ) {
            digit = static_cast<std::uint64_t>( character - L'A' ) + 10;
        }
        else {
            return false;
        }
        if ( digit > maximum || parsed > ( maximum - digit ) / 16 ) {
            return false;
        }
        parsed = parsed * 16 + digit;
    }
    if ( parsed == 0 ) {
        return false;
    }
    value = parsed;
    return true;
}

inline bool shouldRemoveUnusedOutput( const ClientState& state ) noexcept
{
    return state.outputOwned && !state.captureRequested
           && ( !state.collectorStarted || state.collectorReaped );
}

template <class Ops>
Outcome awaitAcknowledgement( Ops& ops, Phase phase, std::uint32_t sequence, std::uint32_t bits,
                              std::uint32_t timeout )
{
    const auto stage = phase == Phase::Ready ? Stage::ReadyWait : Stage::Acknowledgement;
    switch ( ops.waitAckOrExit( timeout ) ) {
    case ChannelWait::TimedOut:
        return { stage, ErrorDomain::Win32, waitTimeoutError, false, false };
    case ChannelWait::Exited:
        return { stage, ErrorDomain::Protocol, collectorExitedBeforeAck, false, false };
    case ChannelWait::Failed:
        return { stage, ErrorDomain::Win32, ops.lastError(), false, false };
    case ChannelWait::Acknowledged:
        break;
    }
    const auto snapshot = ops.readAcknowledgement();
    if ( !validAcknowledgement( snapshot, phase, sequence, bits ) ) {
        return { stage, ErrorDomain::Protocol, invalidProtocol, false, false };
    }
    const auto& ack = snapshot.acknowledgement;
    const auto succeeded = ack.succeeded != 0;
    return { succeeded ? Stage::None : ( phase == Phase::Complete ? Stage::Dump : stage ),
             static_cast<ErrorDomain>( ack.errorDomain ), ack.error, true, succeeded };
}

// A process-signaled observation is the only release barrier. Termination is asynchronous.
// There deliberately is no finite reap timeout that returns to the exception dispatcher.
// An invalid process wait is a fail-stop invariant breach, never a best-effort continuation.
// Ops::failStop must not return; tests throw a sentinel instead of terminating their runner.
template <class Ops>
ReapResult reapCollector( Ops& ops, std::uint32_t grace )
{
    ReapResult result;
    auto observed = ops.waitProcess( grace );
    if ( observed == ProcessWait::Failed ) {
        ops.failStop( Stage::ProcessWait, ops.lastError() );
    }
    if ( observed != ProcessWait::Signaled ) {
        result.terminationAttempted = true;
        result.terminationSucceeded = ops.terminateCollector();
        result.terminationError = result.terminationSucceeded ? 0 : ops.lastError();
        do {
            observed = ops.waitProcess( infiniteWait );
            if ( observed == ProcessWait::Failed ) {
                ops.failStop( Stage::ProcessWait, ops.lastError() );
            }
        } while ( observed != ProcessWait::Signaled );
    }
    ops.reportReap( result );
    return result;
}

// Reaching the safety barrier after forced termination does not satisfy the
// normal-exit deadline. Preserve any earlier API/protocol failure as primary;
// Ops::reportReap already exposes termination success/error as secondary evidence.
inline Outcome includeReapFailure( Outcome outcome, const ReapResult& reap ) noexcept
{
    if ( reap.terminationAttempted && ( outcome.succeeded || outcome.stage == Stage::None ) ) {
        return { Stage::ProcessWait, ErrorDomain::Win32, waitTimeoutError, outcome.acknowledged,
                 false };
    }
    return outcome;
}

template <class Ops>
Outcome captureOnce( ClientState& state, Ops& ops, const Request& request, std::uint32_t bits,
                     std::uint32_t timeout, std::uint32_t grace )
{
    if ( !state.ready || !state.collectorStarted || state.collectorReaped
         || state.captureRequested ) {
        return { Stage::RequestPublish, ErrorDomain::Protocol, collectorUnavailable, false, false };
    }
    // Conservative publication ownership: even SetEvent failure retains requested evidence.
    state.captureRequested = true;
    state.ready = false;
    Outcome outcome;
    if ( !validRequest( makeHeader( bits ), request, bits )
         || request.operation != static_cast<std::uint32_t>( Operation::Capture ) ) {
        outcome
            = { Stage::RequestPublish, ErrorDomain::Protocol, invalidCaptureRequest, false, false };
    }
    else if ( !ops.publishRequest( request ) ) {
        outcome = { Stage::RequestPublish, ErrorDomain::Win32, ops.lastError(), false, false };
    }
    else {
        outcome = awaitAcknowledgement( ops, Phase::Complete, captureSequence, bits, timeout );
    }
    const auto reap = reapCollector( ops, grace );
    state.collectorReaped = true;
    return includeReapFailure( outcome, reap );
}

// The VEH's SEH finally block uses this barrier if diagnostics unwind abnormally
// after publication. Normal completion has already reaped and is a no-op here.
template <class Ops>
void quiesceRequestedCollector( ClientState& state, Ops& ops, std::uint32_t grace )
{
    if ( state.captureRequested && state.collectorStarted && !state.collectorReaped ) {
        static_cast<void>( reapCollector( ops, grace ) );
        state.collectorReaped = true;
        state.ready = false;
    }
}

// The real client's startup unwind and atexit path both use this exact cleanup seam.
// Its caller holds the exclusive lifecycle lock, excluding concurrent VEH resource use.
template <class Ops>
Outcome shutdownCollector( ClientState& state, Ops& ops, std::uint32_t bits,
                           std::uint32_t stopTimeout, std::uint32_t grace )
{
    Outcome stopped;
    if ( state.collectorStarted && !state.collectorReaped ) {
        if ( state.ready && !state.captureRequested ) {
            const Request stop{ static_cast<std::uint32_t>( Operation::Stop ), captureSequence, 0,
                                0, 0 };
            if ( ops.publishRequest( stop ) ) {
                stopped = awaitAcknowledgement( ops, Phase::Stopped, captureSequence, bits,
                                                stopTimeout );
            }
            else {
                stopped
                    = { Stage::RequestPublish, ErrorDomain::Win32, ops.lastError(), false, false };
            }
        }
        const auto reap = reapCollector( ops, grace );
        state.collectorReaped = true;
        stopped = includeReapFailure( stopped, reap );
    }
    state.ready = false;
    if ( shouldRemoveUnusedOutput( state ) ) {
        ops.removeUnusedOutput();
    }
    ops.closeResources();
    state.outputOwned = false;
    state.collectorStarted = false;
    return stopped;
}

} // namespace klogg::testing::collector
