#pragma once

#include <array>
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

// Optional observations use a separately versioned trailer. The functional page stays v1.
constexpr std::uint32_t diagnosticMagic = 0x4b444941U;
constexpr std::uint32_t diagnosticVersion = 1;
constexpr std::size_t diagnosticBytes = 16U * 1024U;
constexpr std::uint32_t maximumDiagnosticReads = 16;
constexpr std::uint32_t maximumDiagnosticReadBytes = 4U * 1024U;
constexpr std::uint32_t maximumDiagnosticQueries = 32;
constexpr std::uint32_t maximumExceptionChain = 3;
constexpr std::uint32_t maximumStackSample = 256;
constexpr std::size_t maximumDiagnosticRecords = 48;
constexpr std::size_t diagnosticModulesPerProcess = 3;
constexpr std::size_t maximumDiagnosticPath = 512;
constexpr std::uint32_t diagnosticTruncated = 1;
constexpr std::uint32_t diagnosticUnavailable = 2;
constexpr std::uint32_t diagnosticCycle = 4;
constexpr std::uint32_t stackLimitsAvailable = 1;
constexpr std::uint32_t pointersDecoded = 1;
constexpr std::uint32_t exceptionDecoded = 2;
constexpr std::uint32_t contextDecoded = 4;

enum class DiagnosticStatus : std::uint32_t {
    Unavailable, Loaded, Absent, Truncated, Error, Complete, Partial, Skipped, Budget, Cycle,
    InvalidRange
};
enum class DiagnosticKind : std::uint32_t { Read = 1, Region = 2 };
enum class DiagnosticObject : std::uint32_t { Pointers = 1, Exception, Context, Stack };
enum class DiagnosticProcess : std::uint32_t { Target = 1, Collector = 2 };
enum class DiagnosticModule : std::uint32_t { DbgHelp = 1, DbgCore = 2, Asan = 3 };

struct DiagnosticHeader {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t structureBytes{};
    std::uint32_t pointerBits{};
    std::uint32_t reserved{};
};
struct alignas( 8 ) DiagnosticInput {
    std::uint32_t sequence{};
    std::uint32_t threadId{};
    std::uint32_t contextFlags{};
    std::uint32_t flags{};
    std::uint64_t exceptionPointers{};
    std::uint64_t exceptionRecord{};
    std::uint64_t contextRecord{};
    std::uint64_t pc{};
    std::uint64_t sp{};
    std::uint64_t fp{};
    std::uint64_t stackLow{};
    std::uint64_t stackHigh{};
};
struct alignas( 8 ) DiagnosticModuleRecord {
    std::uint32_t process{};
    std::uint32_t name{};
    std::uint32_t status{};
    std::uint32_t error{};
    std::uint32_t pathLength{};
    std::array<std::uint32_t, 3> reserved{};
    std::array<std::uint16_t, maximumDiagnosticPath> path{};
};
struct alignas( 8 ) DiagnosticRecord {
    std::uint32_t kind{};
    std::uint32_t object{};
    std::uint32_t order{};
    std::uint32_t status{};
    std::uint64_t address{};
    std::uint64_t requested{};
    std::uint64_t transferred{};
    std::uint64_t regionBase{};
    std::uint64_t regionBytes{};
    std::uint32_t error{};
    std::uint32_t state{};
    std::uint32_t protection{};
    std::uint32_t type{};
    std::array<std::uint32_t, 2> reserved{};
};
struct alignas( 8 ) DiagnosticResult {
    std::uint32_t sequence{};
    std::uint32_t threadId{};
    std::uint32_t dumpSucceeded{};
    std::uint32_t dumpDomain{};
    std::uint32_t dumpError{};
    std::uint32_t closeSucceeded{};
    std::uint32_t closeError{};
    std::uint32_t recordCount{};
    std::uint32_t readCalls{};
    std::uint32_t queryCalls{};
    std::uint32_t requestedBytes{};
    std::uint32_t flags{};
    std::uint32_t contextFlags{};
    std::uint32_t decoded{};
    std::uint64_t exceptionRecord{};
    std::uint64_t contextRecord{};
    std::uint64_t pc{};
    std::uint64_t sp{};
    std::uint64_t fp{};
};
struct alignas( 8 ) DiagnosticData {
    DiagnosticHeader header{};
    DiagnosticInput input{};
    std::array<DiagnosticModuleRecord, diagnosticModulesPerProcess> targetModules{};
    DiagnosticResult result{};
    std::array<DiagnosticRecord, maximumDiagnosticRecords> records{};
    std::array<DiagnosticModuleRecord, diagnosticModulesPerProcess> collectorModules{};
    alignas( 4 ) std::int32_t inputPublication{};
    alignas( 4 ) std::int32_t resultPublication{};
};
static_assert( sizeof( DiagnosticData ) <= diagnosticBytes );
struct alignas( 8 ) DiagnosticStorage {
    DiagnosticData data{};
    std::array<std::uint8_t, diagnosticBytes - sizeof( DiagnosticData )> padding{};
};
struct alignas( 8 ) SharedEnvelope {
    SharedPage page{};
    DiagnosticStorage diagnostics{};
};
static_assert( sizeof( DiagnosticInput ) == 80 );
static_assert( sizeof( DiagnosticModuleRecord ) == 1056 );
static_assert( sizeof( DiagnosticRecord ) == 80 );
static_assert( sizeof( DiagnosticResult ) == 96 );
static_assert( sizeof( DiagnosticStorage ) == diagnosticBytes );
static_assert( offsetof( SharedEnvelope, diagnostics ) == 72 );
static_assert( sizeof( SharedEnvelope ) == 72 + diagnosticBytes );
static_assert( std::is_standard_layout<SharedEnvelope>::value );
static_assert( std::is_trivially_copyable<SharedEnvelope>::value );

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

