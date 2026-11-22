#ifndef KLOGG_REVIEWCONTENTADAPTER_H
#define KLOGG_REVIEWCONTENTADAPTER_H

#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>
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
    // Shared bounded operation records for GUI-published work as well as reads.
    // Call current() immediately before installation and complete() before any
    // publication observer. Neither callback retains the adapter or a widget.
    struct PendingOperation {
        QString id;
        std::function<bool()> current;
        std::function<void( QJsonObject, QString )> complete;
    };
    std::optional<PendingOperation> beginOperation( std::function<void()> cancel,
                                                   qint64 lifetimeMs = LifetimeMs );
    enum class RowResolutionError : std::uint8_t { SnapshotNotFound, InvalidArgument, ResourceLimit, SourceUnavailable };
    using RowMetadata = std::vector<klogg::SnapshotRow>;
    using ResolvedRows = std::variant<RowMetadata, RowResolutionError>;
    // Resolve admitted lease ordinals without decoding. Returned values do not
    // pin content after release; mutation owners must revalidate each identity.
    ResolvedRows resolveRows( const QString& snapshotId, const std::vector<LineNumber>& ordinals );

  private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif
