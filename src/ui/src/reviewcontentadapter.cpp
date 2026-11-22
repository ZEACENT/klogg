#include "reviewcontentadapter.h"
#include <new>
#include "asyncworkbudget.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <utility>
#include <QFuture>
#include <QFutureInterface>
#include <QPointer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

namespace {
using namespace klogg::control;
using Limits = ReviewContentAdapter;

RpcError failure( const RpcRequest& request, const char* code )
{
    const auto symbol = QLatin1String( code );
    const auto number = symbol == QLatin1String( "INVALID_PARAMS" ) ? -32602
                      : symbol == QLatin1String( "METHOD_NOT_FOUND" ) ? -32601 : -32000;
    return { request.id, number, QStringLiteral( "Content request rejected" ),
             QJsonObject{ { "code", code } }, true, false };
}
bool onlyKeys( const QJsonObject& params, std::initializer_list<const char*> allowed )
{
    for ( auto it = params.begin(); it != params.end(); ++it ) {
        if ( !std::any_of( allowed.begin(), allowed.end(), [&]( const char* key ) {
                 return it.key() == QLatin1String( key );
             } ) ) return false;
    }
    return true;
}
std::optional<uint64_t> ordinal( const QJsonValue& value )
{
    bool valid = false;
    const auto number = value.toString().toULongLong( &valid );
    if ( !value.isString() || !valid || QString::number( number ) != value.toString() ) return {};
    return number;
}
QString originName( klogg::LogRowId::Origin origin )
{
    switch ( origin ) {
    case klogg::LogRowId::Origin::IndexedFile: return QStringLiteral( "indexed_file" );
    case klogg::LogRowId::Origin::Capture: return QStringLiteral( "capture" );
    case klogg::LogRowId::Origin::Concatenated: return QStringLiteral( "concatenated" );
    case klogg::LogRowId::Origin::Memory: return QStringLiteral( "memory" );
    }
    return QStringLiteral( "unknown" );
}

// Process-wide admission survives controller reset until workers retire.
klogg::AsyncWorkBudget::Lease acquireReadPermit()
{
    static const klogg::AsyncWorkBudget budget{ Limits::MaxConcurrentReads };
    return budget.tryAcquire();
}
struct ReadOutcome {
    QString error;
    QJsonObject result;
    bool cancelled = false;
};
ReadOutcome readRows( const klogg::LogSnapshot::Ptr& snapshot, const QString& snapshotId,
                     uint64_t start, uint64_t count, const std::atomic<bool>& cancelled )
{
    try {
        uint64_t sourceBytes = 0;
        // Admit the entire physical batch before payload access. Never estimate
        // from decoded strings or silently fall back to an unbounded reader.
        for ( uint64_t offset = 0; offset < count; ++offset ) {
            if ( cancelled ) return { {}, {}, true };
            const auto bytes = snapshot->sourceBytesForLine( LineNumber{ start + offset } );
            if ( !bytes ) return { QStringLiteral( "READ_UNSUPPORTED" ), {}, false };
            if ( *bytes > Limits::MaxSourceBytes - sourceBytes )
                return { QStringLiteral( "RESOURCE_LIMIT" ), {}, false };
            sourceBytes += *bytes;
        }
        QJsonObject result{ { "snapshot_id", snapshotId }, { "start", QString::number( start ) },
                            { "source_bytes", QString::number( sourceBytes ) }, { "rows", QJsonArray{} } };
        auto outputBytes = static_cast<uint64_t>( QJsonDocument( result ).toJson( QJsonDocument::Compact ).size() );
        uint64_t textUnits = 0;
        QJsonArray rows;
        for ( uint64_t offset = 0; offset < count; ++offset ) {
            if ( cancelled ) return { {}, {}, true };
            const LineNumber index{ start + offset };
            auto line = snapshot->readLineBounded( index, Limits::MaxSourceBytes, Limits::MaxTextUnits - textUnits );
            textUnits += static_cast<uint64_t>( line.text.size() );
            const auto row = snapshot->row( index );
            const auto& identity = row.identity;
            QJsonObject serialized{
                { "row", QString::number( index.get() ) },
                { "source_line", QString::number( row.sourceLineNumber.get() ) },
                { "identity", QJsonObject{ { "origin", originName( identity.origin ) },
                    { "incarnation", QString::number( identity.incarnation ) },
                    { "member", QString::number( identity.member ) },
                    { "row", QString::number( identity.row ) } } },
                { "copyable", row.copyable }, { "match", row.match }, { "text", std::move( line.text ) } };
            const auto bytes = static_cast<uint64_t>( QJsonDocument( serialized ).toJson( QJsonDocument::Compact ).size() )
                + ( rows.isEmpty() ? 0U : 1U );
            if ( bytes > Limits::MaxOutputBytes - outputBytes )
                return { QStringLiteral( "RESOURCE_LIMIT" ), {}, false };
            outputBytes += bytes;
            rows.append( serialized );
        }
        if ( cancelled ) return { {}, {}, true };
        result.insert( "rows", rows );
        return { {}, std::move( result ), false };
    }
    catch ( const klogg::SnapshotLimitError& ) { return { QStringLiteral( "RESOURCE_LIMIT" ), {}, false }; }
    catch ( const klogg::SnapshotReadUnsupported& ) { return { QStringLiteral( "READ_UNSUPPORTED" ), {}, false }; }
    catch ( ... ) { return { QStringLiteral( "SOURCE_READ_FAILED" ), {}, false }; }
}
struct Lease {
    klogg::LogSnapshot::Ptr source;
    qint64 created;
};
struct Operation {
    QFuture<ReadOutcome> future;
    std::shared_ptr<std::atomic<bool>> cancelled;
    qint64 created;
    qint64 lifetime = Limits::LifetimeMs;
    std::function<void()> onCancel{};
    void cancel()
    {
        if ( future.isFinished() ) return;
        *cancelled = true;
        if ( onCancel ) onCancel();
    }
};
QJsonObject operationJson( const QString& id, const Operation& operation )
{
    QJsonObject value{ { "operation_id", id }, { "status", "pending" },
                       { "cancel_requested", operation.cancelled->load() } };
    if ( !operation.future.isFinished() ) return value;
    try {
        const auto outcome = operation.future.result();
        if ( outcome.cancelled ) value.insert( "status", "cancelled" );
        else if ( !outcome.error.isEmpty() ) {
            value.insert( "status", "failed" );
            value.insert( "error", QJsonObject{ { "code", outcome.error } } );
        }
        else {
            value.insert( "status", "completed" );
            value.insert( "result", outcome.result );
        }
    }
    catch ( ... ) {
        value.insert( "status", "failed" );
        value.insert( "error", QJsonObject{ { "code", "SOURCE_READ_FAILED" } } );
    }
    return value;
}
}

