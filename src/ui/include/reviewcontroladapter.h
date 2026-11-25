#ifndef KLOGG_REVIEWCONTROLADAPTER_H
#define KLOGG_REVIEWCONTROLADAPTER_H

#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QPointer>

#include "controlrpc.h"

class MainWindow;

// GUI-owner-thread boundary: resolve current runtime identities, observe the
// existing domain owners, then serialize. The provider is the application's
// existing window collection, not a second authoritative window/tab registry.
class ReviewControlAdapter : public QObject {
    Q_OBJECT
  public:
    using WindowProvider = std::function<std::vector<QPointer<MainWindow>>() >;
    using Clock = std::function<qint64()>; // Monotonic milliseconds; injectable for lease tests.
    static constexpr size_t MaxTransactions = 64;
    static constexpr qint64 TransactionLifetimeMs = 60000;
    explicit ReviewControlAdapter( WindowProvider windows, QObject* parent = nullptr );
    ReviewControlAdapter( WindowProvider windows, Clock now, QObject* parent = nullptr );
    void reset(); // Discard unpublished candidates when globally disabling control.
    ~ReviewControlAdapter() override;
    klogg::control::DecodedResponse dispatch( const klogg::control::RpcRequest& request );

  Q_SIGNALS:
    // Private work has retired, including canceled/expired work. Publication,
    // when still permitted, precedes this owner-thread completion notification.
    void preparationFinished( const QString& transactionId );

  private:
    struct State;
    std::unique_ptr<State> state_;
    WindowProvider windows_;
};

#endif