inline DiagnosticHeader makeDiagnosticHeader( std::uint32_t bits ) noexcept
{
    return { diagnosticMagic, diagnosticVersion,
             static_cast<std::uint32_t>( sizeof( DiagnosticData ) ), bits, 0 };
}

inline bool validDiagnosticHeader( const DiagnosticHeader& header, std::uint32_t bits ) noexcept
{
    return ( bits == 32 || bits == 64 ) && header.magic == diagnosticMagic
           && header.version == diagnosticVersion && header.structureBytes == sizeof( DiagnosticData )
           && header.pointerBits == bits && header.reserved == 0;
}

inline bool validDiagnosticAddress( std::uint64_t address, std::uint32_t bits ) noexcept
{
    return bits == 64 || ( bits == 32 && address <= 0xffffffffULL );
}

inline bool validDiagnosticRange( std::uint64_t address, std::uint64_t bytes,
                                 std::uint32_t bits ) noexcept
{
    const auto maximum = bits == 32 ? 0xffffffffULL : ( std::numeric_limits<std::uint64_t>::max )();
    return ( bits == 32 || bits == 64 ) && address != 0 && bytes != 0 && address <= maximum
           && bytes <= maximum - address;
}

inline bool validDiagnosticModule( const DiagnosticModuleRecord& module,
                                  DiagnosticProcess process ) noexcept
{
    if ( module.process != static_cast<std::uint32_t>( process ) || module.name < 1
         || module.name > 3 || module.status > static_cast<std::uint32_t>( DiagnosticStatus::Error )
         || module.pathLength > maximumDiagnosticPath ) {
        return false;
    }
    for ( const auto reserved : module.reserved ) {
        if ( reserved != 0 ) {
            return false;
        }
    }
    if ( module.status == static_cast<std::uint32_t>( DiagnosticStatus::Loaded )
         && module.pathLength == 0 ) {
        return false;
    }
    for ( std::size_t index = 0; index < module.pathLength; ++index ) {
        if ( module.path[ index ] == 0 ) {
            return false;
        }
    }
    return true;
}

inline bool validDiagnosticInput( const DiagnosticData& data, const Request& request,
                                 std::uint32_t bits, bool published ) noexcept
{
    const auto& input = data.input;
    if ( !published || !validDiagnosticHeader( data.header, bits )
         || input.sequence != request.sequence || input.threadId != request.threadId
         || input.exceptionPointers != request.exceptionPointers || input.flags > stackLimitsAvailable
         || !validDiagnosticAddress( input.exceptionRecord, bits )
         || !validDiagnosticAddress( input.contextRecord, bits )
         || !validDiagnosticAddress( input.pc, bits ) || !validDiagnosticAddress( input.sp, bits )
         || !validDiagnosticAddress( input.fp, bits ) || !validDiagnosticAddress( input.stackLow, bits )
         || !validDiagnosticAddress( input.stackHigh, bits ) ) {
        return false;
    }
    if ( input.flags == stackLimitsAvailable && input.stackLow >= input.stackHigh ) {
        return false;
    }
    if ( input.flags == 0 && ( input.stackLow != 0 || input.stackHigh != 0 ) ) {
        return false;
    }
    for ( const auto& module : data.targetModules ) {
        if ( !validDiagnosticModule( module, DiagnosticProcess::Target ) ) {
            return false;
        }
    }
    return true;
}

