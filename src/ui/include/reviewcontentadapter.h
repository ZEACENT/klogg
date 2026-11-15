#ifndef KLOGG_REVIEWCONTENTADAPTER_H
#define KLOGG_REVIEWCONTENTADAPTER_H

#include <functional>
#include <memory>
#include <QObject>

#include "controlrpc.h"
#include "logsnapshot.h"

// Explicit content access owns finite immutable leases and asynchronous reads.
// It never resolves widgets or reopens their original paths. The GUI adapter
// captures through the actual pane owner before handing a lease to this class.
class ReviewContentAdapter : public QObject {
  public:
    using Clock = std::function<qint64()>;
    static constexpr size_t MaxSnapshots = 16;
    static constexpr size_t MaxOperations = 16;
    static constexpr int MaxConcurrentReads = 4;
    static constexpr qint64 LifetimeMs = 60000;
    static constexpr uint64_t MaxRows = 256;
    static constexpr uint64_t MaxSourceBytes = 1024 * 1024;
    static constexpr uint64_t MaxTextUnits = 256 * 1024;
    static constexpr uint64_t MaxOutputBytes = 512 * 1024;

    explicit ReviewContentAdapter( Clock now, QObject* parent = nullptr );
    ~ReviewContentAdapter() override;
    klogg::control::DecodedResponse capture( const klogg::control::RpcRequest& request,
                                            klogg::LogSnapshot::Ptr snapshot );
    klogg::control::DecodedResponse dispatch( const klogg::control::RpcRequest& request );
    void reset();

  private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif
