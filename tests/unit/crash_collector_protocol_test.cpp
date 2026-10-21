#include <catch2/catch.hpp>

#include "../helpers/crash_collector_protocol.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
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

DiagnosticData diagnostic( std::uint32_t bits = 64 )
{
    DiagnosticData data;
    data.header = makeDiagnosticHeader( bits );
    data.input.sequence = captureSequence;
    data.input.threadId = 42;
    data.input.exceptionPointers = 0x12345678U;
    data.input.exceptionRecord = 0x2000U;
    data.input.contextRecord = 0x3000U;
    data.input.pc = 0x4000U;
    data.input.sp = 0x5000U;
    for ( std::size_t index = 0; index < diagnosticModulesPerProcess; ++index ) {
        data.targetModules[ index ].process = static_cast<std::uint32_t>( DiagnosticProcess::Target );
        data.targetModules[ index ].name = static_cast<std::uint32_t>( index + 1 );
        data.targetModules[ index ].status = static_cast<std::uint32_t>( DiagnosticStatus::Absent );
        data.collectorModules[ index ].process = static_cast<std::uint32_t>( DiagnosticProcess::Collector );
        data.collectorModules[ index ].name = static_cast<std::uint32_t>( index + 1 );
        data.collectorModules[ index ].status = static_cast<std::uint32_t>( DiagnosticStatus::Absent );
    }
    data.result.sequence = captureSequence;
    data.result.threadId = 42;
    data.result.dumpDomain = static_cast<std::uint32_t>( ErrorDomain::DumpHResult );
    data.result.dumpError = 0x8007012bU;
    data.result.closeSucceeded = 1;
    data.inputPublication = static_cast<std::int32_t>( captureSequence );
    data.resultPublication = static_cast<std::int32_t>( captureSequence );
    return data;
}

ClientState active()
{
    return { true, true, true, false, false };
}

struct FakeDiagnosticProbes {
    DiagnosticResult& result;
    bool pointersReadable{ true };
    bool exceptionReadable{ true };
    bool contextReadable{ true };
    std::uint64_t observedException{ 0x7000 };
    std::uint64_t observedContext{ 0x8000 };
    std::uint64_t observedSp{ 0x9000 };
    std::uint64_t nextException{};
    std::vector<std::pair<DiagnosticObject, std::uint64_t>> calls{};

    bool readPointers( std::uint64_t address, std::uint64_t& exceptionRecord,
                       std::uint64_t& contextRecord )
    {
        calls.emplace_back( DiagnosticObject::Pointers, address );
        // Partial reads may leave scratch outputs; only a complete read may use them.
        exceptionRecord = observedException;
        contextRecord = observedContext;
        return pointersReadable;
    }
    bool readException( std::uint64_t address, std::uint64_t& next )
    {
        calls.emplace_back( DiagnosticObject::Exception, address );
        next = nextException;
        return exceptionReadable;
    }
    bool readContext( std::uint64_t address )
    {
        calls.emplace_back( DiagnosticObject::Context, address );
        if ( contextReadable ) {
            result.sp = observedSp;
        }
        return contextReadable;
    }
    void readStack( std::uint64_t address )
    {
        calls.emplace_back( DiagnosticObject::Stack, address );
    }
};

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