struct ReviewContentAdapter::State {
    explicit State( Clock clock ) : now( std::move( clock ) ) {}
    Clock now;
    std::map<QString, Lease> snapshots;
    std::map<QString, Operation> operations;
    QTimer timer;
    bool makeRoom()
    {
        if ( operations.size() < MaxOperations ) return true;
        const auto retired = std::find_if( operations.begin(), operations.end(),
            []( const auto& item ) { return item.second.future.isFinished(); } );
        if ( retired == operations.end() ) return false;
        operations.erase( retired );
        return true;
    }
    void expire()
    {
        const auto tick = now();
        for ( auto it = snapshots.begin(); it != snapshots.end(); ) {
            if ( tick - it->second.created >= LifetimeMs ) it = snapshots.erase( it );
            else ++it;
        }
        for ( auto it = operations.begin(); it != operations.end(); ) {
            if ( tick - it->second.created >= it->second.lifetime ) {
                it->second.cancel();
                it = operations.erase( it );
            }
            else ++it;
        }
        if ( snapshots.empty() && operations.empty() ) timer.stop();
    }
};

ReviewContentAdapter::ReviewContentAdapter( Clock now, QObject* parent )
    : QObject( parent ), state_( std::make_unique<State>( std::move( now ) ) )
{
    state_->timer.setInterval( 1000 );
    connect( &state_->timer, &QTimer::timeout, this, [this] { state_->expire(); } );
}
ReviewContentAdapter::~ReviewContentAdapter() { reset(); }
void ReviewContentAdapter::reset()
{
    Q_ASSERT( QThread::currentThread() == thread() );
    state_->timer.stop();
    for ( auto& item : state_->operations ) item.second.cancel();
    state_->operations.clear();
    state_->snapshots.clear();
}

std::optional<ReviewContentAdapter::PendingOperation> ReviewContentAdapter::beginOperation(
    std::function<void()> cancel, qint64 lifetimeMs )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    state_->expire();
    if ( !state_->makeRoom() ) return {};
    const auto id = QUuid::createUuid().toString( QUuid::WithoutBraces );
    auto cancelled = std::make_shared<std::atomic<bool>>( false );
    auto completed = std::make_shared<std::atomic<bool>>( false );
    auto promise = std::make_shared<QFutureInterface<ReadOutcome>>();
    promise->reportStarted();
    state_->operations.emplace( id, Operation{ promise->future(), cancelled, state_->now(),
                                              lifetimeMs, std::move( cancel ) } );
    state_->timer.start();
    const QPointer<ReviewContentAdapter> owner{ this };
    return PendingOperation{ id,
        [owner, id, cancelled] {
            if ( !owner ) return false;
            owner->state_->expire();
            const auto found = owner->state_->operations.find( id );
            return found != owner->state_->operations.end() && found->second.cancelled == cancelled
                && !cancelled->load() && !found->second.future.isFinished();
        },
        [promise, cancelled, completed]( QJsonObject result, QString error ) {
            if ( completed->exchange( true ) ) return;
            promise->reportResult( ReadOutcome{ std::move( error ), std::move( result ), cancelled->load() } );
            promise->reportFinished();
        } };
}

