#include <catch2/catch.hpp>

#include <atomic>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSemaphore>
#include <QThread>

#include "reviewcontentadapter.h"
#include "test_utils.h"

using namespace klogg::control;

namespace {
QJsonObject object( const DecodedResponse& response )
{
    REQUIRE( std::holds_alternative<QJsonValue>( response ) );
    REQUIRE( std::get<QJsonValue>( response ).isObject() );
    return std::get<QJsonValue>( response ).toObject();
}
QString code( const DecodedResponse& response )
{
    REQUIRE( std::holds_alternative<RpcError>( response ) );
    return std::get<RpcError>( response ).data.toObject().value( "code" ).toString();
}
struct Observations {
    std::atomic<int> payloadReads{ 0 };
    std::atomic<int> metadataReads{ 0 };
    std::atomic<bool> readOnGui{ false };
    QSemaphore release;
};
class Snapshot final : public klogg::LogSnapshot {
  public:
    std::shared_ptr<Observations> observations = std::make_shared<Observations>();
    std::optional<uint64_t> extent = 8;
    QString text = QStringLiteral( "retained" );
    bool block = false;
    bool failRead = false;
    bool failMetadata = false;
    bool copyable = true;
    LinesCount lineCount() const override { return 2_lcount; }
    klogg::SnapshotRow row( LineNumber index ) const override
    {
        ++observations->metadataReads;
        if ( failMetadata && index == 1_lnum ) throw klogg::SnapshotError( "sensitive-metadata-detail" );
        return { { klogg::LogRowId::Origin::Capture, 17, 3, index.get() },
                 LineNumber{ index.get() + 11 }, copyable, true };
    }
    std::optional<uint64_t> sourceBytesForLine( LineNumber ) const override { return extent; }
    QString lineString( LineNumber index ) const override
    {
        observations->readOnGui = QThread::currentThread() == QCoreApplication::instance()->thread();
        ++observations->payloadReads;
        if ( block ) observations->release.acquire();
        if ( failRead && index.get() == 1 ) throw klogg::SnapshotError( "sensitive-line-content" );
        return text;
    }
};
struct ReleaseOnExit {
    std::shared_ptr<Observations> observations;
    ~ReleaseOnExit() { observations->release.release( 2 * ReviewContentAdapter::MaxConcurrentReads ); }
};
struct Fixture {
    qint64 now = 1000;
    ReviewContentAdapter adapter{ [this] { return now; } };
    DecodedResponse call( const QString& method, const QJsonObject& params = {} )
    {
        return adapter.dispatch( { "request", method, params } );
    }
    QString capture( klogg::LogSnapshot::Ptr source )
    {
        return object( adapter.capture( { "capture", "source.capture", {} }, std::move( source ) ) )
            .value( "snapshot_id" ).toString();
    }
    QString read( const QString& snapshot, int count = 1 )
    {
        return object( call( "source.read", { { "snapshot_id", snapshot }, { "start", "0" },
                                              { "count", count } } ) ).value( "operation_id" ).toString();
    }
    QJsonObject finished( const QString& operation )
    {
        QJsonObject result;
        REQUIRE( waitUiState( [&] {
            result = object( call( "operation.get", { { "operation_id", operation } } ) );
            return result.value( "status" ) != QLatin1String( "pending" );
        } ) );
        return result;
    }
};
}