TEST_CASE( "Postfailure stack plan uses known active span within the unchanged total budget" )
{
    auto data = diagnostic();
    data.input.flags = stackLimitsAvailable;
    data.input.stackLow = 0x1000;
    data.input.stackHigh = 0x9000;
    const auto full = diagnosticStackPlan( data.input, 0x8800, 1400 );
    CHECK( full.boundsKnown );
    CHECK( full.desiredBytes == 2048 );
    CHECK( full.readBytes == 2048 );
    const auto bounded = diagnosticStackPlan( data.input, 0x2000, 1400 );
    CHECK( bounded.desiredBytes == 0x7000 );
    CHECK( bounded.readBytes == maximumDiagnosticReadBytes - 1400 );
    CHECK( diagnosticStackPlan( data.input, 0x2000, maximumDiagnosticReadBytes ).readBytes == 0 );
    CHECK( diagnosticStackPlan( data.input, 0x2000, 0xffffffffU ).readBytes == 0 );
    CHECK( diagnosticStackPlan( data.input, 0x8fff, 0 ).readBytes == 1 );
    for ( const auto sp : { 0xfffULL, 0x9000ULL, 0xffffffffffffffffULL } ) {
        const auto unknown = diagnosticStackPlan( data.input, sp, 0 );
        CHECK_FALSE( unknown.boundsKnown );
        CHECK( unknown.desiredBytes == 0 );
        CHECK( unknown.readBytes == maximumStackSample );
    }
    data.input.stackLow = 0;
    CHECK_FALSE( diagnosticStackPlan( data.input, 0x8800, 0 ).boundsKnown );
    data.input.stackLow = data.input.stackHigh;
    CHECK_FALSE( diagnosticStackPlan( data.input, 0x8800, 0 ).boundsKnown );
    data.input.flags = 0;
    CHECK_FALSE( diagnosticStackPlan( data.input, 0x8800, 0 ).boundsKnown );
    CHECK( diagnosticStackPlan( data.input, 0x8800, maximumDiagnosticReadBytes - 10 ).readBytes == 10 );
    // Retained release failure: only 256 of this 1968-byte active span was previously sampled.
    data.input.flags = stackLimitsAvailable;
    data.input.stackLow = 152759173120ULL;
    data.input.stackHigh = 152760221696ULL;
    const auto retained = diagnosticStackPlan( data.input, 152760219728ULL, 1400 );
    CHECK( retained.desiredBytes == 1968 );
    CHECK( retained.readBytes == 1968 );
}

TEST_CASE( "Derived stack coverage distinguishes desired requested and actually transferred bytes" )
{
    auto data = diagnostic();
    data.input.flags = stackLimitsAvailable;
    data.input.stackLow = 0x1000;
    data.input.stackHigh = 0x9000;
    data.input.sp = 0x8e00;
    data.result.recordCount = 2;
    for ( auto index : { 0U, 1U } ) {
        auto& record = data.records[ index ];
        record.kind = static_cast<std::uint32_t>( DiagnosticKind::Read );
        record.object = static_cast<std::uint32_t>( DiagnosticObject::Stack );
        record.status = static_cast<std::uint32_t>( DiagnosticStatus::Complete );
        record.address = data.input.sp + index * 256;
        record.requested = 256;
        record.transferred = 256;
    }
    auto summary = summarizeDiagnosticStack( data );
    CHECK( summary.desiredBytes == 512 );
    CHECK( summary.requestedBytes == 512 );
    CHECK( summary.transferredBytes == 512 );
    CHECK( summary.complete );
    data.records[ 1 ].address = data.records[ 0 ].address;
    CHECK_FALSE( summarizeDiagnosticStack( data ).recordsValid );
    data.records[ 1 ].address += 256;
    data.records[ 1 ].status = static_cast<std::uint32_t>( DiagnosticStatus::Error );
    CHECK_FALSE( summarizeDiagnosticStack( data ).complete );
    data.records[ 1 ].status = static_cast<std::uint32_t>( DiagnosticStatus::Partial );
    data.records[ 1 ].transferred = 32;
    summary = summarizeDiagnosticStack( data );
    CHECK( summary.requestedBytes == 512 );
    CHECK( summary.transferredBytes == 288 );
    CHECK_FALSE( summary.complete );
    data.records[ 1 ].status = static_cast<std::uint32_t>( DiagnosticStatus::Budget );
    summary = summarizeDiagnosticStack( data );
    CHECK( summary.requestedBytes == 256 );
    CHECK( summary.transferredBytes == 256 );
    CHECK_FALSE( summary.complete );
    data.result.recordCount = 1;
    data.input.flags = 0;
    CHECK_FALSE( summarizeDiagnosticStack( data ).complete );
    data.input.flags = stackLimitsAvailable;
    data.records[ 0 ].requested = 0xffffffffffffffffULL;
    CHECK_FALSE( summarizeDiagnosticStack( data ).recordsValid );
    CHECK( diagnosticVersion == 1 );
    CHECK( sizeof( SharedPage ) == 72 );
}

TEST_CASE( "Full readable stack observation preserves an earlier partial-copy dump failure" )
{
    auto data = diagnostic();
    const Outcome failed{ Stage::Dump, ErrorDomain::DumpHResult, 0x8007012bU, false, false };
    const auto observation = observeDumpOutcome( &data, capture(), 64, true, failed,
        []( DiagnosticData& observed ) { observed.result.decoded = 7; } );
    CHECK_FALSE( observation.primary.succeeded );
    CHECK( observation.primary.error == 0x8007012bU );
    CHECK( data.result.decoded == 7 );
}

