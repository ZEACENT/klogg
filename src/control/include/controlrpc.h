#ifndef KLOGG_CONTROLRPC_H
#define KLOGG_CONTROLRPC_H

#include <optional>
#include <variant>

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace klogg::control {

// Control protocol profile: JSON-RPC 2.0 envelopes, nonempty string request IDs,
// named parameters, and no request batches. Incoming notifications are rejected
// without execution or a response. Ordered/atomic work uses transaction.apply.
struct RpcRequest {
    QString id;
    QString method;
    QJsonObject params;
};

struct RpcError {
    QJsonValue id{ QJsonValue::Null };
    int code = -32600;
    QString message;
    QJsonValue data{ QJsonValue::Undefined };
    bool replyAllowed = true;
    bool remote = false; // received server error, not a local protocol failure
};

using DecodedRequest = std::variant<RpcRequest, RpcError>;
using DecodedResponse = std::variant<QJsonValue, RpcError>;

DecodedRequest decodeRequest( const QByteArray& payload );
DecodedResponse decodeResponse( const QByteArray& payload, const QString& expectedId );
std::optional<QByteArray> encodeSuccess( const QString& id, const QJsonValue& result );
std::optional<QByteArray> encodeError( const RpcError& error );

} // namespace klogg::control

#endif
