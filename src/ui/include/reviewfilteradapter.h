#ifndef KLOGG_REVIEWFILTERADAPTER_H
#define KLOGG_REVIEWFILTERADAPTER_H

#include <functional>
#include <memory>
#include <QObject>
#include <QPointer>
#include <QString>

#include "controlrpc.h"
#include "searchtoolbar.h"

class QWidget;
class ReviewContentAdapter;
class ReviewTransaction;

// Filter writes and kept-pane creation, using the shared operation registry.
// Target resolution and strict wire decoding belong to ReviewControlAdapter.
class ReviewFilterAdapter : public QObject {
public:
    struct Target {
        QString windowId;
        QString tabId;
        QPointer<QWidget> widget;
        std::function<bool()> current;
    };
    static constexpr int MaxConcurrentPreparations = 2;
    static constexpr qint64 PreparationLifetimeMs = 5 * 60000;
    explicit ReviewFilterAdapter( ReviewContentAdapter& operations );
    ~ReviewFilterAdapter() override;
    klogg::control::DecodedResponse apply( const klogg::control::RpcRequest& request,
                                         Target target, const SearchToolbar::FilterSpec& candidate );
    klogg::control::DecodedResponse apply( const klogg::control::RpcRequest& request,
                                         Target target, const SearchToolbar::FilterSpec& candidate,
                                         std::shared_ptr<ReviewTransaction> transaction, bool privateDocument );
    void reset();
private:
    ReviewContentAdapter& operations_;
};

#endif