TEST_CASE( "Complete wrapper diagnostics use observed roots and decoded stack in order" )
{
    auto data = diagnostic();
    FakeDiagnosticProbes probes{ data.result };
    probeDiagnosticObjects( data, probes );
    REQUIRE( probes.calls.size() == 4 );
    CHECK( probes.calls[ 0 ].first == DiagnosticObject::Pointers );
    CHECK( probes.calls[ 0 ].second == data.input.exceptionPointers );
    CHECK( probes.calls[ 1 ].first == DiagnosticObject::Exception );
    CHECK( probes.calls[ 1 ].second == probes.observedException );
    CHECK( probes.calls[ 2 ].first == DiagnosticObject::Context );
    CHECK( probes.calls[ 2 ].second == probes.observedContext );
    CHECK( probes.calls[ 3 ].first == DiagnosticObject::Stack );
    CHECK( probes.calls[ 3 ].second == probes.observedSp );
    CHECK( data.result.exceptionRecord == probes.observedException );
    CHECK( data.result.contextRecord == probes.observedContext );
    CHECK( data.result.decoded == ( pointersDecoded | exceptionDecoded | contextDecoded ) );
}

TEST_CASE( "Incomplete wrapper diagnostics still probe independently supplied child roots" )
{
    auto data = diagnostic();
    FakeDiagnosticProbes probes{ data.result };
    probes.pointersReadable = false;
    probeDiagnosticObjects( data, probes );
    CHECK( data.result.exceptionRecord == data.input.exceptionRecord );
    CHECK( data.result.contextRecord == data.input.contextRecord );
    CHECK( ( data.result.decoded & pointersDecoded ) == 0 );
    REQUIRE( probes.calls.size() == 4 );
    CHECK( probes.calls[ 0 ].first == DiagnosticObject::Pointers );
    CHECK( probes.calls[ 0 ].second == data.input.exceptionPointers );
    CHECK( probes.calls[ 1 ].first == DiagnosticObject::Exception );
    CHECK( probes.calls[ 1 ].second == data.input.exceptionRecord );
    CHECK( probes.calls[ 2 ].first == DiagnosticObject::Context );
    CHECK( probes.calls[ 2 ].second == data.input.contextRecord );
    CHECK( probes.calls[ 3 ].first == DiagnosticObject::Stack );
    CHECK( probes.calls[ 3 ].second == probes.observedSp );
    CHECK( data.result.decoded == ( exceptionDecoded | contextDecoded ) );
}

TEST_CASE( "Failed exception probes do not gate context and unreadable contexts use originating SP" )
{
    for ( const auto wrapperReadable : { false, true } ) {
        for ( const auto contextReadable : { false, true } ) {
            auto data = diagnostic();
            FakeDiagnosticProbes probes{ data.result };
            probes.pointersReadable = wrapperReadable;
            probes.exceptionReadable = false;
            probes.contextReadable = contextReadable;
            probeDiagnosticObjects( data, probes );
            REQUIRE( probes.calls.size() == 4 );
            CHECK( probes.calls[ 2 ].first == DiagnosticObject::Context );
            CHECK( probes.calls[ 2 ].second
                   == ( wrapperReadable ? probes.observedContext : data.input.contextRecord ) );
            CHECK( probes.calls[ 3 ].first == DiagnosticObject::Stack );
            CHECK( probes.calls[ 3 ].second
                   == ( contextReadable ? probes.observedSp : data.input.sp ) );
            CHECK( ( data.result.decoded & pointersDecoded )
                   == ( wrapperReadable ? pointersDecoded : 0U ) );
            CHECK( ( data.result.decoded & exceptionDecoded ) == 0 );
            CHECK( ( data.result.decoded & contextDecoded )
                   == ( contextReadable ? contextDecoded : 0U ) );
        }
    }
}

TEST_CASE( "Diagnostic sequencing stops an exception cycle before another native read" )
{
    auto data = diagnostic();
    FakeDiagnosticProbes probes{ data.result };
    probes.nextException = probes.observedException;
    probeDiagnosticObjects( data, probes );
    REQUIRE( probes.calls.size() == 4 );
    CHECK( probes.calls[ 1 ].first == DiagnosticObject::Exception );
    CHECK( probes.calls[ 2 ].first == DiagnosticObject::Context );
    CHECK( probes.calls[ 3 ].first == DiagnosticObject::Stack );
    CHECK( ( data.result.flags & diagnosticCycle ) != 0 );
}

