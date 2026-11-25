#ifndef KLOGG_CONTROLTRANSPORT_H
#define KLOGG_CONTROLTRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <QObject>

#include "controlrpc.h"
#include "controlruntime.h"

namespace klogg::control {

struct TransportLimits {
    int maxConnections = 16;
    int handshakeTimeoutMs = 3000;
    qint64 maxQueuedWriteBytes = 2LL * 1024 * 1024;
};

// All server operations and callbacks run on the QObject's owner thread.
// Callbacks must be bounded/nonblocking; asynchronous application operations
// should return their operation handle. stop() is safe inside a callback.
class ControlServer : public QObject {
  public:
    using Handler = std::function<DecodedResponse( const RpcRequest& )>;
    explicit ControlServer( QObject* parent = nullptr );
    ~ControlServer() override;
    bool start( const RuntimeOptions& options, Handler handler, QString* error = nullptr,
                TransportLimits limits = {} );
    void stop();
    bool isRunning() const;
    std::size_t connectionCount() const;
    InstanceDescriptor descriptor() const;
    QString descriptorPath() const;

  private:
    struct Private;
    std::shared_ptr<Private> d_;
};

enum class ClientStatus : std::uint8_t {
    Success,
    Rejected,       // correlated server RPC rejection, no transport ambiguity
    NotSent,        // application request was never submitted
    OutcomeUnknown // submitted, but no trustworthy correlated response arrived
};

struct ClientResult {
    ClientStatus status = ClientStatus::NotSent;
    std::optional<DecodedResponse> response;
    QString error; // generic diagnostics, never includes credentials or exceptions
};

// Headless, blocking, one request/connection, no retry. A single finite budget
// covers connect + hello + application request. Do not call on a server's thread.
ClientResult callLocal( const InstanceDescriptor& instance, const RpcRequest& request,
                        int timeoutMs = 5000 );

} // namespace klogg::control
#endif
