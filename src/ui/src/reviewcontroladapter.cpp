#include "reviewcontroladapter.h"
#include "controlschema.h"

#include <QTimer>

#include <algorithm>
#include <chrono>
#include <map>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QFutureWatcher>
#include "highlighterstatepublisher.h"
#include "reviewhighlightercodec.h"
#include <QThread>

#include "abstractcrawlerwidget.h"
#include "colorlabelscontroller.h"
#include "mainwindow.h"
#include "searchtoolbar.h"
#include "reviewtransaction.h"
#include "reviewcontentadapter.h"
#include "reviewdocumentadapter.h"
#include "reviewfilteradapter.h"
#include "reviewfiltercodec.h"
#include "crawlerwidget.h"
#include "controlframing.h"
#include "controlruntime.h"

namespace {
using klogg::control::RpcError;
using klogg::control::RpcRequest;

RpcError failure( const RpcRequest& request, int code, const QString& symbol,
                  const QString& message )
{
    return { request.id, code, message, QJsonObject{ { "code", symbol } }, true, false };
}

QJsonObject capabilitiesJson()
{
    return {
        { "protocol_version", klogg::control::ControlProtocolVersion },
        { "methods", QJsonArray{
            "capabilities.get", "schema.get", "state.get", "tab.get", "pane.list", "pane.get", "pane.navigate",
            "source.capture", "source.read", "source.release", "operation.get", "operation.cancel",
            "transaction.begin", "transaction.get", "transaction.commit", "transaction.abort",
            "label.list", "label.set", "label.remove", "label.clear",
            "mark.list", "mark.add", "mark.remove", "mark.replace", "mark.clear",
            "highlighter.list", "highlighter.replace", "documents.open", "documents.close", "filter.apply" } },
        { "features", QJsonObject{
            { "explicit_transaction_writes", QJsonArray{ "labels", "marks", "global_highlighters", "documents", "filters" } },
            { "explicit_transaction_filter_draft_reads", true },
            { "implicit_document_kinds", QJsonArray{ "file", "folder" } },
            { "explicit_lifecycle_transactions", true }, { "applied_filters", true },
            { "implicit_filter_kinds", QJsonArray{ "file", "live", "folder" } },
            { "filter_source_policy", "unchanged-snapshot" }, // File/live snapshot admission.
            { "folder_filter_source_policy", "retained-scan" },
            { "filter_kept_pane_creation", true }, { "explicit_filter_transactions", true },
            { "applied_filter_observation", true },
            { "method_schemas", true }, { "source_navigation", true }, { "live_lifecycle", false },
            { "event_subscription", false }, { "durable_transactions", false },
            { "content_requires_explicit_request", true } } },
        { "limits", QJsonObject{
            { "frame_bytes", static_cast<int>( klogg::control::MaxFrameBytes ) },
            { "transactions", static_cast<int>( ReviewControlAdapter::MaxTransactions ) },
            { "transaction_lifetime_ms", static_cast<int>( ReviewControlAdapter::TransactionLifetimeMs ) },
            { "transaction_owners", static_cast<int>( ReviewTransaction::MaxOwners ) },
            { "snapshots", static_cast<int>( ReviewContentAdapter::MaxSnapshots ) },
            { "operations", static_cast<int>( ReviewContentAdapter::MaxOperations ) },
            { "read_rows", static_cast<int>( ReviewContentAdapter::MaxRows ) },
            { "read_source_bytes", static_cast<int>( ReviewContentAdapter::MaxSourceBytes ) },
            { "read_text_units", static_cast<int>( ReviewContentAdapter::MaxTextUnits ) },
            { "read_output_bytes", static_cast<int>( ReviewContentAdapter::MaxOutputBytes ) },
            { "snapshot_lifetime_ms", static_cast<int>( ReviewContentAdapter::LifetimeMs ) },
            { "concurrent_document_preparations", ReviewDocumentAdapter::MaxConcurrentPreparations },
            { "document_preparation_lifetime_ms", static_cast<int>( ReviewDocumentAdapter::PreparationLifetimeMs ) },
            { "explicit_publication_participants", 1 },
            { "filter_pattern_units", klogg::review::MaxFilterPatternUnits },
            { "concurrent_filter_preparations", ReviewFilterAdapter::MaxConcurrentPreparations },
            { "filter_preparation_lifetime_ms", static_cast<int>( ReviewFilterAdapter::PreparationLifetimeMs ) } } }
    };
}

QString kindName( DocumentKind kind )
{
    switch ( kind ) {
    case DocumentKind::File: return QStringLiteral( "file" );
    case DocumentKind::Folder: return QStringLiteral( "folder" );
    case DocumentKind::AdbLogcat: return QStringLiteral( "live" );
    }
    return QStringLiteral( "unknown" );
}

QJsonObject tabMetadata( const MainWindow::TabInventoryEntry& tab, const QString& windowId )
{
    return { { "window_id", windowId }, { "tab_id", tab.tabId },
             { "kind", kindName( tab.kind ) }, { "document_id", tab.documentId },
             { "display_name", tab.displayName }, { "associated_path", tab.associatedPath },
             { "tab_title", tab.tabTitle } };
}

QJsonObject labelsJson( const ColorLabelsController::Snapshot& snapshot )
{
    QJsonArray slots;
    for ( size_t index = 0; index < snapshot.labels.size(); ++index ) {
        QJsonArray entries;
        for ( const auto& entry : snapshot.labels[ index ] ) {
            entries.append( QJsonObject{ { "text", entry.text },
                                         { "ignore_case", entry.ignoreCase },
                                         { "whole_word", entry.wholeWord } } );
        }
        slots.append( QJsonObject{ { "label", static_cast<int>( index + 1 ) },
                                   { "entries", entries } } );
    }
    return { { "revision", QString::number( snapshot.revision ) }, { "slots", slots } };
}

QJsonObject paneJson( const AbstractCrawlerWidget::PaneInventoryEntry& entry )
{
    const auto& pane = entry.metadata;
    QJsonObject result{ { "pane_id", entry.paneId.isEmpty() ? QJsonValue( QJsonValue::Null ) : QJsonValue( entry.paneId ) },
        { "pane", entry.kind == AbstractCrawlerWidget::Pane::Main ? "main" : "results" },
        { "title", entry.title }, { "current", entry.current },
        { "marks_owner_id", entry.marksOwnerId.isEmpty() ? QJsonValue( QJsonValue::Null ) : QJsonValue( entry.marksOwnerId ) },
        { "coordinates", "presented-model-zero-based" }, { "available", pane.available },
        { "row_count", QString::number( pane.sourceRowCount.get() ) },
        { "top_row", pane.topPresentedRow
            ? QJsonValue( QString::number( pane.topPresentedRow->get() ) ) : QJsonValue( QJsonValue::Null ) },
        { "selection_kind", QString::fromLatin1( pane.selectionKind ) },
        { "selected_row", pane.selectedRow ? QJsonValue( QString::number( pane.selectedRow->get() ) ) : QJsonValue( QJsonValue::Null ) },
        { "follow", pane.follow }, { "wrap", pane.wrap },
        { "visible", pane.visible }, { "focused", pane.focused } };
    if ( entry.appliedFilter ) {
        result.insert( "applied_filter", klogg::review::encodeAppliedFilter( *entry.appliedFilter ) );
    }
    return result;
}

QJsonObject marksJson( const klogg::ReviewMarksSnapshot& state, const QString& ownerId )
{
    QJsonArray groups;
    for ( const auto& group : state.groups ) {
        QJsonArray rows;
        for ( const auto row : group.rows ) rows.append( QString::number( row.get() ) );
        groups.append( QJsonObject{ { "member_path", group.memberPath.isEmpty()
            ? QJsonValue( QJsonValue::Null ) : QJsonValue( group.memberPath ) }, { "rows", rows } } );
    }
    return { { "owner_id", ownerId }, { "revision", QString::number( state.revision ) },
        { "scope", state.scope == klogg::ReviewMarksSnapshot::Scope::SourceModel ? "source-model" : "folder-tab" },
        { "coordinates", "stored-source-model-zero-based" }, { "groups", groups } };
}

bool fitsObservationResponse( const QJsonObject& result )
{
    // Leave transport-envelope headroom after JSON escaping.
    constexpr int MaxObservationResponseBytes = 512 * 1024;
    return QJsonDocument( result ).toJson( QJsonDocument::Compact ).size() <= MaxObservationResponseBytes;
}

const char* rowErrorCode( ReviewContentAdapter::RowResolutionError error )
{
    using Error = ReviewContentAdapter::RowResolutionError;
    switch ( error ) {
    case Error::SnapshotNotFound: return "SNAPSHOT_NOT_FOUND";
    case Error::InvalidArgument: return "INVALID_PARAMS";
    case Error::ResourceLimit: return "RESOURCE_LIMIT";
    case Error::SourceUnavailable: return "SOURCE_UNAVAILABLE";
    }
    return "SOURCE_UNAVAILABLE";
}

using Status = ReviewTransaction::Status;
using klogg::control::DecodedResponse;

struct Target {
    QString tabId;
    QPointer<QWidget> widget;
    QPointer<ColorLabelsController> labels;
    QPointer<SearchToolbar> filter;
    bool labelsRead = false;
    bool filterRead = false;
};

struct MarkTarget {
    QString tabId;
    QString ownerId;
    QPointer<QObject> owner;
};

struct Transaction {
    QString id;
    QString error;
    qint64 createdAt = 0;
    std::optional<qint64> retiredAt;
    std::shared_ptr<ReviewTransaction> edits = std::make_shared<ReviewTransaction>();
    // A visible publication can query other models synchronously through Qt.
    // Never enter it while an unrelated visible marks model is write-locked.
    bool hasVisibleParticipant = false;
    bool writesVisibleMarks = false;
    std::vector<Target> targets;
    std::vector<MarkTarget> markTargets;
    bool observingPreparation = false;
};

bool pending( Status status )
{
    return status == Status::Open || status == Status::Preparing;
}

QString transactionError( const Transaction& transaction )
{
    if ( !transaction.error.isEmpty() ) return transaction.error;
    switch ( transaction.edits->status() ) {
    case Status::Conflict: return "REVISION_CONFLICT";
    case Status::TargetClosed: return "TARGET_CLOSED";
    case Status::InvalidArgument: return "INVALID_PARAMS";
    case Status::ResourceLimit: return "RESOURCE_LIMIT";
    case Status::PreparationFailed: return "PREPARATION_FAILED";
    case Status::Aborted: return "TRANSACTION_ABORTED";
    default: return {};
    }
}

QString statusName( const Transaction& transaction )
{
    if ( transaction.error == QLatin1String( "TRANSACTION_EXPIRED" ) ) return "expired";
    if ( transaction.error == QLatin1String( "INVALID_PARAMS" ) ) return "invalid_argument";
    if ( transaction.error == QLatin1String( "TARGET_CLOSED" ) ) return "target_closed";
    if ( transaction.error == QLatin1String( "REVISION_CONFLICT" ) ) return "conflict";
    if ( transaction.error == QLatin1String( "RESOURCE_LIMIT" ) ) return "resource_limit";
    switch ( transaction.edits->status() ) {
    case Status::Open: return "open";
    case Status::Preparing: return "preparing";
    case Status::PreparationFailed: return "preparation_failed";
    case Status::Committed: return "committed";
    case Status::Aborted: return "aborted";
    case Status::Conflict: return "conflict";
    case Status::TargetClosed: return "target_closed";
    case Status::InvalidArgument: return "invalid_argument";
    case Status::ResourceLimit: return "resource_limit";
    }
    return "aborted";
}

QJsonObject transactionJson( const Transaction& transaction )
{
    QJsonObject result{ { "status", statusName( transaction ) } };
    if ( !transaction.id.isEmpty() ) result.insert( "transaction_id", transaction.id );
    const auto error = transactionError( transaction );
    if ( !error.isEmpty() ) result.insert( "error", QJsonObject{ { "code", error } } );
    return result;
}

DecodedResponse transactionResult( const RpcRequest& request, const Transaction& transaction )
{
    const auto code = transactionError( transaction );
    if ( code.isEmpty() ) return transactionJson( transaction );
    return failure( request, code == QLatin1String( "INVALID_PARAMS" ) ? -32602 : -32000,
                    code, QStringLiteral( "Review transaction did not commit" ) );
}

bool onlyKeys( const QJsonObject& params, std::initializer_list<const char*> allowed )
{
    for ( auto it = params.begin(); it != params.end(); ++it ) {
        if ( !std::any_of( allowed.begin(), allowed.end(), [ & ]( const char* name ) {
                 return it.key() == QLatin1String( name );
             } ) ) return false;
    }
    return true;
}

std::optional<uint64_t> canonicalUnsigned( const QJsonValue& value )
{
    bool valid = false;
    const auto number = value.toString().toULongLong( &valid );
    if ( !value.isString() || !valid || QString::number( number ) != value.toString() ) return {};
    return number;
}

struct ResolvedTab {
    QString windowId;
    MainWindow::TabInventoryEntry tab;
    AbstractCrawlerWidget::ReviewControls controls;
    bool privateDocument = false;
};

std::optional<ResolvedTab> resolveTab( const ReviewControlAdapter::WindowProvider& windows,
                                       const QString& id, ReviewDocumentAdapter& documents,
                                       const std::shared_ptr<ReviewTransaction>& edits = {} )
{
    if ( edits ) {
        if ( const auto staged = documents.stagedDocument( edits, id ) ) {
            if ( auto* crawler = dynamic_cast<AbstractCrawlerWidget*>( staged->widget.data() ) ) {
                return ResolvedTab{ staged->windowId,
                    { id, staged->kind, staged->documentId, {}, staged->documentId, {}, staged->widget },
                    crawler->reviewControls(), true };
            }
        }
    }
    for ( const auto& window : windows() ) {
        if ( !window ) continue;
        const auto snapshot = window->inventorySnapshot();
        for ( const auto& tab : snapshot.tabs ) {
            if ( tab.tabId != id || !tab.widget ) continue;
            auto* document = dynamic_cast<AbstractCrawlerWidget*>( tab.widget.data() );
            if ( document ) return ResolvedTab{ snapshot.windowId, tab, document->reviewControls() };
        }
    }
    return {};
}

klogg::ReviewMarksOwner* resolveMarkOwner( const ResolvedTab& tab, const QString& ownerId )
{
    auto* document = dynamic_cast<AbstractCrawlerWidget*>( tab.tab.widget.data() );
    if ( !document ) return nullptr;
    for ( auto* owner : document->reviewMarksOwners() ) {
        if ( owner && owner->reviewMarksId() == ownerId ) return owner;
    }
    return nullptr;
}

void captureTarget( Transaction& transaction, const ResolvedTab& tab, bool filter )
{
    auto found = std::find_if( transaction.targets.begin(), transaction.targets.end(),
        [ & ]( const Target& target ) { return target.tabId == tab.tab.tabId; } );
    if ( found == transaction.targets.end() ) {
        transaction.targets.push_back( { tab.tab.tabId, tab.tab.widget, {}, {}, false, false } );
        found = std::prev( transaction.targets.end() );
    }
    if ( !found->labelsRead ) {
        found->labelsRead = true;
        found->labels = tab.controls.labels;
    }
    if ( filter && !found->filterRead ) {
        found->filterRead = true;
        found->filter = tab.controls.filter;
    }
}

DecodedResponse commit( const RpcRequest& request, Transaction& transaction,
                        const ReviewControlAdapter::WindowProvider& windows, ReviewDocumentAdapter& documents )
{
    if ( pending( transaction.edits->status() ) ) {
        for ( const auto& target : transaction.targets ) {
            const auto current = resolveTab( windows, target.tabId, documents, transaction.edits );
            if ( !current || current->tab.widget != target.widget
                 || ( target.labelsRead && current->controls.labels != target.labels )
                 || ( target.filterRead && current->controls.filter != target.filter ) ) {
                transaction.edits->abort();
                transaction.error = "TARGET_CLOSED";
                return transactionResult( request, transaction );
            }
        }
        for ( const auto& target : transaction.markTargets ) {
            const auto tab = resolveTab( windows, target.tabId, documents, transaction.edits );
            auto* owner = tab ? resolveMarkOwner( *tab, target.ownerId ) : nullptr;
            if ( !owner || owner->reviewMarksObject() != target.owner ) {
                transaction.edits->abort();
                transaction.error = "TARGET_CLOSED";
                return transactionResult( request, transaction );
            }
        }
        transaction.edits->commit( [&] {
            if ( transaction.edits->committedHighlighters() ) HighlighterStatePublisher::instance().publish();
        } );
    }
    return transactionResult( request, transaction );
}
} // namespace