TEST_CASE( "Output close finalization preserves the first failure across the result matrix" )
{
    const Outcome dumped{ Stage::None, ErrorDomain::None, 0, false, true };
    const Outcome failed{ Stage::Dump, ErrorDomain::DumpHResult, 0x8007012bU, false, false };
    CHECK( includeOutputCloseFailure( dumped, true, 0 ).succeeded );
    const auto closeFailed = includeOutputCloseFailure( dumped, false, 6 );
    CHECK_FALSE( closeFailed.succeeded );
    CHECK( closeFailed.domain == ErrorDomain::Win32 );
    CHECK( closeFailed.error == 6 );
    const auto dumpFailed = includeOutputCloseFailure( failed, true, 0 );
    CHECK_FALSE( dumpFailed.succeeded );
    CHECK( dumpFailed.domain == ErrorDomain::DumpHResult );
    CHECK( dumpFailed.error == 0x8007012bU );
    const auto bothFailed = includeOutputCloseFailure( failed, false, 6 );
    CHECK_FALSE( bothFailed.succeeded );
    CHECK( bothFailed.domain == ErrorDomain::DumpHResult );
    CHECK( bothFailed.error == 0x8007012bU );
}

TEST_CASE( "Diagnostic status cannot claim complete reads or successful close with errors" )
{
    auto data = diagnostic();
    REQUIRE( validDiagnosticResult( data, 64, true ) );
    data.result.closeError = 6;
    CHECK_FALSE( validDiagnosticResult( data, 64, true ) );
    data = diagnostic();
    data.result.readCalls = 1;
    data.result.requestedBytes = 16;
    data.result.recordCount = 1;
    data.records[ 0 ] = { static_cast<std::uint32_t>( DiagnosticKind::Read ),
                          static_cast<std::uint32_t>( DiagnosticObject::Pointers ), 1,
                          static_cast<std::uint32_t>( DiagnosticStatus::Complete ),
                          0x12345678U, 16, 15, 0, 0, 0, 0, 0, 0, {} };
    CHECK_FALSE( validDiagnosticResult( data, 64, true ) );
}

TEST_CASE( "Optional diagnostics require known format bounds and observed collector exit" )
{
    for ( const auto bits : { 32U, 64U } ) {
        auto data = diagnostic( bits );
        auto state = active();
        state.captureRequested = true;
        CHECK_FALSE( diagnosticReportingAllowed( state, data, bits ) );
        state.collectorReaped = true;
        CHECK( diagnosticReportingAllowed( state, data, bits ) );
        data.resultPublication = 0;
        CHECK_FALSE( diagnosticReportingAllowed( state, data, bits ) );
        data = diagnostic( bits );
        ++data.header.version;
        CHECK_FALSE( diagnosticReportingAllowed( state, data, bits ) );
        data = diagnostic( bits );
        ++data.header.structureBytes;
        CHECK_FALSE( diagnosticReportingAllowed( state, data, bits ) );
        data = diagnostic( bits );
        data.header.pointerBits = bits == 32 ? 64 : 32;
        CHECK_FALSE( diagnosticReportingAllowed( state, data, bits ) );
        data = diagnostic( bits );
        data.result.sequence = 2;
        CHECK_FALSE( diagnosticReportingAllowed( state, data, bits ) );
        data = diagnostic( bits );
        data.result.recordCount = static_cast<std::uint32_t>( maximumDiagnosticRecords + 1 );
        CHECK_FALSE( diagnosticReportingAllowed( state, data, bits ) );
        data = diagnostic( bits );
        data.collectorModules[ 0 ].pathLength = static_cast<std::uint32_t>( maximumDiagnosticPath + 1 );
        CHECK_FALSE( diagnosticReportingAllowed( state, data, bits ) );
        data = diagnostic( bits );
        data.inputPublication = 0;
        CHECK_FALSE( diagnosticReportingAllowed( state, data, bits ) );
    }
}