TEST_CASE( "Lease row resolution returns ordered metadata without payload or extra content pins",
           "[review-content][review-evidence-rows]" )
{
    Fixture fixture;
    auto source = std::make_shared<Snapshot>();
    source->extent.reset(); // Metadata needs no physical-read budget or decoder.
    source->failRead = true;
    const auto observed = source->observations;
    std::weak_ptr<const klogg::LogSnapshot> weak = source;
    const auto id = fixture.capture( source );
    const auto result = fixture.adapter.resolveRows( id, { 1_lnum, 0_lnum } );
    REQUIRE( std::holds_alternative<ReviewContentAdapter::RowMetadata>( result ) );
    const auto& rows = std::get<ReviewContentAdapter::RowMetadata>( result );
    REQUIRE( rows.size() == 2 );
    CHECK( rows.at( 0 ).identity.row == 1 );
    CHECK( rows.at( 0 ).sourceLineNumber == 12_lnum );
    CHECK( rows.at( 1 ).identity.row == 0 );
    CHECK( observed->payloadReads.load() == 0 );
    object( fixture.call( "source.release", { { "snapshot_id", id } } ) );
    source.reset();
    CHECK( weak.expired() );
    const auto expired = fixture.adapter.resolveRows( id, { 0_lnum } );
    REQUIRE( std::holds_alternative<ReviewContentAdapter::RowResolutionError>( expired ) );
    CHECK( std::get<ReviewContentAdapter::RowResolutionError>( expired )
           == ReviewContentAdapter::RowResolutionError::SnapshotNotFound );
}

TEST_CASE( "Evidence row admission validates the whole bounded range before metadata access",
           "[review-content][review-evidence-rows]" )
{
    Fixture fixture;
    auto source = std::make_shared<Snapshot>();
    const auto id = fixture.capture( source );
    using Error = ReviewContentAdapter::RowResolutionError;
    std::vector<LineNumber> rows{ 0_lnum, 2_lnum };
    auto expected = Error::InvalidArgument;
    SECTION( "out of range" ) {}
    SECTION( "empty request" ) { rows.clear(); }
    SECTION( "too many rows" ) { rows.assign( ReviewContentAdapter::MaxRows + 1, 0_lnum ); expected = Error::ResourceLimit; }
    SECTION( "expired lease" ) { rows = { 0_lnum }; fixture.now += ReviewContentAdapter::LifetimeMs; expected = Error::SnapshotNotFound; }
    const auto result = fixture.adapter.resolveRows( id, rows );
    REQUIRE( std::holds_alternative<Error>( result ) );
    CHECK( std::get<Error>( result ) == expected );
    CHECK( source->observations->metadataReads.load() == 0 );
    CHECK( source->observations->payloadReads.load() == 0 );
}

TEST_CASE( "Evidence metadata failures and synthetic rows do not return a valid prefix",
           "[review-content][review-evidence-rows]" )
{
    Fixture fixture;
    auto source = std::make_shared<Snapshot>();
    const auto id = fixture.capture( source );
    using Error = ReviewContentAdapter::RowResolutionError;
    auto expected = Error::SourceUnavailable;
    SECTION( "provider exception" ) { source->failMetadata = true; }
    SECTION( "synthetic row" ) { source->copyable = false; expected = Error::InvalidArgument; }
    const auto result = fixture.adapter.resolveRows( id, { 0_lnum, 1_lnum } );
    REQUIRE( std::holds_alternative<Error>( result ) );
    CHECK( std::get<Error>( result ) == expected );
    CHECK( source->observations->payloadReads.load() == 0 );
}

TEST_CASE( "Explicit content reads use immutable leases off the GUI thread", "[review-content]" )
{
    Fixture fixture;
    auto source = std::make_shared<Snapshot>();
    const auto observed = source->observations;
    const auto id = fixture.capture( source );
    REQUIRE_FALSE( id.isEmpty() );
    CHECK( observed->payloadReads.load() == 0 );
    const auto operation = fixture.read( id, 2 );
    object( fixture.call( "source.release", { { "snapshot_id", id } } ) );
    source.reset();
    const auto result = fixture.finished( operation );
    CHECK( result.value( "status" ) == QLatin1String( "completed" ) );
    const auto data = result.value( "result" ).toObject();
    CHECK( data.value( "source_bytes" ) == QLatin1String( "16" ) );
    const auto rows = data.value( "rows" ).toArray();
    REQUIRE( rows.size() == 2 );
    CHECK( rows.first().toObject().value( "text" ) == QLatin1String( "retained" ) );
    CHECK( rows.first().toObject().value( "source_line" ) == QLatin1String( "11" ) );
    CHECK( rows.first().toObject().value( "identity" ).toObject().value( "incarnation" ) == QLatin1String( "17" ) );
    CHECK( observed->payloadReads.load() == 2 );
    CHECK_FALSE( observed->readOnGui );
    CHECK( code( fixture.call( "source.read", { { "snapshot_id", id }, { "start", "0" }, { "count", 1 } } ) )
           == QLatin1String( "SNAPSHOT_NOT_FOUND" ) );
}

