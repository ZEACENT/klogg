#ifndef KLOGG_REVIEWDOCUMENTADAPTER_H
#define KLOGG_REVIEWDOCUMENTADAPTER_H

#include <functional>
#include <vector>
#include <QObject>
#include <QPointer>

#include "controlrpc.h"

class MainWindow;
class ReviewContentAdapter;

// Implicit, single-command document transactions. Preparation is worker-owned;
// native membership publication and retirement stay on the GUI owner thread.
class ReviewDocumentAdapter : public QObject {
public:
    using WindowProvider = std::function<std::vector<QPointer<MainWindow>>() >;
    static constexpr int MaxConcurrentPreparations = 2;
    static constexpr qint64 PreparationLifetimeMs = 5 * 60000;
    ReviewDocumentAdapter( WindowProvider windows, ReviewContentAdapter& operations );
    ~ReviewDocumentAdapter() override;
    klogg::control::DecodedResponse dispatch( const klogg::control::RpcRequest& request );
    void reset();
private:
    WindowProvider windows_;
    ReviewContentAdapter& operations_;
};

#endif