TEST_CASE( "Diagnostic wire rejects reserved fields and narrowing but accepts bounded partial evidence" )
{
    for ( const auto bits : { 32U, 64U } ) {
        auto data = diagnostic( bits );
        data.header.reserved = 1;
        CHECK_FALSE( validDiagnosticResult( data, bits, true ) );
        data = diagnostic( bits );
        data.targetModules[ 0 ].reserved[ 2 ] = 1;
        CHECK_FALSE( validDiagnosticResult( data, bits, true ) );
        data = diagnostic( bits );
        data.collectorModules[ 0 ].process
            = static_cast<std::uint32_t>( DiagnosticProcess::Target );
        CHECK_FALSE( validDiagnosticResult( data, bits, true ) );
        data = diagnostic( bits );
        auto& module = data.targetModules[ 0 ];
        module.status = static_cast<std::uint32_t>( DiagnosticStatus::Truncated );
        module.pathLength = static_cast<std::uint32_t>( maximumDiagnosticPath );
        module.path.fill( static_cast<std::uint16_t>( 'x' ) );
        CHECK( validDiagnosticResult( data, bits, true ) );
        module.path[ maximumDiagnosticPath - 1 ] = 0;
        CHECK_FALSE( validDiagnosticResult( data, bits, true ) );
        data = diagnostic( bits );
        data.input.sp = 0x100000000ULL;
        CHECK( validDiagnosticResult( data, bits, true ) == ( bits == 64 ) );
        data = diagnostic( bits );
        data.result.pc = 0x100000000ULL;
        CHECK( validDiagnosticResult( data, bits, true ) == ( bits == 64 ) );
        data = diagnostic( bits );
        data.result.readCalls = 1;
        data.result.requestedBytes = 16;
        data.result.recordCount = 1;
        auto& record = data.records[ 0 ];
        record.kind = static_cast<std::uint32_t>( DiagnosticKind::Read );
        record.object = static_cast<std::uint32_t>( DiagnosticObject::Pointers );
        record.order = 1;
        record.status = static_cast<std::uint32_t>( DiagnosticStatus::Partial );
        record.address = 0x1000;
        record.requested = 16;
        record.transferred = 15;
        record.error = 299;
        CHECK( validDiagnosticResult( data, bits, true ) );
        record.reserved[ 1 ] = 1;
        CHECK_FALSE( validDiagnosticResult( data, bits, true ) );
        record.reserved[ 1 ] = 0;
        record.transferred = 17;
        CHECK_FALSE( validDiagnosticResult( data, bits, true ) );
        record.transferred = 16;
        record.status = static_cast<std::uint32_t>( DiagnosticStatus::Complete );
        CHECK_FALSE( validDiagnosticResult( data, bits, true ) );
        record.error = 0;
        CHECK( validDiagnosticResult( data, bits, true ) );
    }
}

TEST_CASE( "Diagnostic read query and record budgets include failed attempts and stop exactly" )
{
    auto data = diagnostic();
    for ( std::uint32_t index = 0; index < maximumDiagnosticReads; ++index ) {
        CHECK( reserveDiagnosticRead( data.result, 256 ) );
    }
    CHECK( data.result.requestedBytes == maximumDiagnosticReadBytes );
    CHECK_FALSE( reserveDiagnosticRead( data.result, 1 ) );
    CHECK( data.result.readCalls == maximumDiagnosticReads );
    CHECK( data.result.requestedBytes == maximumDiagnosticReadBytes );
    for ( std::uint32_t index = 0; index < maximumDiagnosticQueries; ++index ) {
        CHECK( reserveDiagnosticQuery( data.result ) );
    }
    CHECK_FALSE( reserveDiagnosticQuery( data.result ) );
    CHECK( data.result.queryCalls == maximumDiagnosticQueries );
    for ( std::size_t index = 0; index < maximumDiagnosticRecords; ++index ) {
        CHECK( appendDiagnosticRecord( data, {} ) );
    }
    CHECK_FALSE( appendDiagnosticRecord( data, {} ) );
    CHECK( data.result.recordCount == maximumDiagnosticRecords );
    CHECK( ( data.result.flags & diagnosticTruncated ) != 0 );
}