TEST_CASE( "Content budgets reject unknown or oversized physical extents before payload", "[review-content]" )
{
    Fixture fixture;
    auto source = std::make_shared<Snapshot>();
    QString expected = QStringLiteral( "RESOURCE_LIMIT" );
    SECTION( "aggregate raw bytes" ) { source->extent = ReviewContentAdapter::MaxSourceBytes / 2 + 1; }
    SECTION( "single huge row" ) { source->extent = uint64_t{ 1 } << 40; }
    SECTION( "unknown provider" ) { source->extent.reset(); expected = QStringLiteral( "READ_UNSUPPORTED" ); }
    const auto id = fixture.capture( source );
    const auto result = fixture.finished( fixture.read( id, 2 ) );
    CHECK( result.value( "status" ) == QLatin1String( "failed" ) );
    CHECK( result.value( "error" ).toObject().value( "code" ) == expected );
    CHECK( source->observations->payloadReads == 0 );
    CHECK_FALSE( result.contains( "result" ) );
}

TEST_CASE( "Content reads bound decoded text and JSON escaping without partial results", "[review-content]" )
{
    Fixture fixture;
    auto source = std::make_shared<Snapshot>();
    SECTION( "decoded units" ) {
        source->text = QString( static_cast<int>( ReviewContentAdapter::MaxTextUnits + 1 ), QLatin1Char( 'x' ) );
    }
    SECTION( "serialized escaping" ) {
        source->text = QString( static_cast<int>( ReviewContentAdapter::MaxTextUnits ), QChar( 0 ) );
    }
    const auto result = fixture.finished( fixture.read( fixture.capture( source ) ) );
    CHECK( result.value( "status" ) == QLatin1String( "failed" ) );
    CHECK( result.value( "error" ).toObject().value( "code" ) == QLatin1String( "RESOURCE_LIMIT" ) );
    CHECK_FALSE( result.contains( "result" ) );
}

TEST_CASE( "Content input validation and expiring leases do not read payload", "[review-content]" )
{
    Fixture fixture;
    auto source = std::make_shared<Snapshot>();
    const auto id = fixture.capture( source );
    for ( const QJsonValue& start : { QJsonValue( 0 ), QJsonValue( "01" ), QJsonValue( "-1" ),
                                    QJsonValue( "18446744073709551616" ) } ) {
        CHECK( code( fixture.call( "source.read", { { "snapshot_id", id }, { "start", start }, { "count", 1 } } ) )
               == QLatin1String( "INVALID_PARAMS" ) );
    }
    for ( const auto count : { 0, 257, -1 } ) {
        CHECK( code( fixture.call( "source.read", { { "snapshot_id", id }, { "start", "0" }, { "count", count } } ) )
               == QLatin1String( "INVALID_PARAMS" ) );
    }
    CHECK( code( fixture.call( "source.read", { { "snapshot_id", id }, { "start", "2" }, { "count", 1 } } ) )
           == QLatin1String( "INVALID_PARAMS" ) );
    fixture.now += ReviewContentAdapter::LifetimeMs;
    CHECK( code( fixture.call( "source.read", { { "snapshot_id", id }, { "start", "0" }, { "count", 1 } } ) )
           == QLatin1String( "SNAPSHOT_NOT_FOUND" ) );
    CHECK( source->observations->payloadReads == 0 );
    CHECK( code( fixture.adapter.capture( { "capture", "source.capture", {} }, {} ) )
           == QLatin1String( "TARGET_NOT_READY" ) );
}