ReviewContentAdapter::ResolvedRows ReviewContentAdapter::resolveRows(
    const QString& snapshotId, const std::vector<LineNumber>& ordinals )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    state_->expire();
    if ( ordinals.size() > MaxRows ) return RowResolutionError::ResourceLimit;
    if ( ordinals.empty() ) return RowResolutionError::InvalidArgument;
    const auto found = state_->snapshots.find( snapshotId );
    if ( found == state_->snapshots.end() ) return RowResolutionError::SnapshotNotFound;
    const auto source = found->second.source;
    try {
        const auto count = source->lineCount();
        for ( const auto index : ordinals )
            if ( index >= count ) return RowResolutionError::InvalidArgument;
        RowMetadata rows;
        rows.reserve( ordinals.size() );
        for ( const auto index : ordinals ) {
            const auto row = source->row( index );
            if ( !row.copyable ) return RowResolutionError::InvalidArgument;
            rows.push_back( row );
        }
        return rows;
    }
    catch ( const std::bad_alloc& ) { return RowResolutionError::ResourceLimit; }
    catch ( ... ) { return RowResolutionError::SourceUnavailable; }
}

DecodedResponse ReviewContentAdapter::capture( const RpcRequest& request, klogg::LogSnapshot::Ptr snapshot )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    state_->expire();
    if ( !snapshot ) return failure( request, "TARGET_NOT_READY" );
    if ( state_->snapshots.size() >= MaxSnapshots ) return failure( request, "RESOURCE_LIMIT" );
    const auto id = QUuid::createUuid().toString( QUuid::WithoutBraces );
    const auto count = snapshot->lineCount().get();
    state_->snapshots.emplace( id, Lease{ std::move( snapshot ), state_->now() } );
    state_->timer.start();
    return QJsonObject{ { "snapshot_id", id }, { "row_count", QString::number( count ) },
                        { "lifetime_ms", static_cast<int>( LifetimeMs ) } };
}

DecodedResponse ReviewContentAdapter::dispatch( const RpcRequest& request )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    state_->expire();
    const auto& params = request.params;
    if ( request.method == QLatin1String( "operation.get" ) || request.method == QLatin1String( "operation.cancel" ) ) {
        const auto id = params.value( "operation_id" ).toString();
        if ( !onlyKeys( params, { "operation_id" } ) || id.isEmpty() ) return failure( request, "INVALID_PARAMS" );
        const auto found = state_->operations.find( id );
        if ( found == state_->operations.end() ) return failure( request, "OPERATION_NOT_FOUND" );
        if ( request.method == QLatin1String( "operation.cancel" ) ) found->second.cancel();
        return operationJson( id, found->second );
    }
    const bool release = request.method == QLatin1String( "source.release" );
    if ( !release && request.method != QLatin1String( "source.read" ) ) return failure( request, "METHOD_NOT_FOUND" );
    if ( release ? !onlyKeys( params, { "snapshot_id" } )
                 : !onlyKeys( params, { "snapshot_id", "start", "count" } ) ) return failure( request, "INVALID_PARAMS" );
    const auto id = params.value( "snapshot_id" ).toString();
    if ( id.isEmpty() ) return failure( request, "INVALID_PARAMS" );
    const auto lease = state_->snapshots.find( id );
    if ( lease == state_->snapshots.end() ) return failure( request, "SNAPSHOT_NOT_FOUND" );
    if ( release ) {
        state_->snapshots.erase( lease );
        return QJsonObject{ { "snapshot_id", id }, { "status", "released" } };
    }
    const auto start = ordinal( params.value( "start" ) );
    const auto countValue = params.value( "count" );
    const int count = countValue.toInt( -1 );
    if ( !start || !countValue.isDouble() || count < 1 || count > static_cast<int>( MaxRows )
         || countValue.toDouble() != static_cast<double>( count ) ) return failure( request, "INVALID_PARAMS" );
    const auto total = lease->second.source->lineCount().get();
    if ( *start > total || static_cast<uint64_t>( count ) > total - *start ) return failure( request, "INVALID_PARAMS" );
    if ( !state_->makeRoom() ) return failure( request, "RESOURCE_LIMIT" );
    auto permit = acquireReadPermit();
    if ( !permit ) return failure( request, "RESOURCE_LIMIT" );
    const auto operationId = QUuid::createUuid().toString( QUuid::WithoutBraces );
    auto cancelled = std::make_shared<std::atomic<bool>>( false );
    auto future = QtConcurrent::run( [source = lease->second.source, id, start = *start,
        count, cancelled, permit = std::move( permit )]() mutable {
        auto result = readRows( source, id, start, static_cast<uint64_t>( count ), *cancelled );
        source.reset();
        permit.reset();
        return result;
    } );
    state_->operations.emplace( operationId, Operation{ std::move( future ), std::move( cancelled ), state_->now() } );
    state_->timer.start();
    return QJsonObject{ { "operation_id", operationId }, { "status", "pending" } };
}
