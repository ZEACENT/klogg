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
#include "reviewtransaction.h"
#include "tabgroup.h"

namespace {
using namespace klogg::control;
using Completion = ReviewContentAdapter::PendingOperation;
using Status = ReviewTransaction::Status;

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
QJsonObject outcome( const QString& windowId, const QString& tabId, bool opened, bool staged = false )
{
    QJsonObject result{ { "window_id", windowId }, { "tab_id", tabId }, { "opened", opened } };
    if ( staged ) result.insert( "staged", true );
    return result;
}
QJsonObject completed( const Completion& operation, const QJsonObject& result )
{
    return { { "operation_id", operation.id }, { "status", "completed" }, { "result", result } };
}
QString acquisitionError( FileSourcePreparation::Status status )
{
    using SourceStatus = FileSourcePreparation::Status;
    switch ( status ) {
    case SourceStatus::Missing: return QStringLiteral( "SOURCE_NOT_FOUND" );
    case SourceStatus::NotRegular: return QStringLiteral( "SOURCE_NOT_REGULAR" );
    case SourceStatus::ResourceLimit: return QStringLiteral( "RESOURCE_LIMIT" );
    case SourceStatus::Cancelled: return QStringLiteral( "OPERATION_CANCELLED" );
    default: return QStringLiteral( "SOURCE_PREPARATION_FAILED" );
    }
}
const char* stageError( Status status )
{
    switch ( status ) {
    case Status::TargetClosed: return "TARGET_CLOSED";
    case Status::ResourceLimit: return "RESOURCE_LIMIT";
    case Status::Aborted: return "TRANSACTION_ABORTED";
    case Status::InvalidArgument: return "INVALID_PARAMS";
    case Status::PreparationFailed: return "SOURCE_PREPARATION_FAILED";
    default: return "CONFLICT";
    }
}
struct PreparedOpen {
    QString error;
    std::unique_ptr<PreparedFileSource> file;
    std::vector<QString> members;
    klogg::AsyncWorkBudget::Lease permit;
};

QFuture<std::shared_ptr<PreparedOpen>> acquire( const QString& path, DocumentKind kind,
    const std::shared_ptr<AtomicFlag>& cancelled, klogg::AsyncWorkBudget::Lease permit )
{
    return QtConcurrent::run( [path, kind, settings = IndexingSettings::capture(), cancelled, permit = std::move( permit )] {
        auto result = std::make_shared<PreparedOpen>();
        result->permit = permit;
        if ( kind == DocumentKind::File ) {
            auto source = prepareFileSource( path, settings, *cancelled );
            if ( source.status != FileSourcePreparation::Status::Ready ) result->error = acquisitionError( source.status );
            result->file = std::move( source.source );
        } else if ( !QFileInfo( path ).isDir() ) result->error = "SOURCE_NOT_FOUND";
        else {
            result->members = enumerateFolderFiles( path, {}, [cancelled] { return static_cast<bool>( *cancelled ); } );
            if ( result->members.empty() ) result->error = "EMPTY_FOLDER";
        }
        return result;
    } );
}

std::optional<MainWindow::TabInventoryEntry> findPath( const MainWindow& window, DocumentKind kind,
                                                       const QString& path )
{
    for ( const auto& tab : window.inventorySnapshot().tabs ) {
        if ( tab.widget && tab.kind == kind && tab.documentId == path ) return tab;
    }
    return {};
}

// Held by the participant, never by the staged-tab lookup or a worker. Until
// adoption the entire native graph is private; after extraction it follows the
// same retirement path. No GUI barrier is held while waiting for a commit.
struct Lifecycle {
    enum class Action { Open, Close, Reuse };
    Lifecycle( Action action_, MainWindow& window_, const QString& path_, DocumentKind kind_ )
        : action( action_ ), window( &window_ ), session( window_.sessionOwner() ), path( path_ ), kind( kind_ ),
          groupId( TabGroupManager::get().groupIdForTab( path ) ) {}
    ~Lifecycle() { if ( document ) session->retirePrepared( document ); }