TEST_CASE( "Idle content leases expire without another incoming request", "[review-content]" )
{
    Fixture fixture;
    auto source = std::make_shared<Snapshot>();
    const std::weak_ptr<Snapshot> retained = source;
    REQUIRE_FALSE( fixture.capture( source ).isEmpty() );
    source.reset();
    REQUIRE_FALSE( retained.expired() );
    fixture.now += ReviewContentAdapter::LifetimeMs;
    REQUIRE( waitUiState( [&] { return retained.expired(); } ) );
}

TEST_CASE( "Content provider failures do not expose exception text or partial rows", "[review-content]" )
{
    Fixture fixture;
    auto source = std::make_shared<Snapshot>();
    source->failRead = true;
    const auto result = fixture.finished( fixture.read( fixture.capture( source ), 2 ) );
    CHECK( result.value( "status" ) == QLatin1String( "failed" ) );
    CHECK( result.value( "error" ).toObject().value( "code" ) == QLatin1String( "SOURCE_READ_FAILED" ) );
    CHECK_FALSE( result.contains( "result" ) );
    CHECK_FALSE( QJsonDocument( result ).toJson().contains( "sensitive-line-content" ) );
}

TEST_CASE( "Content admission bounds leases and queued workers", "[review-content]" )
{
    Fixture fixture;
    auto source = std::make_shared<Snapshot>();
    SECTION( "finite snapshot leases release on reset" ) {
        for ( size_t index = 0; index < ReviewContentAdapter::MaxSnapshots; ++index )
            REQUIRE_FALSE( fixture.capture( source ).isEmpty() );
        CHECK( code( fixture.adapter.capture( { "capture", "source.capture", {} }, source ) )
               == QLatin1String( "RESOURCE_LIMIT" ) );
        fixture.adapter.reset();
        REQUIRE_FALSE( fixture.capture( source ).isEmpty() );
    }
    SECTION( "queued or running workers retain finite admission until retirement" ) {
        source->block = true;
        ReleaseOnExit release{ source->observations };
        const auto id = fixture.capture( source );
        for ( int index = 0; index < ReviewContentAdapter::MaxConcurrentReads; ++index )
            REQUIRE_FALSE( fixture.read( id ).isEmpty() );
        CHECK( code( fixture.call( "source.read", { { "snapshot_id", id }, { "start", "0" }, { "count", 1 } } ) )
               == QLatin1String( "RESOURCE_LIMIT" ) );
        const std::weak_ptr<Snapshot> retained = source;
        fixture.adapter.reset();
        source.reset();
        release.observations->release.release( ReviewContentAdapter::MaxConcurrentReads );
        REQUIRE( waitUiState( [&] { return retained.expired(); } ) );
    }
}

TEST_CASE( "Pending content reads do not block GUI queries and reset retires their IDs", "[review-content]" )
{
    Fixture fixture;
    auto source = std::make_shared<Snapshot>();
    source->block = true;
    ReleaseOnExit release{ source->observations };
    const auto operation = fixture.read( fixture.capture( source ) );
    REQUIRE( waitUiState( [&] { return source->observations->payloadReads.load() == 1; } ) );
    CHECK( object( fixture.call( "operation.get", { { "operation_id", operation } } ) ).value( "status" )
           == QLatin1String( "pending" ) );
    SECTION( "reset retires IDs without waiting for payload I/O" ) {
        fixture.adapter.reset();
        CHECK( code( fixture.call( "operation.get", { { "operation_id", operation } } ) )
               == QLatin1String( "OPERATION_NOT_FOUND" ) );
    }
    SECTION( "cancellation discards an unpublished result" ) {
        CHECK( object( fixture.call( "operation.cancel", { { "operation_id", operation } } ) )
                   .value( "cancel_requested" ).toBool() );
        source->observations->release.release();
        const auto result = fixture.finished( operation );
        CHECK( result.value( "status" ) == QLatin1String( "cancelled" ) );
        CHECK_FALSE( result.contains( "result" ) );
    }
    CHECK_FALSE( source->observations->readOnGui );
}
