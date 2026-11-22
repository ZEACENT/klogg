#include "reviewfilteradapter.h"

#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <QFutureWatcher>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include "asyncworkbudget.h"
#include "crawlerwidget.h"
#include "foldercrawlerwidget.h"
#include "reviewcontentadapter.h"

namespace {
using namespace klogg::control;
using Completion = ReviewContentAdapter::PendingOperation;
template <class Widget> struct PreparedFilter {
    typename Widget::FilterUpdatePreparation outcome;
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
template <class Widget>
void publishFilter( ReviewFilterAdapter::Target target, Completion operation,
                    const std::shared_ptr<PreparedFilter<Widget>>& prepared,
                    const std::shared_ptr<typename Widget::FilterUpdateStaging>& staging )
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
    // Dispatch only the native staging type. Admission budget, cancellation,
    // operation lifetime and the publication boundary are shared by all tabs.
    const auto start = [&]( auto* widget ) -> DecodedResponse {
        using Widget = std::remove_pointer_t<decltype( widget )>;
        using Prepared = PreparedFilter<Widget>;
        std::shared_ptr<typename Widget::FilterUpdateStaging> staging = widget->stageFilterUpdate( candidate );
        if ( !staging ) return failure( request, "FILTER_NOT_READY" );
        const auto captured = staging->capture();
        auto cancelled = std::make_shared<AtomicFlag>();
        auto operation = operations_.beginOperation( [cancelled] { cancelled->set(); }, PreparationLifetimeMs );
        if ( !operation ) return failure( request, "RESOURCE_LIMIT" );
        try {
            auto watcher = std::make_unique<QFutureWatcher<std::shared_ptr<Prepared>>>( this );
            connect( watcher.get(), &QFutureWatcherBase::finished, this,
                [delivery = watcher.get(), target, op = *operation, staging = std::move( staging )] {
                    delivery->deleteLater();
                    try { publishFilter( target, op, delivery->result(), staging ); }
                    catch ( ... ) { op.complete( {}, "FILTER_PREPARATION_FAILED" ); }
                } );
            watcher->setFuture( QtConcurrent::run( [captured, cancelled, permit = std::move( permit )] {
                auto prepared = std::make_shared<Prepared>();
                prepared->permit = permit;
                prepared->outcome = Widget::prepareFilterUpdate( captured, *cancelled );
                return prepared;
            } ) );
            watcher.release(); // QObject parent owns delivery, worker owns only values.
        } catch ( ... ) {
            cancelled->set();
            operation->complete( {}, "FILTER_PREPARATION_FAILED" );
        }
        return QJsonObject{ { "operation_id", operation->id }, { "status", "pending" } };
    };
    try {
        if ( auto* file = qobject_cast<CrawlerWidget*>( target.widget.data() ) ) return start( file );
        if ( auto* folder = qobject_cast<FolderCrawlerWidget*>( target.widget.data() ) ) return start( folder );
        return failure( request, "UNSUPPORTED_SOURCE" );
    } catch ( const std::bad_alloc& ) { return failure( request, "RESOURCE_LIMIT" ); }
    catch ( ... ) { return failure( request, "FILTER_NOT_READY" ); }
}