    Status validate() const
    {
        if ( !window ) return Status::TargetClosed;
        if ( action == Action::Open ) {
            if ( findPath( *window, kind, path ) || TabGroupManager::get().groupIdForTab( path ) != groupId )
                return Status::Conflict;
        } else {
            const auto tabs = window->inventorySnapshot().tabs;
            const auto member = std::find_if( tabs.begin(), tabs.end(), [&]( const auto& entry ) {
                return widget && entry.widget == widget && entry.tabId == tabId && entry.kind == kind && entry.documentId == path;
            } );
            if ( member == tabs.end() ) return Status::TargetClosed;
            if ( groups && !groups->isCurrent() ) return Status::Conflict;
        }
        return Status::Open;
    }
    bool install()
    {
        if ( action == Action::Reuse ) return true;
        const auto owners = action == Action::Close ? windows() : std::vector<QPointer<MainWindow>>{ window };
        MainWindow::Publication* target = nullptr;
        publications.reserve( owners.size() );
        for ( const auto& owner : owners ) {
            if ( !owner ) continue;
            auto publication = owner->beginPublication();
            if ( !publication ) return false;
            if ( owner == window ) target = publication.get();
            publications.push_back( std::move( publication ) );
        }
        if ( !target ) return false;
        if ( action == Action::Open ) {
            if ( !target->install( document ) ) return false;
        } else {
            document = target->extract( dynamic_cast<AbstractCrawlerWidget*>( widget.data() ) );
            if ( !document ) return false;
            groups->install(); // Validated on this GUI turn; no intervening notification.
        }
        for ( const auto& publication : publications ) publication->finalize();
        return true;
    }
    void release()
    {
        for ( const auto& publication : publications ) publication->release();
    }
    void notify()
    {
        for ( const auto& publication : publications ) publication->notify();
        if ( action == Action::Reuse ) return;
        if ( groups ) {
            groups->notify();
            session->retirePrepared( document );
            TabGroupManager::get().save();
        } else if ( window && widget && source ) {
            const auto tabs = window->inventorySnapshot().tabs;
            if ( std::any_of( tabs.begin(), tabs.end(), [&]( const auto& entry ) { return entry.widget == widget; } ) )
                source->activatePrepared();
        }
        if ( window ) window->recordPublishedDocument( path, kind, action == Action::Open );
    }

    Action action;
    klogg::AsyncWorkBudget::Lease privateDocumentPermit;
    QPointer<MainWindow> window;
    std::shared_ptr<Session> session;
    QString path;
    DocumentKind kind;
    QString groupId;
    QString tabId;
    QPointer<QWidget> widget;
    QPointer<LogData> source;
    std::unique_ptr<Session::PreparedDocument> document;
    ReviewDocumentAdapter::WindowProvider windows;
    std::unique_ptr<TabGroupManager::PreparedRemoval> groups;
    std::vector<std::unique_ptr<MainWindow::Publication>> publications;
};

// The operation certifies staging in an explicit transaction. An implicit
// command additionally commits here and is terminal before native observers.
Status stage( const std::shared_ptr<Lifecycle>& candidate, const std::shared_ptr<ReviewTransaction>& transaction,
              const Completion& operation, const QJsonObject& result )
{
    ReviewTransaction::Participant participant{ [candidate] { return candidate->validate(); },
        [candidate] { return candidate->install(); }, [candidate] { candidate->release(); },
        [candidate] { candidate->notify(); } };
    Status status;
    if ( transaction ) {
        status = transaction->stageParticipant( std::move( participant ) );
        if ( status == Status::Open ) {
            transaction->finishAsyncStage();
            operation.complete( result, {} );
        }
    } else {
        ReviewTransaction implicit;
        participant.notify = [&] {
            operation.complete( result, {} );
            candidate->notify();
        };
        status = implicit.stageParticipant( std::move( participant ) );
        if ( status == Status::Open ) status = implicit.commit();
    }
    if ( status != Status::Open && status != Status::Committed ) operation.complete( {}, stageError( status ) );
    return status;
}
}

ReviewDocumentAdapter::ReviewDocumentAdapter( WindowProvider windows, ReviewContentAdapter& operations )
    : windows_( std::move( windows ) ), operations_( operations )
{
}
ReviewDocumentAdapter::~ReviewDocumentAdapter() { reset(); }
void ReviewDocumentAdapter::reset()
{
    staged_.clear();
    for ( auto* child : children() ) {
        child->disconnect( this );
        child->deleteLater();
    }
}