struct ReviewControlAdapter::State {
    // Terminal records allow a lost commit response to be queried/repeated
    // without replay. Only terminal records may be evicted to admit new work.
    State( Clock clock, WindowProvider windows )
        : now( std::move( clock ) ), content( now ), documents( std::move( windows ), content ),
          filters( content ) {}
    Clock now;
    QTimer expiryTimer;
    ReviewContentAdapter content;
    ReviewDocumentAdapter documents;
    ReviewFilterAdapter filters;
    std::map<QString, std::shared_ptr<Transaction>> transactions;

    std::shared_ptr<Transaction> begin()
    {
        if ( transactions.size() >= MaxTransactions ) {
            const auto retired = std::find_if( transactions.begin(), transactions.end(),
                []( const auto& item ) { return !pending( item.second->edits->status() ); } );
            if ( retired == transactions.end() ) return {};
            transactions.erase( retired );
        }
        auto transaction = std::make_shared<Transaction>();
        transaction->id = QUuid::createUuid().toString( QUuid::WithoutBraces );
        transaction->createdAt = now();
        transactions.emplace( transaction->id, transaction );
        if ( !expiryTimer.isActive() ) expiryTimer.start();
        return transaction;
    }

    void expire()
    {
        const auto tick = now();
        for ( auto it = transactions.begin(); it != transactions.end(); ) {
            auto& transaction = *it->second;
            if ( pending( transaction.edits->status() )
                 && tick - transaction.createdAt >= TransactionLifetimeMs ) {
                transaction.edits->abort();
                transaction.error = "TRANSACTION_EXPIRED";
                transaction.targets.clear();
                transaction.markTargets.clear();
            }
            if ( !pending( transaction.edits->status() ) ) {
                if ( !transaction.retiredAt ) {
                    transaction.retiredAt = tick;
                    content.discardScope( transaction.id );
                }
                if ( tick - *transaction.retiredAt >= TransactionLifetimeMs ) {
                    it = transactions.erase( it );
                    continue;
                }
            }
            ++it;
        }
        if ( transactions.empty() ) expiryTimer.stop();
    }
};

