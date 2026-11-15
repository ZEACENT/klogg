#include "controlcli.h"

#include <algorithm>
#include <limits>
#include <optional>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QUuid>

#include "controlframing.h"
#include "qtcontainerindex.h"

namespace klogg::control {
namespace {
const char Help[] =
    "Usage: kloggctl [--instance ID] [--timeout-ms MS] COMMAND\n"
    "  --help | -h                         Show this offline help\n"
    "  --version | version                Show offline JSON version\n"
    "  instance list                      List public instance metadata\n"
    "  call METHOD [--params JSON | --params-file PATH|-]\n"
    "  state get\n"
    "  tab get --tab ID [--transaction ID]\n"
    "  label list --tab ID [--transaction ID]\n"
    "  pane get --tab ID --pane main|results\n"
    "  transaction begin\n"
    "  transaction get|commit|abort --transaction ID\n"
    "\n"
    "Params must be one JSON object, at most 1048576 UTF-8 bytes; the\n"
    "complete request must also fit that limit. --params-file - reads stdin\n"
    "until EOF; other commands do not read stdin. Input is never executed.\n"
    "The transport timeout is a positive decimal millisecond count (default\n"
    "5000, maximum 2147483647); it does not bound the stdin producer.\n"
    "An explicit missing instance never falls back. Without --instance,\n"
    "exactly one discovered instance is required. No GUI is launched.\n"
    "Operational stdout is one JSON-RPC result/error object; stderr is for\n"
    "diagnostics. Help alone is text. Large counts/revisions remain strings.\n"
    "Exit: 0 success, 2 invalid arguments/input, 3 unavailable/ambiguous or\n"
    "not sent, 4 server rejection, 5 Outcome unknown. Never automatically\n"
    "retry an unknown outcome: inspect transaction/operation state first.\n";

CliResult jsonResult( CliExit exit, const QJsonObject& object )
{
    return { exit, QJsonDocument( object ).toJson( QJsonDocument::Compact ) + '\n', {} };
}

CliResult success( const QJsonValue& result, const QJsonValue& id = QJsonValue::Null )
{
    return jsonResult( CliExit::Success,
        { { "jsonrpc", "2.0" }, { "id", id }, { "result", result } } );
}

CliResult failure( CliExit exit, const QString& symbol, const QString& message,
                   const QJsonValue& id = QJsonValue::Null, int code = -32000 )
{
    return jsonResult( exit, { { "jsonrpc", "2.0" }, { "id", id },
        { "error", QJsonObject{ { "code", code }, { "message", message },
            { "data", QJsonObject{ { "code", symbol } } } } } } );
}

CliResult invalidArguments()
{
    return failure( CliExit::InvalidArguments, "INVALID_ARGUMENTS",
                    "Invalid arguments or input; use --help for the supported syntax", {}, -32602 );
}

struct Command {
    QString method;
    QString instanceId;
    QJsonObject params;
    int timeoutMs = 5000;
    bool list = false;
};

std::optional<QByteArray> readParams( QIODevice& device )
{
    if ( !device.isReadable() ) return {};
    QByteArray bytes;
    char chunk[8192];
    while ( bytes.size() <= static_cast<ContainerIndex>( MaxFrameBytes ) ) {
        const auto remaining = static_cast<qint64>( MaxFrameBytes ) + 1 - bytes.size();
        const auto size = device.read( chunk, std::min( remaining, static_cast<qint64>( sizeof( chunk ) ) ) );
        if ( size < 0 ) return {};
        if ( size == 0 ) break;
        bytes.append( chunk, static_cast<ContainerIndex>( size ) );
    }
    if ( bytes.size() > static_cast<ContainerIndex>( MaxFrameBytes ) ) return {};
    return bytes;
}

std::optional<Command> parse( const QStringList& arguments, QIODevice& standardInput )
{
    QMap<QString, QString> options;
    QStringList words;
    const QStringList known{ "--instance", "--timeout-ms", "--params", "--params-file",
                             "--tab", "--transaction", "--pane" };
    for ( ContainerIndex i = 0; i < arguments.size(); ++i ) {
        const auto& arg = arguments.at( i );
        if ( arg.startsWith( '-' ) ) {
            if ( !known.contains( arg ) || options.contains( arg ) || i + 1 == arguments.size() )
                return {};
            options.insert( arg, arguments.at( ++i ) );
        }
        else {
            words.push_back( arg );
        }
    }
    if ( words.size() != 2 ) return {};
    Command command;
    if ( options.contains( "--instance" ) ) {
        command.instanceId = options.take( "--instance" );
        if ( command.instanceId.isEmpty() ) return {};
    }
    if ( options.contains( "--timeout-ms" ) ) {
        const auto value = options.take( "--timeout-ms" );
        if ( value.isEmpty() || !std::all_of( value.cbegin(), value.cend(), []( QChar ch ) {
                 return ch >= QLatin1Char( '0' ) && ch <= QLatin1Char( '9' );
             } ) ) return {};
        bool ok = false;
        const auto timeout = value.toULongLong( &ok );
        if ( !ok || timeout == 0 || timeout > static_cast<qulonglong>( std::numeric_limits<int>::max() ) )
            return {};
        command.timeoutMs = static_cast<int>( timeout );
    }
    if ( words == QStringList{ "instance", "list" } ) {
        if ( !command.instanceId.isEmpty() ) return {};
        command.list = true;
    }
    else if ( words.first() == "call" ) {
        command.method = words.at( 1 );
        if ( options.contains( "--params" ) && options.contains( "--params-file" ) ) return {};
        std::optional<QByteArray> bytes = QByteArray( "{}" );
        if ( options.contains( "--params" ) ) {
            const auto value = options.take( "--params" );
            // Reject before allocating UTF-8 for an already oversized argument.
            if ( value.size() > static_cast<ContainerIndex>( MaxFrameBytes ) ) return {};
            bytes = value.toUtf8();
        }
        else if ( options.contains( "--params-file" ) ) {
            const auto path = options.take( "--params-file" );
            if ( path == "-" ) bytes = readParams( standardInput );
            else {
                QFile file( path );
                if ( path.isEmpty() || !file.open( QIODevice::ReadOnly ) ) return {};
                bytes = readParams( file );
                if ( file.error() != QFileDevice::NoError ) return {};
            }
        }
        if ( !bytes || bytes->size() > static_cast<ContainerIndex>( MaxFrameBytes ) ) return {};
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson( *bytes, &error );
        if ( error.error != QJsonParseError::NoError || !document.isObject() ) return {};
        command.params = document.object();
    }
    else {
        command.method = words.join( '.' );
        const bool tabQuery = command.method == "tab.get" || command.method == "label.list";
        const bool paneQuery = command.method == "pane.get";
        if ( paneQuery ) {
            const auto pane = options.take( "--pane" );
            if ( pane != "main" && pane != "results" ) return {};
            command.params.insert( "pane", pane );
        }
        if ( tabQuery || paneQuery ) {
            const auto tab = options.take( "--tab" );
            if ( tab.isEmpty() ) return {};
            command.params.insert( "tab_id", tab );
        }
        else if ( command.method != "state.get" && command.method != "transaction.begin"
                  && command.method != "transaction.get" && command.method != "transaction.commit"
                  && command.method != "transaction.abort" ) return {};
        const bool transactionRequired = words.first() == "transaction" && words.at( 1 ) != "begin";
        if ( transactionRequired || ( tabQuery && options.contains( "--transaction" ) ) ) {
            const auto transaction = options.take( "--transaction" );
            if ( transaction.isEmpty() ) return {};
            command.params.insert( "transaction_id", transaction );
        }
    }
    if ( !options.isEmpty() ) return {};
    return command;
}

QString remoteSymbol( const RpcError& error, const QString& token )
{
    const auto code = error.data.toObject().value( "code" ).toString();
    // Copy only the machine-readable symbol, never remote messages or payloads.
    if ( code.isEmpty() || code.size() > 64 || ( !token.isEmpty() && code.contains( token ) )
         || !std::all_of( code.cbegin(), code.cend(), []( QChar ch ) {
                return ( ch >= QLatin1Char( 'A' ) && ch <= QLatin1Char( 'Z' ) )
                       || ( ch >= QLatin1Char( '0' ) && ch <= QLatin1Char( '9' ) )
                       || ch == QLatin1Char( '_' );
            } ) ) return QStringLiteral( "REMOTE_REJECTED" );
    return code;
}
} // namespace

CliResult runControlCli( const QStringList& arguments, QIODevice& standardInput,
                         const QString& version, const CliServices& services,
                         const RuntimeOptions& runtime )
{
    if ( arguments == QStringList{ "--help" } || arguments == QStringList{ "-h" } )
        return { CliExit::Success, Help, {} };
    if ( arguments == QStringList{ "--version" } || arguments == QStringList{ "version" } )
        return success( QJsonObject{ { "version", version }, { "protocol_version", ControlProtocolVersion } } );
    const auto command = parse( arguments, standardInput );
    if ( !command ) return invalidArguments();

    const RpcRequest request{ QUuid::createUuid().toString( QUuid::WithoutBraces ),
                              command->method, command->params };
    if ( !command->list ) {
        // Reuse the protocol's validation and budget, including envelope overhead.
        const auto bytes = QJsonDocument( QJsonObject{ { "jsonrpc", "2.0" }, { "id", request.id },
            { "method", request.method }, { "params", request.params } } ).toJson( QJsonDocument::Compact );
        if ( !std::holds_alternative<RpcRequest>( decodeRequest( bytes ) ) ) return invalidArguments();
    }
    const auto discovery = services.discover( runtime );
    if ( !discovery.error.isEmpty() )
        return failure( CliExit::Unavailable, "DISCOVERY_FAILED", "Cannot safely discover local instances" );
    if ( command->list ) {
        QJsonArray instances;
        for ( const auto& instance : discovery.instances ) {
            instances.append( QJsonObject{ { "instance_id", instance.instanceId },
                { "installation_id", instance.installationId }, { "profile_id", instance.profileId },
                { "protocol_min", instance.protocolMin }, { "protocol_max", instance.protocolMax } } );
        }
        return success( QJsonObject{ { "instances", instances } } );
    }
    const InstanceDescriptor* selected = nullptr;
    for ( const auto& instance : discovery.instances ) {
        if ( !command->instanceId.isEmpty() && instance.instanceId != command->instanceId ) continue;
        if ( selected ) return failure( CliExit::Unavailable, "AMBIGUOUS_INSTANCE",
                                       "Select one unambiguous instance with --instance" );
        selected = &instance;
    }
    if ( !selected ) return failure( CliExit::Unavailable, "INSTANCE_NOT_FOUND", "No matching local instance" );

    // Exactly one application request. A lost reply must never trigger replay.
    const auto reply = services.call( *selected, request, command->timeoutMs );
    if ( reply.status == ClientStatus::Success && reply.response ) {
        if ( const auto* value = std::get_if<QJsonValue>( &*reply.response ) )
            return success( *value, request.id );
    }
    if ( reply.status == ClientStatus::Rejected && reply.response ) {
        if ( const auto* error = std::get_if<RpcError>( &*reply.response ); error && error->remote )
            return failure( CliExit::Rejected, remoteSymbol( *error, selected->token ),
                            "The server rejected the request", request.id, error->code );
    }
    if ( reply.status == ClientStatus::NotSent )
        return failure( CliExit::Unavailable, "NOT_SENT", "The application request was not sent", request.id );
    return failure( CliExit::OutcomeUnknown, "OUTCOME_UNKNOWN",
                    "Outcome unknown; do not automatically retry. Inspect transaction or operation state first",
                    request.id );
}
} // namespace klogg::control
