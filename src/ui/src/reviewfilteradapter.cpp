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
#include "reviewtransaction.h"

namespace {
using namespace klogg::control;
using Status = ReviewTransaction::Status;
using Target = ReviewFilterAdapter::Target;
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
ReviewTransaction::Participant filterParticipant( Target target,
    std::shared_ptr<typename Widget::FilterUpdateStaging> staging,
    std::shared_ptr<PreparedFilter<Widget>> prepared )
{
    // Prepared data and its permit survive until commit or discard, but no
    // native publication guard is held while the transaction is open.
    struct Deferred { std::unique_ptr<typename Widget::FilterUpdatePublication> guard; };
    auto state = std::make_shared<Deferred>();
    return {
        [target = std::move( target ), staging = std::move( staging ), prepared = std::move( prepared ), state] {
            if ( !target.widget || !target.current() ) return Status::TargetClosed;
            state->guard = staging->lock( prepared->outcome.update );
            return state->guard ? Status::Open : Status::Conflict;
        },
        [state] { return state->guard->install(); },
        [state] { if ( state->guard ) state->guard->release(); },
        [state] { state->guard->notify(); }
    };
}

struct Delivery {
    Target target;
    Completion operation;
    std::shared_ptr<ReviewTransaction> transaction;
    QPointer<SearchToolbar> draftOwner;
    bool privateDocument;

    void fail( Status status, const QString& error ) const
    {
        if ( transaction ) transaction->failAsyncStage( status );
        operation.complete( {}, error );
    }

    template <class Widget>
    void publish( const std::shared_ptr<PreparedFilter<Widget>>& prepared,
                  const std::shared_ptr<typename Widget::FilterUpdateStaging>& staging ) const
    {
        if ( !operation.current() || ( transaction && transaction->status() != Status::Open ) ) {
            fail( Status::Aborted, "OPERATION_CANCELLED" );
            return;
        }
        if ( !target.widget || !target.current() ) { fail( Status::TargetClosed, "TARGET_CLOSED" ); return; }
        if ( prepared->outcome.status != SearchPreparation::Status::Ready ) {
            fail( Status::PreparationFailed, preparationError( prepared->outcome.status ) );
            return;
        }
        QJsonObject result{ { "window_id", target.windowId }, { "tab_id", target.tabId } };
        if ( transaction ) result.insert( "staged", true );
        else result.insert( "updated", true );
        if ( transaction && !privateDocument ) {
            if ( !draftOwner ) { fail( Status::TargetClosed, "TARGET_CLOSED" ); return; }
            auto status = transaction->stageParticipant( filterParticipant<Widget>( target, staging, prepared ) );
            if ( status == Status::Open ) status = transaction->stageFilterDraft( *draftOwner, staging->effectiveFilter() );
            if ( status != Status::Open ) {
                fail( status, status == Status::ResourceLimit ? "RESOURCE_LIMIT" : "CONFLICT" );
                return;
            }
            transaction->finishAsyncStage();
            operation.complete( result, {} );
            return;
        }
        // Implicit writes publish immediately. A hidden widget is already
        // transaction-private: install there so later scoped reads see its rows.
        auto publication = staging->lock( prepared->outcome.update );
        if ( !publication ) { fail( Status::Conflict, "CONFLICT" ); return; }
        result.insert( "pane_id", publication->paneId() );
        if ( ( !transaction && !operation.current() ) || !publication->install() ) {
            publication->release();
            fail( Status::Conflict, "CONFLICT" );
            return;
        }
        publication->release();
        if ( transaction ) transaction->finishAsyncStage();
        operation.complete( result, {} );
        // Native notifications include persistent search history; no private
        // filter (or aborted document) should escape through that side channel.
        if ( !privateDocument ) publication->notify();
    }
};
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
    return apply( request, std::move( target ), candidate, {}, false );
}