std::optional<ReviewDocumentAdapter::StagedDocument> ReviewDocumentAdapter::stagedDocument(
    const std::shared_ptr<ReviewTransaction>& transaction, const QString& tabId ) const
{
    if ( !transaction || ( transaction->status() != Status::Open && transaction->status() != Status::Preparing ) ) return {};
    for ( const auto& entry : staged_ ) {
        if ( entry.tabId == tabId && entry.window && entry.document.widget && entry.transaction.lock() == transaction )
            return entry.document;
    }
    return {};
}

DecodedResponse ReviewDocumentAdapter::dispatch( const RpcRequest& request, const std::shared_ptr<ReviewTransaction>& transaction )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    const bool opening = request.method == QLatin1String( "documents.open" );
    if ( !opening && request.method != QLatin1String( "documents.close" ) ) return failure( request, "METHOD_NOT_FOUND" );
    const auto& params = request.params;
    if ( opening ? !onlyKeys( params, { "window_id", "path", "kind", "follow" } )
                 : !onlyKeys( params, { "window_id", "tab_id" } ) ) return failure( request, "INVALID_PARAMS" );
    const auto windowId = params.value( "window_id" );
    if ( !windowId.isString() || windowId.toString().isEmpty() ) return failure( request, "INVALID_PARAMS" );
    QPointer<MainWindow> window;
    for ( const auto& candidate : windows_() ) {
        if ( candidate && candidate->runtimeWindowId() == windowId.toString() ) window = candidate;
    }
    if ( !window ) return failure( request, "TARGET_NOT_FOUND" );
    const auto stageExisting = [&]( const MainWindow::TabInventoryEntry& tab, Lifecycle::Action action ) -> DecodedResponse {
        auto candidate = std::make_shared<Lifecycle>( action, *window, tab.documentId, tab.kind );
        candidate->tabId = tab.tabId;
        candidate->widget = tab.widget;
        if ( action == Lifecycle::Action::Close ) {
            candidate->windows = windows_;
            candidate->groups = TabGroupManager::get().prepareTabRemovals( { tab.documentId } );
        }
        auto operation = operations_.beginOperation( {} );
        if ( !operation ) return failure( request, "RESOURCE_LIMIT" );
        const auto result = outcome( window->runtimeWindowId(), tab.tabId, opening, bool( transaction ) );
        const auto status = stage( candidate, transaction, *operation, result );
        if ( status != Status::Open && status != Status::Committed ) return failure( request, stageError( status ) );
        return completed( *operation, result );
    };
    if ( !opening ) {
        const auto tabId = params.value( "tab_id" );
        if ( !tabId.isString() || tabId.toString().isEmpty() ) return failure( request, "INVALID_PARAMS" );
        for ( const auto& tab : window->inventorySnapshot().tabs ) {
            if ( tab.tabId != tabId.toString() || !tab.widget ) continue;
            if ( tab.kind == DocumentKind::AdbLogcat ) return failure( request, "UNSUPPORTED_SOURCE" );
            return stageExisting( tab, Lifecycle::Action::Close );
        }
        return failure( request, "TARGET_NOT_FOUND" );
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
    if ( const auto existing = findPath( *window, kind, path ) ) return stageExisting( *existing, Lifecycle::Action::Reuse );

    static const klogg::AsyncWorkBudget budget{ MaxConcurrentPreparations };
    auto permit = budget.tryAcquire();
    if ( !permit ) return failure( request, "RESOURCE_LIMIT" );
    auto cancelled = std::make_shared<AtomicFlag>();
    const std::weak_ptr<ReviewTransaction> weakTx = transaction;
    auto operation = operations_.beginOperation( [cancelled, weakTx] {
        cancelled->set();
        if ( const auto tx = weakTx.lock() ) tx->failAsyncStage( Status::Aborted );
    }, PreparationLifetimeMs );
    if ( !operation ) return failure( request, "RESOURCE_LIMIT" );
    auto candidate = std::make_shared<Lifecycle>( Lifecycle::Action::Open, *window, path, kind );
    if ( transaction ) {
        const auto status = transaction->beginAsyncStage( [cancelled] { cancelled->set(); } );
        if ( status != Status::Open ) {
            operation->complete( {}, stageError( status ) );
            return failure( request, stageError( status ) );
        }
    }
    using Watcher = QFutureWatcher<std::shared_ptr<PreparedOpen>>;
    const auto finishOpen = [this, pending = std::move( candidate ), weakTx, explicitTx = bool( transaction ),
                             follow = followValue.toBool(), operation = *operation]( Watcher& watcher ) mutable {
        auto candidate = std::move( pending ); // The watcher does not retain the private graph after delivery.
        auto tx = weakTx.lock();
        const auto reject = [&]( Status reason, const QString& code ) {
            if ( tx ) tx->failAsyncStage( reason );
            operation.complete( {}, code );
        };
        try {
            const auto window = candidate->window;
            if ( !operation.current() || ( explicitTx && ( !tx || !tx->asyncStagePending() ) ) ) {
                reject( Status::Aborted, "OPERATION_CANCELLED" ); return;
            }
            if ( !window ) { reject( Status::TargetClosed, "TARGET_CLOSED" ); return; }
            if ( const auto existing = findPath( *window, candidate->kind, candidate->path ) ) {
                if ( explicitTx ) reject( Status::Conflict, "CONFLICT" );
                else operation.complete( outcome( window->runtimeWindowId(), existing->tabId, true ), {} );
                return;
            }
            const auto prepared = watcher.result();
            if ( !prepared->error.isEmpty() ) {
                reject( prepared->error == "RESOURCE_LIMIT" ? Status::ResourceLimit : Status::PreparationFailed, prepared->error );
                return;
            }
            auto& document = candidate->document;
            document = candidate->kind == DocumentKind::File
                ? window->prepareFile( prepared->file, follow ) : window->prepareFolder( candidate->path, prepared->members );
            if ( !document || ( candidate->kind == DocumentKind::File && !document->startLoading() ) ) {
                reject( Status::PreparationFailed, "SOURCE_PREPARATION_FAILED" ); return;
            }
            if ( !operation.current() || ( tx && !tx->asyncStagePending() ) ) { reject( Status::Aborted, "OPERATION_CANCELLED" ); return; }
            const auto valid = candidate->validate();
            if ( valid != Status::Open ) { reject( valid, stageError( valid ) ); return; }
            candidate->widget = dynamic_cast<QWidget*>( document->view() );
            candidate->source = dynamic_cast<LogData*>( document->source() );
            candidate->tabId = dynamic_cast<AbstractCrawlerWidget*>( document->view() )->runtimeTabId();
            const auto id = window->runtimeWindowId();
            if ( tx ) {
                candidate->privateDocumentPermit = prepared->permit;
                staged_.erase( std::remove_if( staged_.begin(), staged_.end(), []( const auto& entry ) {
                    const auto owner = entry.transaction.lock();
                    return !owner || ( owner->status() != Status::Open && owner->status() != Status::Preparing );
                } ), staged_.end() );
                staged_.push_back( { tx, window, candidate->tabId, { id, candidate->kind, candidate->path, candidate->widget } } );
            }
            stage( candidate, tx, operation, outcome( id, candidate->tabId, true, bool( tx ) ) );
        } catch ( const std::bad_alloc& ) { reject( Status::ResourceLimit, "RESOURCE_LIMIT" ); }
        catch ( ... ) { reject( Status::PreparationFailed, "SOURCE_PREPARATION_FAILED" ); }
    };
    Status launchFailure = Status::PreparationFailed;
    try {
        auto watcher = std::make_unique<Watcher>( this );
        connect( watcher.get(), &Watcher::finished, this, [raw = watcher.get(), finish = std::move( finishOpen )]() mutable {
            raw->deleteLater();
            finish( *raw );
        } );
        watcher->setFuture( acquire( path, kind, cancelled, std::move( permit ) ) );
        watcher.release(); // QObject parent owns completion delivery.
        return QJsonObject{ { "operation_id", operation->id }, { "status", "pending" } };
    } catch ( const std::bad_alloc& ) { launchFailure = Status::ResourceLimit; }
    catch ( ... ) {}
    cancelled->set();
    if ( transaction ) transaction->failAsyncStage( launchFailure );
    operation->complete( {}, stageError( launchFailure ) );
    return failure( request, stageError( launchFailure ) );
}
