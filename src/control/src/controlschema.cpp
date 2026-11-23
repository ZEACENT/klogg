#include "controlschema.h"

#include <QJsonArray>
#include <QStringList>

namespace klogg::control {
namespace {
using Object = QJsonObject;
using Array = QJsonArray;

// Build ordinary, self-contained JSON Schemas. Reuse the small shapes here,
// rather than sending a registry of unrelated definitions with every reply.
Object with( Object base, const Object& extra )
{
    for ( auto it = extra.begin(); it != extra.end(); ++it ) base.insert( it.key(), it.value() );
    return base;
}
Object object( const Object& required = {}, const Object& optional = {} )
{
    return { { "type", "object" }, { "properties", with( required, optional ) },
        { "required", Array::fromStringList( required.keys() ) }, { "additionalProperties", false } };
}
Object array( const Object& items ) { return { { "type", "array" }, { "items", items } }; }
Object choices( const Array& values ) { return { { "type", "string" }, { "enum", values } }; }
Object either( const Array& schemas ) { return { { "oneOf", schemas } }; }
Object note( Object schema, const char* description )
{
    schema.insert( "description", description );
    return schema;
}
Object nullable( const Object& value ) { return either( { value, Object{ { "type", "null" } } } ); }

Object buildCatalog()
{
    Object methods;
    const auto add = [&]( const char* method, const char* description, const Object& params, const Object& result ) {
        methods.insert( method, Object{ { "method", method }, { "description", description },
                                        { "params", params }, { "result", result } } );
    };
    const Object text{ { "type", "string" } };
    const Object identifier= with( text, { { "minLength", 1 } } );
    const Object decimal= note( with( text, { { "pattern", "^(0|[1-9][0-9]{0,19})$" }, { "maxLength", 20 } } ),
                              "Canonical decimal uint64 string; maximum 18446744073709551615." );
    const Object boolean{ { "type", "boolean" } };
    const Object yes{ { "const", true } };
    const Object integer{ { "type", "integer" } };
    const Object strings = array( text );
    const Object error = object( { { "code", text } } );
    const Object tab{ { "tab_id", identifier} };
    const Object transactionId{ { "transaction_id", identifier} };
    const Object write = with( transactionId, { { "expected_revision", decimal} } );
    const Object windowTab = with( tab, { { "window_id", identifier} } );
    const Object operationId{ { "operation_id", identifier} };
    const Object snap{ { "snapshot_id", identifier} };
    const Object owner = with( tab, { { "owner_id", identifier} } );
    const Object panes{ { "pane_id", identifier}, { "pane", choices( { "main", "results" } ) } };
    const Object paneSelector{ { "oneOf", Array{ Object{ { "required", Array{ "pane_id" } } },
                                                  Object{ { "required", Array{ "pane" } } } } } };
    const Object pending = object( with( operationId, { { "status", choices( { "pending" } ) } } ) );
    const Object tabFields = with( windowTab, { { "kind", choices( { "file", "folder", "live", "unknown" } ) },
        { "document_id", text }, { "display_name", text }, { "associated_path", text }, { "tab_title", text } } );
    const Object flags{ { "ignore_case", boolean }, { "whole_word", boolean } };
    const Object slot = with( integer, { { "minimum", 1 } } );
    const Object labelEntries = array( object( with( flags, { { "text", text } } ) ) );
    const Object labels = object( { { "revision", decimal}, { "slots", array( object( { { "label", slot }, { "entries", labelEntries } } ) ) } } );
    const Object query{ { "text", text }, { "match_case", boolean }, { "use_regexp", boolean },
                        { "inverse", boolean }, { "boolean", boolean } };
    const Object filter = with( query, { { "auto_refresh", boolean }, { "keep_results", boolean } } );
    const Object draft = object( with( filter, { { "revision", decimal} } ) );
    const Object applied{ { "applied_filter", object( with( query, { { "complete", boolean } } ) ) } };
    const Object paneFields{ { "pane_id", nullable( identifier) }, { "pane", panes.value( "pane" ) },
        { "title", text }, { "current", boolean }, { "marks_owner_id", nullable( identifier) },
        { "coordinates", choices( { "presented-model-zero-based" } ) }, { "available", boolean },
        { "row_count", decimal}, { "top_row", nullable( decimal) }, { "follow", boolean }, { "wrap", boolean },
        { "visible", boolean }, { "focused", boolean }, { "selection_kind", choices( { "none", "line", "lines", "portion" } ) },
        { "selected_row", nullable( decimal) } };

    Object features{ { "explicit_transaction_writes", strings }, { "implicit_document_kinds", strings },
        { "implicit_filter_kinds", strings }, { "filter_source_policy", text }, { "folder_filter_source_policy", text } };
    for ( const auto* key : { "explicit_transaction_filter_draft_reads", "explicit_lifecycle_transactions", "applied_filters",
            "filter_kept_pane_creation", "explicit_filter_transactions", "applied_filter_observation", "source_navigation",
            "live_lifecycle", "event_subscription", "durable_transactions", "content_requires_explicit_request", "method_schemas" } )
        features.insert( key, boolean );
    Object limits;
    for ( const auto* key : { "frame_bytes", "transactions", "transaction_lifetime_ms", "transaction_owners", "snapshots",
            "operations", "read_rows", "read_source_bytes", "read_text_units", "read_output_bytes", "snapshot_lifetime_ms",
            "concurrent_document_preparations", "document_preparation_lifetime_ms", "explicit_publication_participants",
            "filter_pattern_units", "concurrent_filter_preparations", "filter_preparation_lifetime_ms" } ) limits.insert( key, integer );
    add( "capabilities.get", "Discover available methods, feature flags and runtime limits; pass one method name to schema.get for its wire contract.",
        object(), object( { { "protocol_version", integer }, { "methods", strings }, { "features", object( features ) }, { "limits", object( limits ) } } ) );
    const Object window = object( { { "window_id", identifier}, { "revision", decimal}, { "current_tab_id", text },
        { "tabs", array( object( tabFields ) ) } } );
    add( "state.get", "Inventory published windows and tabs, not private staged documents or log rows.", object(),
        object( { { "scope", choices( { "window-tab-inventory" } ) }, { "windows", array( window ) } } ) );
    add( "tab.get", "Inspect a tab, including staged labels and repeatable filter draft when transaction_id is supplied. A private tab requires its owning transaction. The filter draft is not an applied result.",
        object( tab, transactionId ), object( with( tabFields, { { "labels", nullable( labels ) }, { "filter_draft", nullable( draft ) } } ) ) );
    add( "pane.list", "List actual panes (including kept results) with metadata, not row payloads. A private tab requires its owning transaction. Visible tabs expose published panes, not pending filter results.",
        object( tab, transactionId ), object( with( tab, { { "panes", array( object( paneFields, applied ) ) } } ) ) );
    add( "pane.get", "Inspect one concrete pane_id or the current main/results alias, not both. Private tabs require their transaction. Coordinates refer to presented rows, not snapshot ordinals.",
        with( object( tab, with( transactionId, panes ) ), paneSelector ), object( with( tab, paneFields ), applied ) );
    add( "pane.navigate", "Select one visible data row via a retained public snapshot ordinal and an explicit pane ID, not an alias or a stored source line. Immediate UI action; not transaction-scoped. Returns the zero-based presented coordinate, never text.",
        object( with( with( tab, snap ), { { "pane_id", identifier}, { "row", decimal} } ) ),
        object( with( tab, { { "pane_id", identifier}, { "presented_row", decimal} } ) ) );

    const Object capture = with( object( with( tab, { { "scope", choices( { "presented", "matches" } ) } } ), with( panes, transactionId ) ), paneSelector );
    add( "source.capture", "Retain a bounded-lifetime snapshot; no row text is returned. Choose exactly one pane selector; matches is valid only on results. All explicit captures (including public tabs) belong to that transaction and retire when it ends.",
        capture, object( with( snap, { { "row_count", decimal}, { "lifetime_ms", integer } } ) ) );
    add( "source.read", "Start an explicit bounded content read. start is a zero-based snapshot ordinal; the entire range must exist. Supply exactly the capture's transaction scope. Poll operation.get in that same scope for rows, subject to advertised byte/text limits.",
        object( with( snap, { { "start", decimal}, { "count", with( integer, { { "minimum", 1 }, { "maximum", 256 } } ) } } ), transactionId ), pending );
    add( "source.release", "Release a retained snapshot in its capture scope. Already staged mark proofs survive release.",
        object( snap, transactionId ), object( with( snap, { { "status", choices( { "released" } ) } } ) ) );

    const Object identity = object( { { "origin", choices( { "indexed_file", "capture", "concatenated", "memory", "unknown" } ) },
        { "incarnation", decimal}, { "member", decimal}, { "row", decimal} } );
    const Object row = object( { { "row", decimal}, { "source_line", decimal}, { "identity", identity },
        { "copyable", boolean }, { "match", boolean }, { "text", text } } );
    const Object readResult = object( with( snap, { { "start", decimal}, { "source_bytes", decimal}, { "rows", array( row ) } } ) );
    const Object documentResult = object( with( windowTab, { { "opened", boolean } } ), { { "staged", yes } } );
    const Object stagedPane = object( with( windowTab, { { "staged", yes } } ), { { "pane_id", identifier} } );
    const Object updatedPane = object( with( windowTab, { { "updated", yes }, { "pane_id", identifier} } ) );
    const Object operation = with( operationId, { { "cancel_requested", boolean } } );
    const Object operationState = either( {
        object( with( operation, { { "status", choices( { "pending", "cancelled" } ) } } ) ),
        object( with( operation, { { "status", choices( { "failed" } ) }, { "error", error } } ) ),
        object( with( operation, { { "status", choices( { "completed" } ) },
            { "result", either( { readResult, documentResult, stagedPane, updatedPane } ) } } ) ) } );
    add( "operation.get", "Poll pending work. Document/filter preparation receipts (even explicit stages) use no transaction_id; staged:true means staged, not committed. For source.read use its exact scope; only a requested content read returns rows. Terminal explicit transactions retire private read handles.",
        object( operationId, transactionId ), operationState );
    add( "operation.cancel", "Request cancellation and return the current operation record (possibly already complete). Document/filter receipt IDs are unscoped; cancelling pending explicit preparation aborts its transaction. Private source.read operation IDs require their owning transaction.",
        object( operationId, transactionId ), operationState );

    const Object open = choices( { "open" } );
    const Object committed = choices( { "committed" } );
    const Object preparing = choices( { "preparing", "committed" } );
    const Object receipt = object( with( transactionId, { { "status", choices( { "open", "preparing", "committed", "aborted",
        "expired", "conflict", "target_closed", "invalid_argument", "resource_limit", "preparation_failed" } ) } } ), { { "error", error } } );
    const Object begun = object( with( transactionId, { { "status", open } } ) );
    const Object committing = object( with( transactionId, { { "status", preparing } } ) );
    const Object trackedWrite = either( { begun, committing } );
    const Object labelWrite = either( { begun, object( { { "status", committed } } ) } );
    add( "transaction.begin", "Begin a bounded, non-durable transaction. Staged writes are private until commit. At most one visible publication participant (document lifecycle or visible filter); do not combine it with visible mark writes. A rejected bound request generally invalidates the open transaction.",
        object(), begun );
    add( "transaction.get", "Inspect the transaction even during asynchronous work or after a lost commit response. Terminal records can expire or be evicted. Inspect status and error.code, not just RPC success.",
        object( transactionId ), receipt );
    add( "transaction.commit", "Validate and publish staged changes together. Wait for outstanding stage operation receipts first (TRANSACTION_BUSY). preparing is not committed; poll transaction.get. Repeating a retained committed ID does not replay writes.",
        object( transactionId ), committing );
    add( "transaction.abort", "Discard open/preparing work and its private handles; cannot undo a committed transaction.",
        object( transactionId ), receipt );

    add( "label.list", "Read the tab's label slots and revision, including own staged edits. Private tabs require transaction_id.", object( tab, transactionId ), labels );
    const Object texts{ { "texts", with( strings, { { "maxItems", 1000 } } ) } };
    add( "label.set", "Assign texts to an existing one-based label slot (not a replacement of all entries). Stage with transaction_id or commit immediately. expected_revision checks the label owner's revision.",
        object( with( with( tab, texts ), { { "label", slot } } ), with( write,
            { { "ignore_case", with( boolean, { { "default", true } } ) }, { "whole_word", with( boolean, { { "default", false } } ) } } ) ), labelWrite );
    add( "label.remove", "Remove specified texts across the tab's label slots. Stage with transaction_id or commit immediately.",
        object( with( tab, texts ), write ), labelWrite );
    add( "label.clear", "Clear all label slots for the tab. Stage with transaction_id or commit immediately.", object( tab, write ), labelWrite );

    const Object group = object( { { "member_path", nullable( text ) }, { "rows", array( decimal) } } );
    add( "mark.list", "Observe an owner's stored mark coordinates, not snapshot ordinals or log text. Get owner_id from pane metadata. Transactional reads see staged marks.",
        object( owner, transactionId ), object( with( owner, { { "revision", decimal}, { "scope", choices( { "source-model", "folder-tab" } ) },
            { "coordinates", choices( { "stored-source-model-zero-based" } ) }, { "groups", array( group ) } } ) ) );
    const Object evidence = object( with( with( owner, snap ), { { "rows", note( with( array( decimal),
        { { "minItems", 1 }, { "maxItems", 256 } } ), "Zero-based copyable snapshot ordinals for this mark owner. The snapshot must be public or belong to this transaction." ) } } ), write );
    add( "mark.add", "Add evidence-backed rows; stage with transaction_id or autocommit. Poll the returned transaction if preparing. Visible mark writes cannot mix with a visible publication participant.", evidence, trackedWrite );
    add( "mark.remove", "Remove evidence-backed rows; stage with transaction_id or autocommit. Poll the returned transaction if preparing. Visible mark writes cannot mix with a visible publication participant.", evidence, trackedWrite );
    add( "mark.replace", "Replace all marks of this owner with the evidence-backed rows. For an empty set use mark.clear. Stage with transaction_id or autocommit; poll if preparing. Visible mark writes cannot mix with a visible publication participant.", evidence, trackedWrite );
    add( "mark.clear", "Clear the entire mark owner without a snapshot; stage with transaction_id or autocommit. Poll if preparing. Visible mark writes cannot mix with a visible publication participant.", object( owner, write ), trackedWrite );

    const Object outColor = nullable( with( text, { { "pattern", "^#[0-9a-fA-F]{8}$" } } ) );
    const Object color = note( text, "Valid Qt color string, for example #ff336699 (ARGB)." );
    const Object outRule = object( { { "pattern", text }, { "ignore_case", boolean }, { "use_regexp", boolean },
        { "match_only", boolean }, { "foreground", outColor }, { "background", outColor }, { "variate_colors", boolean }, { "color_variance", integer } } );
    const Object rule = object( { { "pattern", text }, { "foreground", color }, { "background", color } },
        { { "ignore_case", with( boolean, { { "default", false } } ) }, { "use_regexp", with( boolean, { { "default", true } } ) },
          { "match_only", with( boolean, { { "default", false } } ) }, { "variate_colors", with( boolean, { { "default", false } } ) },
          { "color_variance", with( integer, { { "minimum", 5 }, { "maximum", 99 }, { "default", 15 } } ) } } );
    const auto sets = [&]( const Object& item ) { return with( array( object( {
        { "id", identifier}, { "name", text }, { "rules", array( item ) } } ) ), { { "maxItems", 128 } } ); };
    const Object palette = array( object( { { "name", text }, { "foreground", outColor }, { "background", outColor }, { "cycle", boolean } } ) );
    add( "highlighter.list", "Read global highlighter sets, active IDs, and the read-only label palette/defaults. With a transaction, read its staged snapshot.", object( {}, transactionId ),
        object( { { "revision", decimal}, { "sets", sets( outRule ) }, { "active_set_ids", strings },
                  { "label_palette", palette }, { "label_defaults", object( flags ) } } ) );
    add( "highlighter.replace", "Replace all global sets and active IDs (empty arrays clear them); preserve the label palette. Set IDs must be unique and every active ID must refer to one. At most 1024 rules and 1048576 UTF-16 text units in aggregate. Stage with transaction_id or autocommit; poll if preparing.",
        object( { { "sets", sets( rule ) }, { "active_set_ids", with( array( identifier), { { "maxItems", 128 }, { "uniqueItems", true } } ) } }, write ), trackedWrite );

    const Object follow = with( boolean, { { "default", false } } );
    const Object path = note( with( identifier, { { "maxLength", 32768 } } ), "Absolute path, no NUL, at most 32768 UTF-16 units." );
    const Object openParams = with( object( { { "window_id", identifier}, { "path", path } }, with( transactionId,
        { { "kind", with( choices( { "file", "folder" } ), { { "default", "file" } } ) }, { "follow", follow } } ) ),
        { { "not", Object{ { "required", Array{ "kind", "follow" } },
            { "properties", Object{ { "kind", choices( { "folder" } ) } } } } } } );
    const auto complete = [&]( bool opened ) { return object( with( operationId, { { "status", choices( { "completed" } ) },
        { "result", object( with( windowTab, { { "opened", Object{ { "const", opened } } } } ), { { "staged", yes } } ) } } ) ); };
    add( "documents.open", "Prepare a regular file (default) or folder without selecting it; reuse an existing tab. Poll the operation without transaction_id. With transaction_id, completed means staged, not published: use its tab_id in that transaction, then commit. No live lifecycle. Counts as one visible publication participant.",
        openParams, either( { pending, complete( true ) } ) );
    add( "documents.close", "Close an existing file/folder tab, not its file; live tabs unsupported. With transaction_id close is staged, not published, until commit. Uses the single visible publication participant; the completed operation is unscoped.",
        object( windowTab, transactionId ), complete( false ) );
    const Object patch = object( {}, with( filter, { { "text", note( with( text, { { "maxLength", 32767 } } ), "No NUL; at most 32767 UTF-16 units. Empty resets the native filter." ) } } ) );
    add( "filter.apply", "Patch the tab's current filter draft; omitted fields retain their values. expected_revision checks the draft. Poll operation.get without transaction_id. On implicit completion updated:true identifies the pane. An explicit stage returns staged:true; a private tab exposes pane_id and rows inside the transaction, but a visible tab exposes only a staged draft until commit. A visible filter consumes the single visible publication participant. File/live use unchanged-snapshot admission; folder uses a retained scan.",
        object( with( tab, { { "filter", patch } } ), write ), pending );
    const auto schemaMethod = "schema.get";
    auto targets = Array::fromStringList( methods.keys() );
    targets.append( schemaMethod );
    const Object anySchema{ { "type", "object" } }; // A JSON Schema, not an instance of that schema.
    add( schemaMethod, "Describe one advertised method's named params and success result. Does not execute the method, inspect a transaction or return application data. Errors use the existing JSON-RPC envelope.",
        object( { { "method", choices( targets ) } } ),
        object( { { "method", identifier}, { "description", text }, { "params", anySchema }, { "result", anySchema } } ) );
    return methods;
}
} // namespace

DecodedResponse methodSchema( const RpcRequest& request )
{
    const auto target = request.params.value( "method" );
    if ( request.params.size() != 1 || !target.isString() || target.toString().isEmpty() || target.toString().size() > 128 )
        return RpcError{ request.id, -32602, "Expected one method name", Object{ { "code", "INVALID_PARAMS" } }, true, false };
    static const Object catalog = buildCatalog();
    auto schema = catalog.value( target.toString() );
    if ( schema.isUndefined() )
        return RpcError{ request.id, -32601, "Unknown method", Object{ { "code", "METHOD_NOT_FOUND" } }, true, false };
    return schema;
}
} // namespace klogg::control