klogg::control::DecodedResponse ReviewFilterAdapter::apply( const klogg::control::RpcRequest& request,
    Target target, const SearchToolbar::FilterSpec& candidate,
    std::shared_ptr<ReviewTransaction> transaction, bool privateDocument )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    const auto reject = [&]( Status status, const char* code ) {
        if ( transaction ) transaction->failAsyncStage( status );
        return failure( request, code );
    };
    try {
        if ( privateDocument && !transaction ) return failure( request, "INVALID_PARAMS" );
        auto cancelled = std::make_shared<AtomicFlag>();
        if ( transaction ) {
            const auto status = transaction->beginAsyncStage( [cancelled] { cancelled->set(); } );
            if ( status != Status::Open ) return failure( request, "TRANSACTION_PREPARING" );
        }
        if ( !target.widget || !target.current || !target.current() ) return reject( Status::TargetClosed, "TARGET_NOT_FOUND" );
        static const klogg::AsyncWorkBudget budget{ MaxConcurrentPreparations };
        auto permit = budget.tryAcquire();
        if ( !permit ) return reject( Status::ResourceLimit, "RESOURCE_LIMIT" );
        // Only the native staging type varies across file, live and folder tabs.
        const auto start = [&]( auto* widget ) -> DecodedResponse {
            using Widget = std::remove_pointer_t<decltype( widget )>;
            using Prepared = PreparedFilter<Widget>;
            const auto draftOwner = widget->reviewControls().filter;
            if ( !draftOwner ) return reject( Status::PreparationFailed, "FILTER_NOT_READY" );
            if ( transaction && !privateDocument && !transaction->filterDraft( *draftOwner ) ) {
                return reject( Status::ResourceLimit, "RESOURCE_LIMIT" );
            }
            std::shared_ptr<typename Widget::FilterUpdateStaging> staging = widget->stageFilterUpdate( candidate );
            if ( !staging ) return reject( Status::PreparationFailed, "FILTER_NOT_READY" );
            const auto captured = staging->capture();
            auto operation = operations_.beginOperation( [cancelled, weak = std::weak_ptr<ReviewTransaction>( transaction )] {
                cancelled->set();
                if ( const auto stagedEdits = weak.lock() ) stagedEdits->failAsyncStage( Status::Aborted );
            }, PreparationLifetimeMs );
            if ( !operation ) return reject( Status::ResourceLimit, "RESOURCE_LIMIT" );
            try {
                auto watcher = std::make_unique<QFutureWatcher<std::shared_ptr<Prepared>>>( this );
                connect( watcher.get(), &QFutureWatcherBase::finished, this,
                    [watcher = watcher.get(), delivery = Delivery{ target, *operation, transaction, draftOwner, privateDocument }, staging] {
                        watcher->deleteLater();
                        try { delivery.publish( watcher->result(), staging ); }
                        catch ( const std::bad_alloc& ) { delivery.fail( Status::ResourceLimit, "RESOURCE_LIMIT" ); }
                        catch ( ... ) { delivery.fail( Status::PreparationFailed, "FILTER_PREPARATION_FAILED" ); }
                    } );
                watcher->setFuture( QtConcurrent::run( [captured, cancelled, permit = std::move( permit )] {
                    auto prepared = std::make_shared<Prepared>();
                    prepared->permit = permit;
                    prepared->outcome = Widget::prepareFilterUpdate( captured, *cancelled );
                    return prepared;
                } ) );
                watcher.release(); // QObject parent owns delivery; worker owns only values.
            } catch ( ... ) {
                cancelled->set();
                if ( transaction ) transaction->failAsyncStage( Status::PreparationFailed );
                operation->complete( {}, "FILTER_PREPARATION_FAILED" );
            }
            return QJsonObject{ { "operation_id", operation->id }, { "status", "pending" } };
        };
        if ( auto* file = qobject_cast<CrawlerWidget*>( target.widget.data() ) ) return start( file );
        if ( auto* folder = qobject_cast<FolderCrawlerWidget*>( target.widget.data() ) ) return start( folder );
        return reject( Status::InvalidArgument, "UNSUPPORTED_SOURCE" );
    } catch ( const std::bad_alloc& ) { return reject( Status::ResourceLimit, "RESOURCE_LIMIT" ); }
    catch ( ... ) { return reject( Status::PreparationFailed, "FILTER_NOT_READY" ); }
}
