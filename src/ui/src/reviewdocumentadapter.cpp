#include "reviewdocumentadapter.h"

#include <algorithm>
#include <memory>
#include <new>
#include <utility>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include "abstractcrawlerwidget.h"
#include "asyncworkbudget.h"
#include "folderenumeration.h"
#include "logdata.h"
#include "mainwindow.h"
#include "preparedfilesource.h"
#include "reviewcontentadapter.h"
#include "tabgroup.h"

namespace {
using namespace klogg::control;
using Completion = ReviewContentAdapter::PendingOperation;

RpcError failure( const RpcRequest& request, const char* code )
{
    return { request.id, QString::fromLatin1( code ) == QLatin1String( "INVALID_PARAMS" ) ? -32602 : -32000,
             QStringLiteral( "Document request rejected" ), QJsonObject{ { "code", code } }, true, false };
}
bool onlyKeys( const QJsonObject& params, std::initializer_list<const char*> allowed )
{
    for ( auto it = params.begin(); it != params.end(); ++it ) {
        if ( std::none_of( allowed.begin(), allowed.end(), [&]( const char* key ) {
                 return it.key() == QLatin1String( key );
             } ) ) return false;
    }
    return true;
}
QJsonObject outcome( const QString& windowId, const QString& tabId, bool opened )
{
    return { { "window_id", windowId }, { "tab_id", tabId }, { "opened", opened } };
}
QJsonObject completed( const Completion& operation, const QJsonObject& result )
{
    return { { "operation_id", operation.id }, { "status", "completed" }, { "result", result } };
}
QString acquisitionError( FileSourcePreparation::Status status )
{
    using Status = FileSourcePreparation::Status;
    switch ( status ) {
    case Status::Missing: return QStringLiteral( "SOURCE_NOT_FOUND" );
    case Status::NotRegular: return QStringLiteral( "SOURCE_NOT_REGULAR" );
    case Status::ResourceLimit: return QStringLiteral( "RESOURCE_LIMIT" );
    case Status::Cancelled: return QStringLiteral( "OPERATION_CANCELLED" );
    default: return QStringLiteral( "SOURCE_PREPARATION_FAILED" );
    }
}
struct PreparedOpen {
    QString error;
    std::unique_ptr<PreparedFileSource> file;
    std::vector<QString> members;
    klogg::AsyncWorkBudget::Lease permit;
};

void publishOpen( QPointer<MainWindow> window, const std::shared_ptr<Session>& session,
                  const QString& path, DocumentKind kind, bool follow, const Completion& operation,
                  const std::shared_ptr<PreparedOpen>& prepared )
{
    std::unique_ptr<Session::PreparedDocument> document;
    try {
        if ( !operation.current() ) { operation.complete( {}, "OPERATION_CANCELLED" ); return; }
        if ( !window ) { operation.complete( {}, "TARGET_CLOSED" ); return; }
        // A human may have opened the path while preparation ran. Preserve that
        // document and its selection rather than inserting an unwanted duplicate.
        for ( const auto& tab : window->inventorySnapshot().tabs ) {
            if ( tab.widget && tab.kind == kind && tab.documentId == path ) {
                operation.complete( outcome( window->runtimeWindowId(), tab.tabId, true ), {} );
                return;
            }
        }
        if ( !prepared->error.isEmpty() ) { operation.complete( {}, prepared->error ); return; }
        document = kind == DocumentKind::File
            ? window->prepareFile( prepared->file, follow )
            : window->prepareFolder( path, prepared->members );
        if ( !document ) { operation.complete( {}, "SOURCE_PREPARATION_FAILED" ); return; }
        if ( kind == DocumentKind::File && !document->startLoading() ) {
            session->retirePrepared( document );
            operation.complete( {}, "SOURCE_PREPARATION_FAILED" );
            return;
        }
        if ( !operation.current() || !window ) {
            session->retirePrepared( document );
            operation.complete( {}, "TARGET_CLOSED" );
            return;
        }
        QPointer<QWidget> widget{ dynamic_cast<QWidget*>( document->view() ) };
        auto* crawler = dynamic_cast<AbstractCrawlerWidget*>( document->view() );
        QPointer<LogData> source{ dynamic_cast<LogData*>( document->source() ) };
        const auto result = outcome( window->runtimeWindowId(), crawler->runtimeTabId(), true );
        auto publication = window->beginPublication();
        if ( !publication || !publication->install( document ) ) {
            session->retirePrepared( document );
            operation.complete( {}, "CONFLICT" );
            return;
        }
        publication->finalize();
        publication->release();
        operation.complete( result, {} ); // Terminal before any native observer.
        publication->notify();
        if ( window && widget && source ) {
            const auto inventory = window->inventorySnapshot();
            if ( std::any_of( inventory.tabs.begin(), inventory.tabs.end(), [&]( const auto& tab ) {
                     return tab.widget == widget;
                 } ) ) source->activatePrepared();
        }
        if ( window ) window->recordPublishedDocument( path, kind, true );
    } catch ( const std::bad_alloc& ) {
        if ( document ) session->retirePrepared( document );
        operation.complete( {}, "RESOURCE_LIMIT" );
    } catch ( ... ) {
        if ( document ) session->retirePrepared( document );
        operation.complete( {}, "SOURCE_PREPARATION_FAILED" );
    }
}
}