TEST_CASE( "Diagnostic ranges and protections reject wraparound and unsafe memory" )
{
    CHECK( validDiagnosticRange( 0x1000, 4096, 32 ) );
    CHECK_FALSE( validDiagnosticRange( 0, 1, 64 ) );
    CHECK_FALSE( validDiagnosticRange( 0x1000, 0, 64 ) );
    CHECK_FALSE( validDiagnosticRange( 0xffffffffULL, 2, 32 ) );
    CHECK_FALSE( validDiagnosticRange( 0x100000000ULL, 1, 32 ) );
    CHECK_FALSE( validDiagnosticRange( ( std::numeric_limits<std::uint64_t>::max )(), 1, 64 ) );
    for ( const auto protection : { 0x02U, 0x04U, 0x08U, 0x20U, 0x40U, 0x80U } ) {
        CHECK( readableDiagnosticRegion( 0x1000, protection ) );
        CHECK_FALSE( readableDiagnosticRegion( 0x1000, protection | 0x100U ) );
        CHECK_FALSE( readableDiagnosticRegion( 0x2000, protection ) );
    }
    CHECK_FALSE( readableDiagnosticRegion( 0x1000, 0x01 ) );
    CHECK_FALSE( readableDiagnosticRegion( 0x1000, 0x10 ) );
}

TEST_CASE( "Exception diagnostics stop cycles and cap unique chains without another read" )
{
    DiagnosticResult result;
    std::array<std::uint64_t, maximumExceptionChain> visited{};
    std::uint32_t count = 0;
    CHECK( visitDiagnosticException( visited, count, 0x1000, result ) );
    CHECK( visitDiagnosticException( visited, count, 0x2000, result ) );
    CHECK_FALSE( visitDiagnosticException( visited, count, 0x1000, result ) );
    CHECK( count == 2 );
    CHECK( ( result.flags & diagnosticCycle ) != 0 );
    CHECK( visitDiagnosticException( visited, count, 0x3000, result ) );
    CHECK_FALSE( visitDiagnosticException( visited, count, 0x4000, result ) );
    CHECK( count == maximumExceptionChain );
    CHECK( ( result.flags & diagnosticTruncated ) != 0 );
}

TEST_CASE( "Unavailable diagnostic input skips production probes and preserves capture and reap" )
{
    enum class InvalidInput { Missing, Unpublished, Unsupported, Thread, Pointers, Reserved, Path };
    for ( const auto bits : { 32U, 64U } ) {
        for ( const auto succeeded : { false, true } ) {
            const Outcome primary{ succeeded ? Stage::None : Stage::Dump,
                                   succeeded ? ErrorDomain::None : ErrorDomain::DumpHResult,
                                   succeeded ? 0U : 0x8007012bU, false, succeeded };
            for ( const auto invalid : { InvalidInput::Missing, InvalidInput::Unpublished,
                                         InvalidInput::Unsupported, InvalidInput::Thread,
                                         InvalidInput::Pointers, InvalidInput::Reserved,
                                         InvalidInput::Path } ) {
                auto data = diagnostic( bits );
                data.resultPublication = 0;
                data.result.dumpError = 0xfeedU;
                switch ( invalid ) {
                case InvalidInput::Missing: break;
                case InvalidInput::Unpublished: data.inputPublication = 0; break;
                case InvalidInput::Unsupported: ++data.header.version; break;
                case InvalidInput::Thread: ++data.input.threadId; break;
                case InvalidInput::Pointers: ++data.input.exceptionPointers; break;
                case InvalidInput::Reserved: data.header.reserved = 1; break;
                case InvalidInput::Path:
                    data.targetModules[ 0 ].pathLength
                        = static_cast<std::uint32_t>( maximumDiagnosticPath + 1 );
                    break;
                }
                std::uint32_t probes = 0;
                const auto observation = observeDumpOutcome(
                    invalid == InvalidInput::Missing ? nullptr : &data, capture(), bits,
                    data.inputPublication == static_cast<std::int32_t>( captureSequence ), primary,
                    [&probes]( DiagnosticData& ) { ++probes; } );
                CHECK_FALSE( observation.requested );
                CHECK( probes == 0 );
                CHECK( data.result.dumpError == 0xfeedU );
                CHECK( observation.primary.stage == primary.stage );
                CHECK( observation.primary.domain == primary.domain );
                CHECK( observation.primary.error == primary.error );
                CHECK( observation.primary.succeeded == primary.succeeded );
                CHECK( observation.primary.acknowledged == primary.acknowledged );
                auto state = active();
                FakeOps ops;
                ops.snapshot = successful( Phase::Complete, bits );
                ops.snapshot.acknowledgement.succeeded = observation.primary.succeeded ? 1U : 0U;
                ops.snapshot.acknowledgement.errorDomain
                    = static_cast<std::uint32_t>( observation.primary.domain );
                ops.snapshot.acknowledgement.error = observation.primary.error;
                const auto result = captureOnce( state, ops, capture(), bits, 100, 10 );
                CHECK( result.succeeded == primary.succeeded );
                CHECK( result.domain == primary.domain );
                CHECK( result.error == primary.error );
                CHECK( state.collectorReaped );
                CHECK( ops.calls.back() == "reaped" );
                CHECK_FALSE( diagnosticReportingAllowed( state, data, bits ) );
            }
        }
    }
}