ReviewControlAdapter::ReviewControlAdapter( WindowProvider windows, QObject* parent )
    : ReviewControlAdapter( std::move( windows ), [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch() ).count();
    }, parent )
{
}

ReviewControlAdapter::ReviewControlAdapter( WindowProvider windows, Clock now, QObject* parent )
    : QObject( parent ), state_( std::make_unique<State>( std::move( now ), windows ) ),
      windows_( std::move( windows ) )
{
    // A disconnected client must not retain a private document graph indefinitely.
    state_->expiryTimer.setInterval( 1000 );
    connect( &state_->expiryTimer, &QTimer::timeout, this, [this] { state_->expire(); } );
}

void ReviewControlAdapter::reset()
{
    Q_ASSERT( QThread::currentThread() == thread() );
    state_->expiryTimer.stop();
    for ( auto& item : state_->transactions ) item.second->edits->abort();
    state_->transactions.clear();
    state_->content.reset();
    state_->documents.reset();
    state_->filters.reset();
}

ReviewControlAdapter::~ReviewControlAdapter() { reset(); }

DecodedResponse ReviewControlAdapter::dispatch( const RpcRequest& request )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    state_->expire();
    const auto& params = request.params;
    const auto& method = request.method;
    std::shared_ptr<Transaction> transaction;
    const auto reject = [ & ]( const QString& code ) -> DecodedResponse {
        if ( transaction && transaction->edits->status() == Status::Open ) {
            transaction->error = code;
            transaction->edits->abort();
        }
        const auto number = code == QLatin1String( "INVALID_PARAMS" ) ? -32602
            : code == QLatin1String( "METHOD_NOT_FOUND" ) ? -32601 : -32000;
        return failure( request, number, code, QStringLiteral( "Control request rejected" ) );
    };
    // Bind explicit transaction context before every route, including malformed
    // observations and unknown methods. A rejected suffix cannot leave staged
    // writes silently committable just because dispatch returned early.
    const auto transactionId = params.value( "transaction_id" );
    if ( !transactionId.isUndefined() ) {
        if ( !transactionId.isString() || transactionId.toString().isEmpty() ) return reject( "INVALID_PARAMS" );
        const auto found = state_->transactions.find( transactionId.toString() );
        if ( found == state_->transactions.end() ) return reject( "TRANSACTION_NOT_FOUND" );
        transaction = found->second;
    }

    const auto edits = transaction ? transaction->edits : nullptr;
    const bool txControl = method == "transaction.get" || method == "transaction.abort" || method == "transaction.commit";
    if ( edits && edits->asyncStagePending() && method != "transaction.get" && method != "transaction.abort" )
        return failure( request, -32000, "TRANSACTION_BUSY", "Wait for the staged operation before continuing" );
    if ( edits && !txControl && edits->status() != Status::Open ) {
        if ( edits->status() == Status::Preparing ) return reject( "TRANSACTION_PREPARING" );
        if ( edits->status() == Status::Committed ) return reject( "TRANSACTION_COMMITTED" );
        return transactionResult( request, *transaction );
    }
    const auto innerRequest = [&] {
        auto inner = request;
        inner.params.remove( "transaction_id" );
        return inner;
    };
    const auto stagedResponse = [&]( DecodedResponse result ) {
        if ( const auto* error = std::get_if<RpcError>( &result ); edits && error && edits->status() == Status::Open ) {
            transaction->error = error->data.toObject().value( "code" ).toString();
            edits->abort();
        }
        return result;
    };
    if ( method == QLatin1String( "schema.get" ) ) {
        if ( transaction ) return reject( "INVALID_PARAMS" );
        return klogg::control::methodSchema( request );
    }
    if ( method == QLatin1String( "capabilities.get" ) ) {
        if ( !params.isEmpty() ) return reject( "INVALID_PARAMS" );
        return capabilitiesJson();
    }

    if ( method == QLatin1String( "state.get" ) ) {
        if ( !params.isEmpty() ) return reject( "INVALID_PARAMS" );
        QJsonArray windows;
        for ( const auto& window : windows_() ) {
            if ( !window ) continue;
            const auto snapshot = window->inventorySnapshot();
            QJsonArray tabs;
            for ( const auto& tab : snapshot.tabs ) {
                if ( tab.widget ) tabs.append( tabMetadata( tab, snapshot.windowId ) );
            }
            windows.append( QJsonObject{ { "window_id", snapshot.windowId },
                                         { "revision", QString::number( snapshot.revision ) },
                                         { "current_tab_id", snapshot.currentTabId },
                                         { "tabs", tabs } } );
        }
        return QJsonObject{ { "scope", "window-tab-inventory" }, { "windows", windows } };
    }

    if ( method == QLatin1String( "documents.open" ) || method == QLatin1String( "documents.close" ) ) {
        if ( transaction && transaction->writesVisibleMarks ) return reject( "INVALID_PARAMS" );
        if ( transaction ) transaction->hasVisibleParticipant = true;
        return stagedResponse( state_->documents.dispatch( innerRequest(), edits ) );
    }

    if ( method == QLatin1String( "filter.apply" ) ) {
        if ( !onlyKeys( params, { "tab_id", "filter", "expected_revision", "transaction_id" } ) )
            return reject( "INVALID_PARAMS" );
        const auto tabId = params.value( "tab_id" );
        if ( !tabId.isString() || tabId.toString().isEmpty() ) return reject( "INVALID_PARAMS" );
        const auto tab = resolveTab( windows_, tabId.toString(), state_->documents, edits );
        if ( !tab ) return reject( "TARGET_NOT_FOUND" );
        const auto widget = tab->tab.widget;
        if ( !widget || !tab->controls.filter ) return reject( "UNSUPPORTED_SOURCE" );
        const auto draft = tab->controls.filter->filterDraftSnapshot();
        auto decoded = klogg::review::decodeFilterPatch( params.value( "filter" ), draft.filter );
        if ( const auto* error = std::get_if<klogg::review::FilterDecodeError>( &decoded ) )
            return reject( *error == klogg::review::FilterDecodeError::ResourceLimit ? "RESOURCE_LIMIT" : "INVALID_PARAMS" );
        if ( params.contains( "expected_revision" ) ) {
            const auto expected = canonicalUnsigned( params.value( "expected_revision" ) );
            if ( !expected ) return reject( "INVALID_PARAMS" );
            if ( *expected != draft.revision ) return reject( "REVISION_CONFLICT" );
        }
        if ( transaction && !tab->privateDocument ) {
            if ( transaction->writesVisibleMarks ) return reject( "INVALID_PARAMS" );
            transaction->hasVisibleParticipant = true;
        }
        ReviewFilterAdapter::Target target{ tab->windowId, tab->tab.tabId, widget,
            [windows = windows_, id = tab->tab.tabId, windowId = tab->windowId, widget,
             owner = QPointer<ReviewControlAdapter>( this ), weak = std::weak_ptr<ReviewTransaction>( edits )] {
                if ( !owner ) return false;
                const auto current = resolveTab( windows, id, owner->state_->documents, weak.lock() );
                return widget && current && current->windowId == windowId
                    && current->tab.widget.data() == widget.data();
            } };
        return stagedResponse( state_->filters.apply( innerRequest(), std::move( target ),
            std::get<SearchToolbar::FilterSpec>( decoded ), edits, tab->privateDocument ) );
    }

    if ( method == QLatin1String( "source.read" ) || method == QLatin1String( "source.release" )
         || method == QLatin1String( "operation.get" ) || method == QLatin1String( "operation.cancel" ) ) {
        return stagedResponse( state_->content.dispatch( innerRequest(), transaction ? transaction->id : QString{} ) );
    }

    if ( method == QLatin1String( "pane.navigate" ) ) {
        if ( transaction || !onlyKeys( params, { "tab_id", "pane_id", "snapshot_id", "row" } ) ) return reject( "INVALID_PARAMS" );
        const auto tabId = params.value( "tab_id" ).toString();
        const auto paneId = params.value( "pane_id" ).toString();
        const auto snapshotId = params.value( "snapshot_id" ).toString();
        const auto ordinal = canonicalUnsigned( params.value( "row" ) );
        if ( tabId.isEmpty() || paneId.isEmpty() || snapshotId.isEmpty() || !ordinal ) return reject( "INVALID_PARAMS" );
        const auto tab = resolveTab( windows_, tabId, state_->documents );
        if ( !tab ) return reject( "TARGET_NOT_FOUND" );
        const auto resolved = state_->content.resolveRows( snapshotId, { LineNumber{ *ordinal } } );
        if ( const auto* error = std::get_if<ReviewContentAdapter::RowResolutionError>( &resolved ) )
            return reject( rowErrorCode( *error ) );
        auto* doc = dynamic_cast<AbstractCrawlerWidget*>( tab->tab.widget.data() );
        const auto result = doc->navigateReviewPane( paneId, std::get<ReviewContentAdapter::RowMetadata>( resolved ).front().identity );
        if ( const auto* row = std::get_if<LineNumber>( &result ) )
            return QJsonObject{ { "tab_id", tabId }, { "pane_id", paneId }, { "presented_row", QString::number( row->get() ) } };
        const auto error = std::get<AbstractCrawlerWidget::NavigationError>( result );
        if ( error == AbstractCrawlerWidget::NavigationError::PaneNotFound ) return reject( "PANE_NOT_FOUND" );
        return reject( error == AbstractCrawlerWidget::NavigationError::RowNotPresent ? "ROW_NOT_PRESENT" : "TARGET_NOT_READY" );
    }
    const bool captureSource = method == QLatin1String( "source.capture" );
    const bool listPanes = method == QLatin1String( "pane.list" );
    if ( method == QLatin1String( "pane.get" ) || captureSource || listPanes ) {
        // The aliases remain convenient live observations. An explicit pane ID
        // resolves actual membership and never follows a human page switch.
        const bool keysValid = listPanes ? onlyKeys( params, { "tab_id", "transaction_id" } )
            : captureSource ? onlyKeys( params, { "tab_id", "pane", "pane_id", "scope", "transaction_id" } )
                            : onlyKeys( params, { "tab_id", "pane", "pane_id", "transaction_id" } );
        if ( !keysValid ) return reject( "INVALID_PARAMS" );
        const auto tabId = params.value( "tab_id" );
        if ( !tabId.isString() || tabId.toString().isEmpty() ) return reject( "INVALID_PARAMS" );
        const auto tab = resolveTab( windows_, tabId.toString(), state_->documents, edits );
        if ( !tab ) return reject( "TARGET_NOT_FOUND" );
        const auto* document = dynamic_cast<const AbstractCrawlerWidget*>( tab->tab.widget.data() );
        const auto panes = document->paneInventory();
        if ( listPanes ) {
            constexpr size_t MaxObservedPanes = 128;
            if ( panes.size() > MaxObservedPanes ) return reject( "RESOURCE_LIMIT" );
            QJsonArray items;
            for ( const auto& pane : panes ) items.append( paneJson( pane ) );
            const QJsonObject result{ { "tab_id", tabId }, { "panes", items } };
            if ( !fitsObservationResponse( result ) ) return reject( "RESOURCE_LIMIT" );
            return result;
        }
        if ( params.contains( "pane" ) == params.contains( "pane_id" ) ) return reject( "INVALID_PARAMS" );
        AbstractCrawlerWidget::PaneInventoryEntry selected;
        if ( params.contains( "pane_id" ) ) {
            const auto id = params.value( "pane_id" );
            if ( !id.isString() || id.toString().isEmpty() ) return reject( "INVALID_PARAMS" );
            const auto found = std::find_if( panes.begin(), panes.end(), [&]( const auto& pane ) {
                return pane.paneId == id.toString();
            } );
            if ( found == panes.end() ) return reject( "PANE_NOT_FOUND" );
            selected = *found;
        }
        else {
            const auto name = params.value( "pane" ).toString();
            if ( name != QLatin1String( "main" ) && name != QLatin1String( "results" ) ) return reject( "INVALID_PARAMS" );
            selected.kind = name == QLatin1String( "main" )
                ? AbstractCrawlerWidget::Pane::Main : AbstractCrawlerWidget::Pane::Results;
            const auto found = std::find_if( panes.begin(), panes.end(), [&]( const auto& pane ) {
                return pane.kind == selected.kind && pane.current;
            } );
            if ( found != panes.end() ) selected = *found;
        }
        if ( captureSource ) {
            const auto scope = params.value( "scope" ).toString();
            // Raw sources have no match membership; do not label all rows as matches.
            if ( ( scope != QLatin1String( "presented" ) && scope != QLatin1String( "matches" ) )
                 || ( scope == QLatin1String( "matches" ) && selected.kind == AbstractCrawlerWidget::Pane::Main ) )
                return reject( "INVALID_PARAMS" );
            try {
                return stagedResponse( state_->content.capture( request, document->createPaneSnapshot( selected.paneId,
                    scope == QLatin1String( "presented" ) ? klogg::SnapshotScope::PresentedRows
                                                          : klogg::SnapshotScope::Matches ), transaction ? transaction->id : QString{} ) );
            }
            catch ( ... ) { return reject( "SOURCE_CAPTURE_FAILED" ); }
        }
        auto result = paneJson( selected );
        result.insert( "tab_id", tabId );
        if ( !fitsObservationResponse( result ) ) return reject( "RESOURCE_LIMIT" );
        return result;
    }

    if ( method == QLatin1String( "transaction.begin" ) ) {
        if ( !params.isEmpty() ) return reject( "INVALID_PARAMS" );
        transaction = state_->begin();
        if ( !transaction ) return reject( "RESOURCE_LIMIT" );
        return transactionJson( *transaction );
    }

    const auto finishCommit = [&]( const std::shared_ptr<Transaction>& candidate ) {
        // Publication observers may destroy this adapter. Do not dereference it
        // after commit unless the weak lifetime guard still permits doing so.
        QPointer<ReviewControlAdapter> self( this );
        const auto windows = windows_;
        auto response = commit( request, *candidate, windows, state_->documents );
        if ( self && candidate->edits->status() == Status::Preparing && !candidate->observingPreparation ) {
            const auto future = candidate->edits->preparationFuture();
            // A reentrant observer may see the non-yielding publication phase;
            // only worker preparation has a future that needs a watcher.
            if ( !future ) return response;
            auto* watcher = new QFutureWatcher<ReviewTransaction::Preparation>( self );
            QPointer<QObject> watcherGuard( watcher );
            candidate->observingPreparation = true;
            // Retain no request payload while compiling. The candidate owns its
            // admitted semantic state, independently of transport/socket lifetime.
            const RpcRequest completionRequest{ request.id, "transaction.commit", {} };
            connect( watcher, &QFutureWatcherBase::finished, self,
                [self, candidate, watcherGuard, completionRequest] {
                    if ( !self ) return;
                    self->state_->expire();
                    const auto currentWindows = self->windows_;
                    if ( candidate->edits->status() == Status::Preparing )
                        commit( completionRequest, *candidate, currentWindows, self->state_->documents );
                    if ( self ) Q_EMIT self->preparationFinished( candidate->id );
                    if ( watcherGuard ) watcherGuard->deleteLater();
                } );
            watcher->setFuture( *future );
        }
        return response;
    };

    const bool get = method == QLatin1String( "transaction.get" );
    const bool commitRequest = method == QLatin1String( "transaction.commit" );
    const bool abort = method == QLatin1String( "transaction.abort" );
    const bool lifecycle = get || commitRequest || abort;
    const bool tabGet = method == QLatin1String( "tab.get" );
    const bool labelList = method == QLatin1String( "label.list" );
    const bool markList = method == QLatin1String( "mark.list" );
    const bool markAdd = method == QLatin1String( "mark.add" );
    const bool markRemove = method == QLatin1String( "mark.remove" );
    const bool markReplace = method == QLatin1String( "mark.replace" );
    const bool markClear = method == QLatin1String( "mark.clear" );
    const bool markWrite = markAdd || markRemove || markReplace || markClear;
    const bool labelSet = method == QLatin1String( "label.set" );
    const bool labelRemove = method == QLatin1String( "label.remove" );
    const bool labelClear = method == QLatin1String( "label.clear" );
    const bool write = labelSet || labelRemove || labelClear;
    const bool highlighterList = method == QLatin1String( "highlighter.list" );
    const bool highlighterReplace = method == QLatin1String( "highlighter.replace" );
    if ( !lifecycle && !tabGet && !labelList && !markList && !markWrite && !write && !highlighterList && !highlighterReplace )
        return reject( "METHOD_NOT_FOUND" );
    if ( lifecycle && !transaction ) return reject( "INVALID_PARAMS" );
    if ( lifecycle ) {
        if ( !onlyKeys( params, { "transaction_id" } ) ) return reject( "INVALID_PARAMS" );
        if ( get ) return transactionJson( *transaction );
        if ( abort ) {
            if ( transaction->edits->status() == Status::Committed )
                return reject( "TRANSACTION_COMMITTED" );
            transaction->edits->abort();
            return transactionJson( *transaction );
        }
        return finishCommit( transaction );
    }
    if ( highlighterList || highlighterReplace ) {
        if ( highlighterList ? !onlyKeys( params, { "transaction_id" } )
            : !onlyKeys( params, { "transaction_id", "expected_revision", "sets", "active_set_ids" } ) )
            return reject( "INVALID_PARAMS" );
        if ( highlighterList ) {
            const auto state = transaction ? transaction->edits->highlighters()
                : HighlighterSetCollection::get().boundedSnapshot();
            if ( !state ) return reject( "RESOURCE_LIMIT" );
            const auto result = klogg::review::encodeHighlighters( *state );
            if ( !fitsObservationResponse( result ) ) return reject( "RESOURCE_LIMIT" );
            return result;
        }
        auto decoded = klogg::review::decodeHighlighters( params.value( "sets" ), params.value( "active_set_ids" ) );
        if ( const auto* error = std::get_if<klogg::review::HighlighterDecodeError>( &decoded ) )
            return reject( *error == klogg::review::HighlighterDecodeError::ResourceLimit ? "RESOURCE_LIMIT" : "INVALID_PARAMS" );
        std::optional<uint64_t> expected;
        if ( params.contains( "expected_revision" ) ) {
            expected = canonicalUnsigned( params.value( "expected_revision" ) );
            if ( !expected ) return reject( "INVALID_PARAMS" );
        }
        const bool autocommit = !transaction;
        if ( autocommit ) transaction = state_->begin();
        if ( !transaction ) return reject( "RESOURCE_LIMIT" );
        const auto base = transaction->edits->highlighters();
        if ( !base ) return transactionResult( request, *transaction );
        if ( expected && *expected != base->revision ) return reject( "REVISION_CONFLICT" );
        auto& change = std::get<klogg::review::HighlighterReplacement>( decoded );
        transaction->edits->stageHighlighters( std::move( change.sets ), std::move( change.activeSetIds ) );
        if ( autocommit ) return finishCommit( transaction );
        return transactionResult( request, *transaction );
    }
    const auto tabId = params.value( "tab_id" );
    if ( !tabId.isString() || tabId.toString().isEmpty() ) return reject( "INVALID_PARAMS" );
    const auto tab = resolveTab( windows_, tabId.toString(), state_->documents, edits );
    if ( !tab ) return reject( "TARGET_NOT_FOUND" );
    const auto controls = tab->controls;

    if ( markList || markWrite ) {
        if ( markWrite && transaction && !tab->privateDocument ) {
            if ( transaction->hasVisibleParticipant ) return reject( "INVALID_PARAMS" );
            transaction->writesVisibleMarks = true;
        }
        const bool validKeys = markList ? onlyKeys( params, { "tab_id", "owner_id", "transaction_id" } )
            : markClear ? onlyKeys( params, { "tab_id", "owner_id", "transaction_id", "expected_revision" } )
            : onlyKeys( params, { "tab_id", "owner_id", "transaction_id", "expected_revision", "snapshot_id", "rows" } );
        if ( !validKeys ) return reject( "INVALID_PARAMS" );
        const auto ownerId = params.value( "owner_id" );
        if ( !ownerId.isString() || ownerId.toString().isEmpty() ) return reject( "INVALID_PARAMS" );
        auto* owner = resolveMarkOwner( *tab, ownerId.toString() );
        if ( !owner ) return reject( "MARK_OWNER_NOT_FOUND" );
        std::optional<uint64_t> expected;
        if ( params.contains( "expected_revision" ) ) {
            expected = canonicalUnsigned( params.value( "expected_revision" ) );
            if ( !expected ) return reject( "INVALID_PARAMS" );
        }
        ReviewTransaction::MarkEdit edit;
        if ( markWrite ) {
            using Kind = ReviewTransaction::MarkEdit::Kind;
            edit.kind = markAdd ? Kind::Add : markRemove ? Kind::Remove : markReplace ? Kind::Replace : Kind::Clear;
            if ( !markClear ) {
                const auto snapshotId = params.value( "snapshot_id" );
                const auto rows = params.value( "rows" );
                if ( !snapshotId.isString() || snapshotId.toString().isEmpty() || !rows.isArray() )
                    return reject( "INVALID_PARAMS" );
                const auto values = rows.toArray();
                if ( static_cast<uint64_t>( values.size() ) > ReviewContentAdapter::MaxRows ) return reject( "RESOURCE_LIMIT" );
                if ( values.isEmpty() ) return reject( "INVALID_PARAMS" );
                std::vector<LineNumber> ordinals;
                ordinals.reserve( static_cast<size_t>( values.size() ) );
                for ( const auto& value : values ) {
                    const auto row = canonicalUnsigned( value );
                    if ( !row ) return reject( "INVALID_PARAMS" );
                    ordinals.push_back( LineNumber{ *row } );
                }
                const auto resolved = state_->content.resolveRows( snapshotId.toString(), ordinals, transaction ? transaction->id : QString{} );
                if ( const auto* error = std::get_if<ReviewContentAdapter::RowResolutionError>( &resolved ) ) {
                    return reject( rowErrorCode( *error ) );
                }
                for ( const auto& row : std::get<ReviewContentAdapter::RowMetadata>( resolved ) ) edit.rows.push_back( row.identity );
            }
        }
        const bool autocommit = markWrite && !transaction;
        if ( autocommit ) transaction = state_->begin();
        if ( autocommit && !transaction ) return reject( "RESOURCE_LIMIT" );
        const auto state = transaction ? transaction->edits->marks( *owner ) : owner->reviewMarksSnapshot();
        if ( !state ) return transaction ? transactionResult( request, *transaction ) : reject( "RESOURCE_LIMIT" );
        if ( transaction ) {
            const auto& targets = transaction->markTargets;
            const auto found = std::find_if( targets.begin(), targets.end(), [&]( const auto& target ) {
                return target.tabId == tabId.toString() && target.ownerId == ownerId.toString();
            } );
            if ( found == targets.end() ) transaction->markTargets.push_back(
                { tabId.toString(), ownerId.toString(), owner->reviewMarksObject() } );
        }
        if ( markWrite ) {
            if ( expected && *expected != state->revision ) return reject( "REVISION_CONFLICT" );
            transaction->edits->stageMarks( *owner, edit, tab->privateDocument );
            if ( autocommit ) return finishCommit( transaction );
            return transactionResult( request, *transaction );
        }
        auto result = marksJson( *state, ownerId.toString() );
        result.insert( "tab_id", tabId );
        if ( !fitsObservationResponse( result ) ) return reject( "RESOURCE_LIMIT" );
        return result;
    }

    if ( !write ) {
        if ( !onlyKeys( params, { "tab_id", "transaction_id" } ) ) return reject( "INVALID_PARAMS" );
        QJsonValue labels( QJsonValue::Null );
        QJsonValue filter( QJsonValue::Null );
        if ( controls.labels ) {
            const auto snapshot = transaction ? transaction->edits->labels( *controls.labels )
                : std::optional<ColorLabelsController::Snapshot>( controls.labels->snapshot() );
            if ( !snapshot ) return transactionResult( request, *transaction );
            labels = labelsJson( *snapshot );
        }
        if ( labelList ) {
            if ( !controls.labels ) return reject( "TARGET_NOT_READY" );
            if ( transaction ) captureTarget( *transaction, *tab, false );
            return labels;
        }
        if ( controls.filter ) {
            const auto snapshot = transaction && !tab->privateDocument ? transaction->edits->filterDraft( *controls.filter )
                : std::optional<SearchToolbar::FilterDraftSnapshot>( controls.filter->filterDraftSnapshot() );
            if ( !snapshot ) return transactionResult( request, *transaction );
            filter = klogg::review::encodeFilterDraft( *snapshot );
        }
        if ( transaction ) captureTarget( *transaction, *tab, true );
        auto value = tabMetadata( tab->tab, tab->windowId );
        value.insert( "labels", labels );
        value.insert( "filter_draft", filter );
        return value;
    }

    const bool validKeys = labelSet
        ? onlyKeys( params, { "tab_id", "transaction_id", "expected_revision", "label", "texts",
                             "ignore_case", "whole_word" } )
        : labelRemove ? onlyKeys( params, { "tab_id", "transaction_id", "expected_revision", "texts" } )
                      : onlyKeys( params, { "tab_id", "transaction_id", "expected_revision" } );
    if ( !validKeys ) return reject( "INVALID_PARAMS" );
    if ( !controls.labels ) return reject( "TARGET_NOT_READY" );
    ColorLabelsController::Edit edit;
    edit.kind = labelSet ? ColorLabelsController::Edit::Kind::Set
              : labelRemove ? ColorLabelsController::Edit::Kind::Remove
                            : ColorLabelsController::Edit::Kind::Clear;
    if ( labelSet ) {
        const auto slot = params.value( "label" );
        const int number = slot.toInt( -1 );
        if ( !slot.isDouble() || number < 1 || slot.toDouble() != static_cast<double>( number ) )
            return reject( "INVALID_PARAMS" );
        edit.label = static_cast<size_t>( number - 1 );
        for ( const auto* name : { "ignore_case", "whole_word" } ) {
            if ( params.contains( name ) && !params.value( name ).isBool() ) return reject( "INVALID_PARAMS" );
        }
        edit.defaults.ignoreCase = params.value( "ignore_case" ).toBool( true );
        edit.defaults.wholeWord = params.value( "whole_word" ).toBool( false );
    }
    if ( !labelClear ) {
        if ( !params.value( "texts" ).isArray() ) return reject( "INVALID_PARAMS" );
        const auto texts = params.value( "texts" ).toArray();
        if ( static_cast<size_t>( texts.size() ) > ColorLabelsManager::MaxBulkLabelTexts )
            return reject( "RESOURCE_LIMIT" );
        for ( const auto& text : texts ) {
            if ( !text.isString() ) return reject( "INVALID_PARAMS" );
            edit.texts.append( text.toString() );
        }
    }
    std::optional<uint64_t> expectedRevision;
    if ( params.contains( "expected_revision" ) ) {
        expectedRevision = canonicalUnsigned( params.value( "expected_revision" ) );
        if ( !expectedRevision ) return reject( "INVALID_PARAMS" );
    }
    const bool autocommit = !transaction;
    if ( autocommit ) transaction = std::make_shared<Transaction>();
    const auto base = transaction->edits->labels( *controls.labels );
    if ( !base ) return transactionResult( request, *transaction );
    if ( expectedRevision && *expectedRevision != base->revision ) return reject( "REVISION_CONFLICT" );
    captureTarget( *transaction, *tab, false );
    transaction->edits->stageLabels( *controls.labels, { edit } );
    if ( autocommit ) return commit( request, *transaction, windows_, state_->documents );
    return transactionResult( request, *transaction );
}