ReviewDocumentAdapter::ReviewDocumentAdapter( WindowProvider windows, ReviewContentAdapter& operations )
    : windows_( std::move( windows ) ), operations_( operations )
{
}
ReviewDocumentAdapter::~ReviewDocumentAdapter() { reset(); }
void ReviewDocumentAdapter::reset()
{
    // The shared operation registry owns cancellation. Workers own only values;
    // disconnecting completion delivery never joins one on the GUI thread.
    for ( auto* child : children() ) {
        child->disconnect( this );
        child->deleteLater();
    }
}

DecodedResponse ReviewDocumentAdapter::dispatch( const RpcRequest& request )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    const bool opening = request.method == QLatin1String( "documents.open" );
    if ( !opening && request.method != QLatin1String( "documents.close" ) ) return failure( request, "METHOD_NOT_FOUND" );
    const auto& params = request.params;
    if ( opening ? !onlyKeys( params, { "window_id", "path", "kind", "follow" } )
                 : !onlyKeys( params, { "window_id", "tab_id" } ) ) return failure( request, "INVALID_PARAMS" );
    const auto windowId = params.value( "window_id" );
    if ( !windowId.isString() || windowId.toString().isEmpty() ) return failure( request, "INVALID_PARAMS" );
    const auto windows = windows_();
    QPointer<MainWindow> window;
    for ( const auto& candidate : windows ) {
        if ( candidate && candidate->runtimeWindowId() == windowId.toString() ) window = candidate;
    }
    if ( !window ) return failure( request, "TARGET_NOT_FOUND" );
    const auto session = window->sessionOwner();
    if ( !opening ) {
        const auto tabId = params.value( "tab_id" );
        if ( !tabId.isString() || tabId.toString().isEmpty() ) return failure( request, "INVALID_PARAMS" );
        const auto inventory = window->inventorySnapshot();
        const auto target = std::find_if( inventory.tabs.begin(), inventory.tabs.end(), [&]( const auto& tab ) {
            return tab.tabId == tabId.toString();
        } );
        if ( target == inventory.tabs.end() || !target->widget ) return failure( request, "TARGET_NOT_FOUND" );
        if ( target->kind == DocumentKind::AdbLogcat ) return failure( request, "UNSUPPORTED_SOURCE" );
        auto operation = operations_.beginOperation( {} );
        if ( !operation ) return failure( request, "RESOURCE_LIMIT" );
        auto groups = TabGroupManager::get().prepareTabRemovals( { target->documentId } );
        std::vector<std::unique_ptr<MainWindow::Publication>> publications;
        MainWindow::Publication* targetPublication = nullptr;
        for ( const auto& owner : windows ) {
            if ( !owner ) continue;
            auto publication = owner->beginPublication();
            if ( !publication ) {
                operation->complete( {}, "CONFLICT" );
                return QJsonObject{ { "operation_id", operation->id }, { "status", "failed" },
                                    { "error", QJsonObject{ { "code", "CONFLICT" } } } };
            }
            if ( owner == window ) targetPublication = publication.get();
            publications.push_back( std::move( publication ) );
        }
        if ( !targetPublication || !groups->isCurrent() || !operation->current() ) {
            operation->complete( {}, "CONFLICT" );
            return failure( request, "CONFLICT" );
        }
        auto detached = targetPublication->extract( dynamic_cast<AbstractCrawlerWidget*>( target->widget.data() ) );
        if ( !detached ) { operation->complete( {}, "CONFLICT" ); return failure( request, "CONFLICT" ); }
        groups->install();
        for ( const auto& publication : publications ) publication->finalize();
        for ( const auto& publication : publications ) publication->release();
        const auto result = outcome( windowId.toString(), tabId.toString(), false );
        const auto response = completed( *operation, result );
        operation->complete( result, {} );
        for ( const auto& publication : publications ) publication->notify();
        groups->notify();
        session->retirePrepared( detached );
        TabGroupManager::get().save();
        if ( window ) window->recordPublishedDocument( target->documentId, target->kind, false );
        return response;
    }

    const auto pathValue = params.value( "path" );
    const auto kindValue = params.value( "kind" );
    const auto followValue = params.value( "follow" );
    if ( !pathValue.isString() || pathValue.toString().isEmpty() || pathValue.toString().size() > 32768
         || pathValue.toString().contains( QChar{} ) || !QDir::isAbsolutePath( pathValue.toString() )
         || ( !kindValue.isUndefined() && kindValue != "file" && kindValue != "folder" )
         || ( !followValue.isUndefined() && !followValue.isBool() ) ) return failure( request, "INVALID_PARAMS" );
    const auto path = QDir::cleanPath( pathValue.toString() );
    const auto kind = kindValue == "folder" ? DocumentKind::Folder : DocumentKind::File;
    if ( kind == DocumentKind::Folder && !followValue.isUndefined() ) return failure( request, "INVALID_PARAMS" );
    for ( const auto& tab : window->inventorySnapshot().tabs ) {
        if ( tab.widget && tab.kind == kind && tab.documentId == path ) {
            auto operation = operations_.beginOperation( {} );
            if ( !operation ) return failure( request, "RESOURCE_LIMIT" );
            const auto result = outcome( window->runtimeWindowId(), tab.tabId, true );
            operation->complete( result, {} );
            return completed( *operation, result );
        }
    }
    static const klogg::AsyncWorkBudget budget{ MaxConcurrentPreparations };
    auto permit = budget.tryAcquire();
    if ( !permit ) return failure( request, "RESOURCE_LIMIT" );
    auto cancelled = std::make_shared<AtomicFlag>();
    auto operation = operations_.beginOperation( [cancelled] { cancelled->set(); }, PreparationLifetimeMs );
    if ( !operation ) return failure( request, "RESOURCE_LIMIT" );
    const auto settings = IndexingSettings::capture();
    auto future = QtConcurrent::run( [path, kind, settings, cancelled, permit = std::move( permit )] {
        auto result = std::make_shared<PreparedOpen>();
        result->permit = permit;
        try {
            if ( kind == DocumentKind::File ) {
                auto source = prepareFileSource( path, settings, *cancelled );
                if ( source.status != FileSourcePreparation::Status::Ready ) result->error = acquisitionError( source.status );
                result->file = std::move( source.source );
            } else if ( !QFileInfo( path ).isDir() ) result->error = "SOURCE_NOT_FOUND";
            else {
                result->members = enumerateFolderFiles( path, {}, [cancelled] { return static_cast<bool>( *cancelled ); } );
                if ( result->members.empty() ) result->error = "EMPTY_FOLDER";
            }
        } catch ( const std::bad_alloc& ) { result->error = "RESOURCE_LIMIT"; }
        catch ( ... ) { result->error = "SOURCE_PREPARATION_FAILED"; }
        return result;
    } );
    auto* watcher = new QFutureWatcher<std::shared_ptr<PreparedOpen>>( this );
    connect( watcher, &QFutureWatcher<std::shared_ptr<PreparedOpen>>::finished, this,
             [watcher, window, session, path, kind, follow = followValue.toBool(), operation = *operation] {
        watcher->deleteLater();
        try { publishOpen( window, session, path, kind, follow, operation, watcher->result() ); }
        catch ( ... ) { operation.complete( {}, "SOURCE_PREPARATION_FAILED" ); }
    } );
    watcher->setFuture( future );
    return QJsonObject{ { "operation_id", operation->id }, { "status", "pending" } };
}
