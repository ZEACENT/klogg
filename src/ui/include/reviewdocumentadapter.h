#ifndef KLOGG_REVIEWDOCUMENTADAPTER_H
#define KLOGG_REVIEWDOCUMENTADAPTER_H

#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <QObject>
#include <QPointer>

#include "controlrpc.h"

class MainWindow;
class ReviewContentAdapter;
class ReviewTransaction;
class QWidget;
enum class DocumentKind;

// Worker-owned acquisition, transaction-owned private documents, and native
// membership publication/retirement on the GUI owner thread.
class ReviewDocumentAdapter : public QObject {
public:
    using WindowProvider = std::function<std::vector<QPointer<MainWindow>>() >;
    static constexpr int MaxConcurrentPreparations = 2;
    static constexpr qint64 PreparationLifetimeMs = qint64{ 5 } * 60000;
    struct StagedDocument {
        QString windowId;
        DocumentKind kind;
        QString documentId;
        QPointer<QWidget> widget;
    };
    ReviewDocumentAdapter( WindowProvider windows, ReviewContentAdapter& operations );
    ~ReviewDocumentAdapter() override;
    klogg::control::DecodedResponse dispatch( const klogg::control::RpcRequest& request,
        const std::shared_ptr<ReviewTransaction>& transaction = {} );
    std::optional<StagedDocument> stagedDocument( const std::shared_ptr<ReviewTransaction>& transaction,
                                                  const QString& tabId ) const;
    void reset();
private:
    struct Entry {
        std::weak_ptr<ReviewTransaction> transaction;
        QPointer<MainWindow> window;
        QString tabId;
        StagedDocument document;
    };
    std::vector<Entry> staged_;
    WindowProvider windows_;
    ReviewContentAdapter& operations_;
};

#endif
