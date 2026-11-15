#include "reviewcontroladapter.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <utility>

#include <QJsonArray>
#include <QThread>

#include "abstractcrawlerwidget.h"
#include "colorlabelscontroller.h"
#include "mainwindow.h"
#include "searchtoolbar.h"
#include "reviewtransaction.h"
#include "reviewcontentadapter.h"

namespace {
using klogg::control::RpcError;
using klogg::control::RpcRequest;

RpcError failure( const RpcRequest& request, int code, const QString& symbol,
                  const QString& message )
{
    return { request.id, code, message, QJsonObject{ { "code", symbol } }, true, false };
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

QJsonObject draftJson( const SearchToolbar::FilterDraftSnapshot& snapshot )
{
    const auto& filter = snapshot.filter;
    return { { "revision", QString::number( snapshot.revision ) },
             { "text", filter.text }, { "match_case", filter.matchCase },
             { "use_regexp", filter.useRegexp }, { "inverse", filter.inverse },
             { "boolean", filter.boolean }, { "auto_refresh", filter.autoRefresh },
             { "keep_results", filter.keepResults } };
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

struct Transaction {
    QString id;
    QString error;
    qint64 createdAt = 0;
    std::optional<qint64> retiredAt;
    ReviewTransaction edits;
    std::vector<Target> targets;
};

QString statusName( const Transaction& transaction )
{
    if ( transaction.error == QLatin1String( "TRANSACTION_EXPIRED" ) ) return "expired";
    if ( transaction.error == QLatin1String( "INVALID_PARAMS" ) ) return "invalid_argument";
    if ( transaction.error == QLatin1String( "TARGET_CLOSED" ) ) return "target_closed";
    if ( transaction.error == QLatin1String( "REVISION_CONFLICT" ) ) return "conflict";
    if ( transaction.error == QLatin1String( "RESOURCE_LIMIT" ) ) return "resource_limit";
    switch ( transaction.edits.status() ) {
    case Status::Open: return "open";
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
    return result;
}

DecodedResponse transactionResult( const RpcRequest& request, const Transaction& transaction )
{
    if ( transaction.error.isEmpty()
         && ( transaction.edits.status() == Status::Open
              || transaction.edits.status() == Status::Committed ) ) {
        return transactionJson( transaction );
    }
    auto code = transaction.error;
    if ( code.isEmpty() ) {
        switch ( transaction.edits.status() ) {
        case Status::Conflict: code = "REVISION_CONFLICT"; break;
        case Status::TargetClosed: code = "TARGET_CLOSED"; break;
        case Status::InvalidArgument: code = "INVALID_PARAMS"; break;
        case Status::ResourceLimit: code = "RESOURCE_LIMIT"; break;
        default: code = "TRANSACTION_ABORTED"; break;
        }
    }
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

struct ResolvedTab {
    QString windowId;
    MainWindow::TabInventoryEntry tab;
    AbstractCrawlerWidget::ReviewControls controls;
};

std::optional<ResolvedTab> resolveTab( const ReviewControlAdapter::WindowProvider& windows,
                                       const QString& id )
{
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
                        const ReviewControlAdapter::WindowProvider& windows )
{
    if ( transaction.edits.status() == Status::Open ) {
        for ( const auto& target : transaction.targets ) {
            const auto current = resolveTab( windows, target.tabId );
            if ( !current || current->tab.widget != target.widget
                 || ( target.labelsRead && current->controls.labels != target.labels )
                 || ( target.filterRead && current->controls.filter != target.filter ) ) {
                transaction.edits.abort();
                transaction.error = "TARGET_CLOSED";
                return transactionResult( request, transaction );
            }
        }
        transaction.edits.commit();
    }
    return transactionResult( request, transaction );
}
} // namespace

struct ReviewControlAdapter::State {
    // Terminal records allow a lost commit response to be queried/repeated
    // without replay. Only terminal records may be evicted to admit new work.
    explicit State( Clock clock ) : now( std::move( clock ) ), content( now ) {}
    Clock now;
    ReviewContentAdapter content;
    std::map<QString, std::shared_ptr<Transaction>> transactions;

    void expire()
    {
        const auto tick = now();
        for ( auto it = transactions.begin(); it != transactions.end(); ) {
            auto& transaction = *it->second;
            if ( transaction.edits.status() == Status::Open
                 && tick - transaction.createdAt >= TransactionLifetimeMs ) {
                transaction.edits.abort();
                transaction.error = "TRANSACTION_EXPIRED";
                transaction.targets.clear();
            }
            if ( transaction.edits.status() != Status::Open ) {
                if ( !transaction.retiredAt ) transaction.retiredAt = tick;
                if ( tick - *transaction.retiredAt >= TransactionLifetimeMs ) {
                    it = transactions.erase( it );
                    continue;
                }
            }
            ++it;
        }
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
    : QObject( parent ), state_( std::make_unique<State>( std::move( now ) ) ),
      windows_( std::move( windows ) )
{
}

void ReviewControlAdapter::reset()
{
    Q_ASSERT( QThread::currentThread() == thread() );
    for ( auto& item : state_->transactions ) item.second->edits.abort();
    state_->transactions.clear();
    state_->content.reset();
}

ReviewControlAdapter::~ReviewControlAdapter() = default;

DecodedResponse ReviewControlAdapter::dispatch( const RpcRequest& request )
{
    Q_ASSERT( QThread::currentThread() == thread() );
    state_->expire();
    const auto& params = request.params;
    const auto& method = request.method;
    std::shared_ptr<Transaction> transaction;
    const auto reject = [ & ]( const QString& code ) -> DecodedResponse {
        if ( transaction && transaction->edits.status() == Status::Open ) {
            transaction->error = code;
            transaction->edits.abort();
        }
        return failure( request, code == QLatin1String( "INVALID_PARAMS" ) ? -32602 : -32000,
                        code, QStringLiteral( "Control request rejected" ) );
    };

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

    if ( method == QLatin1String( "source.read" ) || method == QLatin1String( "source.release" )
         || method == QLatin1String( "operation.get" ) || method == QLatin1String( "operation.cancel" ) )
        return state_->content.dispatch( request );

    const bool captureSource = method == QLatin1String( "source.capture" );
    if ( method == QLatin1String( "pane.get" ) || captureSource ) {
        // Presentation observation is live, not a transactional source address.
        // Only an explicit capture acquires a source lease; ordinary metadata
        // never reads payload or rebuilds layout.
        if ( captureSource ? !onlyKeys( params, { "tab_id", "pane", "scope" } )
                           : !onlyKeys( params, { "tab_id", "pane" } ) )
            return reject( "INVALID_PARAMS" );
        const auto tabId = params.value( "tab_id" );
        const auto paneName = params.value( "pane" ).toString();
        if ( !tabId.isString() || tabId.toString().isEmpty()
             || ( paneName != QLatin1String( "main" ) && paneName != QLatin1String( "results" ) ) )
            return reject( "INVALID_PARAMS" );
        const auto tab = resolveTab( windows_, tabId.toString() );
        if ( !tab ) return reject( "TARGET_NOT_FOUND" );
        const auto* document = dynamic_cast<const AbstractCrawlerWidget*>( tab->tab.widget.data() );
        const auto paneId = paneName == QLatin1String( "main" )
            ? AbstractCrawlerWidget::Pane::Main : AbstractCrawlerWidget::Pane::Results;
        if ( captureSource ) {
            const auto scope = params.value( "scope" ).toString();
            // Raw main-pane providers have no match membership and intentionally
            // ignore SnapshotScope internally. Do not advertise all source rows
            // as match-only evidence through the public control protocol.
            if ( ( scope != QLatin1String( "presented" ) && scope != QLatin1String( "matches" ) )
                 || ( scope == QLatin1String( "matches" ) && paneId == AbstractCrawlerWidget::Pane::Main ) )
                return reject( "INVALID_PARAMS" );
            try {
                return state_->content.capture( request, document->createPaneSnapshot( paneId,
                    scope == QLatin1String( "presented" ) ? klogg::SnapshotScope::PresentedRows
                                                          : klogg::SnapshotScope::Matches ) );
            }
            catch ( ... ) { return reject( "SOURCE_CAPTURE_FAILED" ); }
        }
        const auto pane = document->paneMetadata( paneId );
        return QJsonObject{ { "tab_id", tabId }, { "pane", paneName },
            { "coordinates", "presented-model-zero-based" }, { "available", pane.available },
            { "row_count", QString::number( pane.sourceRowCount.get() ) },
            { "top_row", pane.topPresentedRow
                ? QJsonValue( QString::number( pane.topPresentedRow->get() ) ) : QJsonValue( QJsonValue::Null ) },
            { "follow", pane.follow }, { "wrap", pane.wrap },
            { "visible", pane.visible }, { "focused", pane.focused } };
    }

    if ( method == QLatin1String( "transaction.begin" ) ) {
        if ( !params.isEmpty() ) return reject( "INVALID_PARAMS" );
        if ( state_->transactions.size() >= MaxTransactions ) {
            const auto retired = std::find_if( state_->transactions.begin(), state_->transactions.end(),
                []( const auto& item ) { return item.second->edits.status() != Status::Open; } );
            if ( retired == state_->transactions.end() ) return reject( "RESOURCE_LIMIT" );
            state_->transactions.erase( retired );
        }
        transaction = std::make_shared<Transaction>();
        transaction->id = QUuid::createUuid().toString( QUuid::WithoutBraces );
        transaction->createdAt = state_->now();
        state_->transactions.emplace( transaction->id, transaction );
        return transactionJson( *transaction );
    }

    const bool get = method == QLatin1String( "transaction.get" );
    const bool commitRequest = method == QLatin1String( "transaction.commit" );
    const bool abort = method == QLatin1String( "transaction.abort" );
    const bool lifecycle = get || commitRequest || abort;
    const bool tabGet = method == QLatin1String( "tab.get" );
    const bool labelList = method == QLatin1String( "label.list" );
    const bool labelSet = method == QLatin1String( "label.set" );
    const bool labelRemove = method == QLatin1String( "label.remove" );
    const bool labelClear = method == QLatin1String( "label.clear" );
    const bool write = labelSet || labelRemove || labelClear;
    if ( !lifecycle && !tabGet && !labelList && !write ) {
        return failure( request, -32601, "METHOD_NOT_FOUND", "Unknown control method" );
    }
    const auto transactionId = params.value( "transaction_id" );
    if ( lifecycle || !transactionId.isUndefined() ) {
        if ( !transactionId.isString() || transactionId.toString().isEmpty() )
            return reject( "INVALID_PARAMS" );
        const auto found = state_->transactions.find( transactionId.toString() );
        if ( found == state_->transactions.end() ) return reject( "TRANSACTION_NOT_FOUND" );
        transaction = found->second;
    }
    if ( lifecycle ) {
        if ( !onlyKeys( params, { "transaction_id" } ) ) return reject( "INVALID_PARAMS" );
        if ( get ) return transactionJson( *transaction );
        if ( abort ) {
            if ( transaction->edits.status() == Status::Committed )
                return reject( "TRANSACTION_COMMITTED" );
            transaction->edits.abort();
            return transactionJson( *transaction );
        }
        return commit( request, *transaction, windows_ );
    }
    if ( transaction && transaction->edits.status() != Status::Open ) {
        if ( transaction->edits.status() == Status::Committed )
            return reject( "TRANSACTION_COMMITTED" );
        return transactionResult( request, *transaction );
    }
    const auto tabId = params.value( "tab_id" );
    if ( !tabId.isString() || tabId.toString().isEmpty() ) return reject( "INVALID_PARAMS" );
    const auto tab = resolveTab( windows_, tabId.toString() );
    if ( !tab ) return reject( "TARGET_NOT_FOUND" );
    const auto controls = tab->controls;

    if ( !write ) {
        if ( !onlyKeys( params, { "tab_id", "transaction_id" } ) ) return reject( "INVALID_PARAMS" );
        QJsonValue labels( QJsonValue::Null );
        QJsonValue filter( QJsonValue::Null );
        if ( controls.labels ) {
            const auto snapshot = transaction ? transaction->edits.labels( *controls.labels )
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
            const auto snapshot = transaction ? transaction->edits.filterDraft( *controls.filter )
                : std::optional<SearchToolbar::FilterDraftSnapshot>( controls.filter->filterDraftSnapshot() );
            if ( !snapshot ) return transactionResult( request, *transaction );
            filter = draftJson( *snapshot );
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
        const auto value = params.value( "expected_revision" );
        bool valid = false;
        const auto number = value.toString().toULongLong( &valid );
        if ( !value.isString() || !valid || QString::number( number ) != value.toString() )
            return reject( "INVALID_PARAMS" );
        expectedRevision = number;
    }
    const bool autocommit = !transaction;
    if ( autocommit ) transaction = std::make_shared<Transaction>();
    const auto base = transaction->edits.labels( *controls.labels );
    if ( !base ) return transactionResult( request, *transaction );
    if ( expectedRevision && *expectedRevision != base->revision ) return reject( "REVISION_CONFLICT" );
    captureTarget( *transaction, *tab, false );
    transaction->edits.stageLabels( *controls.labels, { edit } );
    if ( autocommit ) return commit( request, *transaction, windows_ );
    return transactionResult( request, *transaction );
}