inline bool validDiagnosticResult( const DiagnosticData& data, std::uint32_t bits,
                                  bool published ) noexcept
{
    const auto& result = data.result;
    const Request request{ static_cast<std::uint32_t>( Operation::Capture ), captureSequence,
                           data.input.threadId, 0, data.input.exceptionPointers };
    if ( !published || data.inputPublication != static_cast<std::int32_t>( captureSequence )
         || !validRequest( makeHeader( bits ), request, bits )
         || !validDiagnosticInput( data, request, bits, true )
         || result.sequence != captureSequence || result.threadId != data.input.threadId
         || result.dumpSucceeded > 1 || result.closeSucceeded > 1
         || ( result.closeSucceeded != 0 && result.closeError != 0 )
         || result.dumpDomain > static_cast<std::uint32_t>( ErrorDomain::Protocol )
         || result.recordCount > maximumDiagnosticRecords || result.readCalls > maximumDiagnosticReads
         || result.queryCalls > maximumDiagnosticQueries
         || result.requestedBytes > maximumDiagnosticReadBytes || result.flags > 7 || result.decoded > 7
         || !validDiagnosticAddress( result.exceptionRecord, bits )
         || !validDiagnosticAddress( result.contextRecord, bits )
         || !validDiagnosticAddress( result.pc, bits ) || !validDiagnosticAddress( result.sp, bits )
         || !validDiagnosticAddress( result.fp, bits ) ) {
        return false;
    }
    if ( result.dumpSucceeded != 0
         && ( result.dumpDomain != static_cast<std::uint32_t>( ErrorDomain::None )
              || result.dumpError != 0 ) ) {
        return false;
    }
    if ( result.dumpSucceeded == 0
         && result.dumpDomain == static_cast<std::uint32_t>( ErrorDomain::None ) ) {
        return false;
    }
    for ( std::size_t index = 0; index < result.recordCount; ++index ) {
        const auto& record = data.records[ index ];
        if ( record.kind < 1 || record.kind > 2 || record.object < 1 || record.object > 4
             || record.order != index + 1
             || record.status > static_cast<std::uint32_t>( DiagnosticStatus::InvalidRange )
             || record.transferred > record.requested
             || ( record.kind == static_cast<std::uint32_t>( DiagnosticKind::Read )
                  && record.status == static_cast<std::uint32_t>( DiagnosticStatus::Complete )
                  && ( record.transferred != record.requested || record.error != 0 ) )
             || !validDiagnosticAddress( record.address, bits )
             || !validDiagnosticAddress( record.regionBase, bits ) ) {
            return false;
        }
        for ( const auto reserved : record.reserved ) {
            if ( reserved != 0 ) {
                return false;
            }
        }
    }
    for ( const auto& module : data.collectorModules ) {
        if ( !validDiagnosticModule( module, DiagnosticProcess::Collector ) ) {
            return false;
        }
    }
    return true;
}

inline bool diagnosticReportingAllowed( const ClientState& state,
                                        const DiagnosticData& data, std::uint32_t bits ) noexcept
{
    return state.captureRequested && state.collectorReaped
           && validDiagnosticResult( data, bits,
                                     data.resultPublication == static_cast<std::int32_t>( captureSequence ) );
}

inline bool reserveDiagnosticRead( DiagnosticResult& result, std::uint32_t bytes ) noexcept
{
    if ( bytes == 0 || result.readCalls >= maximumDiagnosticReads
         || bytes > maximumDiagnosticReadBytes
         || result.requestedBytes > maximumDiagnosticReadBytes - bytes ) {
        result.flags |= diagnosticTruncated;
        return false;
    }
    ++result.readCalls;
    result.requestedBytes += bytes;
    return true;
}

inline bool reserveDiagnosticQuery( DiagnosticResult& result ) noexcept
{
    if ( result.queryCalls >= maximumDiagnosticQueries ) {
        result.flags |= diagnosticTruncated;
        return false;
    }
    ++result.queryCalls;
    return true;
}