TEST_CASE( "Valid diagnostic input initializes production evidence and probes failed dumps only" )
{
    for ( const auto succeeded : { false, true } ) {
        for ( const auto flags : { 0U, diagnosticUnavailable, diagnosticTruncated } ) {
            auto data = diagnostic();
            data.resultPublication = 0;
            const Outcome primary{ succeeded ? Stage::None : Stage::Dump,
                                   succeeded ? ErrorDomain::None : ErrorDomain::DumpHResult,
                                   succeeded ? 0U : 0x8007012bU, false, succeeded };
            std::uint32_t probes = 0;
            const auto observation = observeDumpOutcome(
                &data, capture(), 64, true, primary,
                [&probes, flags]( DiagnosticData& evidence ) {
                    ++probes;
                    evidence.result.flags = flags;
                    if ( flags == diagnosticTruncated ) {
                        for ( std::uint32_t index = 0; index < maximumDiagnosticReads; ++index ) {
                            CHECK( reserveDiagnosticRead( evidence.result, 256 ) );
                        }
                        CHECK_FALSE( reserveDiagnosticRead( evidence.result, 1 ) );
                    }
                } );
            CHECK( observation.requested );
            CHECK( probes == ( succeeded ? 0U : 1U ) );
            CHECK( data.result.sequence == captureSequence );
            CHECK( data.result.threadId == data.input.threadId );
            CHECK( data.result.dumpSucceeded == ( succeeded ? 1U : 0U ) );
            CHECK( data.result.dumpDomain == static_cast<std::uint32_t>( primary.domain ) );
            CHECK( data.result.dumpError == primary.error );
            CHECK( data.result.flags == ( succeeded ? 0U : flags ) );
            CHECK( data.resultPublication == 0 );
            CHECK( observation.primary.stage == primary.stage );
            CHECK( observation.primary.domain == primary.domain );
            CHECK( observation.primary.error == primary.error );
            CHECK( observation.primary.succeeded == primary.succeeded );
            CHECK( observation.primary.acknowledged == primary.acknowledged );
            const auto closed = includeOutputCloseFailure( observation.primary, false, 6 );
            CHECK_FALSE( closed.succeeded );
            CHECK( closed.domain == ( succeeded ? ErrorDomain::Win32 : ErrorDomain::DumpHResult ) );
            CHECK( closed.error == ( succeeded ? 6U : 0x8007012bU ) );
        }
    }
}

TEST_CASE( "Capture timeout leaves unpublished frozen dump evidence unavailable after forced reap" )
{
    auto data = diagnostic();
    data.resultPublication = 0;
    const Outcome primary{ Stage::Dump, ErrorDomain::DumpHResult, 0x8007012bU, false, false };
    const auto observation = observeDumpOutcome(
        &data, capture(), 64, true, primary, []( DiagnosticData& ) {} );
    REQUIRE( observation.requested );
    auto state = active();
    FakeOps ops;
    ops.channelResult = ChannelWait::TimedOut;
    ops.waits = { ProcessWait::TimedOut, ProcessWait::Signaled };
    ops.waitingCheckpoint = [&state, &data] {
        CHECK_FALSE( diagnosticReportingAllowed( state, data, 64 ) );
    };
    const auto result = captureOnce( state, ops, capture(), 64, 100, 10 );
    CHECK_FALSE( result.succeeded );
    CHECK_FALSE( result.acknowledged );
    CHECK( result.stage == Stage::Acknowledgement );
    CHECK( result.domain == ErrorDomain::Win32 );
    CHECK( result.error == waitTimeoutError );
    CHECK( data.result.dumpError == primary.error );
    CHECK( data.resultPublication == 0 );
    CHECK( state.collectorReaped );
    CHECK_FALSE( diagnosticReportingAllowed( state, data, 64 ) );
    CHECK( ops.calls == std::vector<std::string>{ "publish", "ack.wait.100", "process.wait.10",
                                                 "terminate", "process.wait.4294967295", "reaped" } );
}

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
