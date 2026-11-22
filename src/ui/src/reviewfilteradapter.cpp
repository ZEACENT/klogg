#include "reviewfilteradapter.h"

#include <memory>
#include <new>
#include <utility>
#include <QFutureWatcher>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include "asyncworkbudget.h"
#include "crawlerwidget.h"
#include "reviewcontentadapter.h"

namespace {
using namespace klogg::control;
using Completion = ReviewContentAdapter::PendingOperation;
struct PreparedFilter {
    CrawlerWidget::FilterUpdatePreparation outcome;
    klogg::AsyncWorkBudget::Lease permit;
};
RpcError failure( const RpcRequest& request, const char* code )
{
    return { request.id, -32000, QStringLiteral( "Filter request rejected" ),
             QJsonObject{ { "code", code } }, true, false };
}
QString preparationError( SearchPreparation::Status status )
{
    switch ( status ) {
    case SearchPreparation::Status::CompileFailure: return QStringLiteral( "FILTER_COMPILE_FAILED" );
    case SearchPreparation::Status::Cancelled: return QStringLiteral( "OPERATION_CANCELLED" );
    default: return QStringLiteral( "FILTER_PREPARATION_FAILED" );
    }
}
void publishFilter( ReviewFilterAdapter::Target target, Completion operation,
                    const std::shared_ptr<PreparedFilter>& prepared,
                    const std::shared_ptr<CrawlerWidget::FilterUpdateStaging>& staging )
{
    try {
        if ( !operation.current() ) { operation.complete( {}, "OPERATION_CANCELLED" ); return; }
        if ( !target.widget || !target.current() ) { operation.complete( {}, "TARGET_CLOSED" ); return; }
        if ( prepared->outcome.status != SearchPreparation::Status::Ready ) {
            operation.complete( {}, preparationError( prepared->outcome.status ) );
            return;
        }
        auto publication = staging->lock( prepared->outcome.update );
        if ( !publication ) { operation.complete( {}, "CONFLICT" ); return; }
        // Allocate the complete response before installing any visible state.
        // Content and result leases still require explicit source.capture/read.
        const QJsonObject result{ { "window_id", target.windowId }, { "tab_id", target.tabId },
                                  { "pane_id", publication->paneId() }, { "updated", true } };
        if ( !operation.current() || !publication->install() ) {
            operation.complete( {}, "CONFLICT" );
            return;
        }
        publication->release();
        operation.complete( result, {} );
        publication->notify(); // No adapter/target state access after observers run.
    } catch ( const std::bad_alloc& ) {
        operation.complete( {}, "RESOURCE_LIMIT" );
    } catch ( ... ) {
        operation.complete( {}, "FILTER_PREPARATION_FAILED" );
    }
}
}

ReviewFilterAdapter::ReviewFilterAdapter( ReviewContentAdapter& operations )
    : operations_( operations ) {}
ReviewFilterAdapter::~ReviewFilterAdapter() { reset(); }
void ReviewFilterAdapter::reset()
{
    // The shared registry cancels before this delivery layer is reset. Worker
    // captures are value-only, so watcher destruction never joins GUI work.
    for ( auto* child : children() ) {
        child->disconnect( this );
        child->deleteLater();
    }
}

klogg::control::DecodedResponse ReviewFilterAdapter::apply( const klogg::control::RpcRequest& request,
    Target target, const SearchToolbar::FilterSpec& candidate )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if ( !target.widget || !target.current || !target.current() ) return failure( request, "TARGET_NOT_FOUND" );
    static const klogg::AsyncWorkBudget budget{ MaxConcurrentPreparations };
    auto permit = budget.tryAcquire();
    if ( !permit ) return failure( request, "RESOURCE_LIMIT" );
    std::shared_ptr<CrawlerWidget::FilterUpdateStaging> staging;
    try {
        staging = target.widget->stageFilterUpdate( candidate );
    } catch ( const std::bad_alloc& ) { return failure( request, "RESOURCE_LIMIT" ); }
    catch ( ... ) { return failure( request, "FILTER_NOT_READY" ); }
    if ( !staging ) return failure( request, "FILTER_NOT_READY" );
    const auto captured = staging->capture();
    auto cancelled = std::make_shared<AtomicFlag>();
    auto operation = operations_.beginOperation( [cancelled] { cancelled->set(); }, PreparationLifetimeMs );
    if ( !operation ) return failure( request, "RESOURCE_LIMIT" );
    auto future = QtConcurrent::run( [captured, cancelled, permit = std::move( permit )] {
        auto prepared = std::make_shared<PreparedFilter>();
        prepared->permit = permit;
        prepared->outcome = CrawlerWidget::prepareFilterUpdate( captured, *cancelled );
        return prepared;
    } );
    auto* watcher = new QFutureWatcher<std::shared_ptr<PreparedFilter>>( this );
    connect( watcher, &QFutureWatcherBase::finished, this,
        [watcher, target = std::move( target ), operation = *operation, staging = std::move( staging )] {
            watcher->deleteLater();
            try { publishFilter( target, operation, watcher->result(), staging ); }
            catch ( ... ) { operation.complete( {}, "FILTER_PREPARATION_FAILED" ); }
        } );
    watcher->setFuture( future );
    return QJsonObject{ { "operation_id", operation->id }, { "status", "pending" } };
}