inline bool readableDiagnosticRegion( std::uint32_t state, std::uint32_t protection ) noexcept
{
    // Documented Windows MEM_COMMIT/PAGE_* wire values, not native API storage.
    if ( state != 0x1000U || ( protection & 0x100U ) != 0 ) {
        return false;
    }
    switch ( protection & 0xffU ) {
    case 0x02U: case 0x04U: case 0x08U: case 0x20U: case 0x40U: case 0x80U:
        return true;
    default:
        return false;
    }
}

inline bool appendDiagnosticRecord( DiagnosticData& data, DiagnosticRecord record ) noexcept
{
    if ( data.result.recordCount >= maximumDiagnosticRecords ) {
        data.result.flags |= diagnosticTruncated;
        return false;
    }
    record.order = data.result.recordCount + 1;
    data.records[ data.result.recordCount++ ] = record;
    return true;
}

inline bool visitDiagnosticException( std::array<std::uint64_t, maximumExceptionChain>& visited,
                                      std::uint32_t& count, std::uint64_t address,
                                      DiagnosticResult& result ) noexcept
{
    for ( std::uint32_t index = 0; index < count && index < maximumExceptionChain; ++index ) {
        if ( visited[ index ] == address ) {
            result.flags |= diagnosticCycle;
            return false;
        }
    }
    if ( count >= maximumExceptionChain ) {
        result.flags |= diagnosticTruncated;
        return false;
    }
    visited[ count++ ] = address;
    return true;
}

template <class Probes>
void probeDiagnosticObjects( DiagnosticData& data, Probes& probes )
{
    auto exceptionRecord = data.input.exceptionRecord;
    auto contextRecord = data.input.contextRecord;
    std::uint64_t observedException = 0;
    std::uint64_t observedContext = 0;
    if ( probes.readPointers( data.input.exceptionPointers, observedException, observedContext ) ) {
        data.result.decoded |= pointersDecoded;
        exceptionRecord = observedException;
        contextRecord = observedContext;
    }
    // Independent parent-origin roots remain usable if the wrapper read is incomplete.
    data.result.exceptionRecord = exceptionRecord;
    data.result.contextRecord = contextRecord;
    auto next = exceptionRecord;
    std::array<std::uint64_t, maximumExceptionChain> visited{};
    std::uint32_t depth = 0;
    while ( next != 0 ) {
        if ( !visitDiagnosticException( visited, depth, next, data.result ) ) {
            break;
        }
        std::uint64_t linkedRecord = 0;
        if ( !probes.readException( next, linkedRecord ) ) {
            break;
        }
        data.result.decoded |= exceptionDecoded;
        next = linkedRecord;
    }
    if ( probes.readContext( contextRecord ) ) {
        data.result.decoded |= contextDecoded;
    }
    const auto sp = ( data.result.decoded & contextDecoded ) != 0 ? data.result.sp : data.input.sp;
    probes.readStack( sp );
}

struct DiagnosticObservation {
    Outcome primary{};
    bool requested{};
};

template <class Probe>
DiagnosticObservation observeDumpOutcome( DiagnosticData* data, const Request& request,
                                          std::uint32_t bits, bool inputPublished,
                                          Outcome primary, Probe&& collectFailure )
{
    if ( data == nullptr || !validDiagnosticInput( *data, request, bits, inputPublished ) ) {
        return { primary, false };
    }
    data->result = {};
    data->result.sequence = request.sequence;
    data->result.threadId = request.threadId;
    data->result.dumpSucceeded = primary.succeeded ? 1U : 0U;
    data->result.dumpDomain = static_cast<std::uint32_t>( primary.domain );
    data->result.dumpError = primary.error;
    for ( std::size_t index = 0; index < data->collectorModules.size(); ++index ) {
        auto& module = data->collectorModules[ index ];
        module = {};
        module.process = static_cast<std::uint32_t>( DiagnosticProcess::Collector );
        module.name = static_cast<std::uint32_t>( index + 1 );
    }
    if ( !primary.succeeded ) {
        collectFailure( *data );
    }
    return { primary, true };
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

inline Outcome includeOutputCloseFailure( Outcome outcome, bool closed,
                                          std::uint32_t closeError ) noexcept
{
    if ( !closed && outcome.succeeded ) {
        return { Stage::Dump, ErrorDomain::Win32, closeError, outcome.acknowledged, false };
    }
    return outcome;
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
