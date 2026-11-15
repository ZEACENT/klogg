#ifndef KLOGG_REVIEWTRANSACTION_H
#define KLOGG_REVIEWTRANSACTION_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <QPointer>

#include "colorlabelscontroller.h"
#include "searchtoolbar.h"

// Owner-thread review-state transaction. The first supported write scope is
// per-tab color labels; filter drafts can participate as read dependencies.
// Source indexing, filter-result adoption, marks and global highlighters are NOT
// transactional through this class yet. There is no filesystem/DB durability.
class ReviewTransaction {
  public:
    enum class Status : uint8_t {
        Open, Committed, Aborted, Conflict, TargetClosed, InvalidArgument, ResourceLimit
    };
    static constexpr size_t MaxOwners = 64;

    ReviewTransaction() = default;
    ReviewTransaction( const ReviewTransaction& ) = delete;
    ReviewTransaction& operator=( const ReviewTransaction& ) = delete;

    // Reads see the captured base plus this transaction's own writes. Reads do
    // not refresh dependencies: otherwise an intervening human edit could hide.
    std::optional<ColorLabelsController::Snapshot> labels( ColorLabelsController& owner );
    std::optional<SearchToolbar::FilterDraftSnapshot> filterDraft( SearchToolbar& owner );
    Status stageLabels( ColorLabelsController& owner,
                        const std::vector<ColorLabelsController::Edit>& edits );
    Status commit();
    Status abort();
    Status status() const { return status_; }

  private:
    struct Labels {
        QPointer<ColorLabelsController> owner;
        uint64_t revision = 0;
        ColorLabelsManager candidate;
    };
    struct Filter {
        QPointer<SearchToolbar> owner;
        SearchToolbar::FilterDraftSnapshot snapshot;
    };
    Labels* captureLabels( ColorLabelsController& owner );
    Status finish( Status status );

    Status status_ = Status::Open;
    size_t commandCount_ = 0;
    size_t textUnits_ = 0;
    size_t textCount_ = 0;
    std::vector<Labels> labels_;
    std::vector<Filter> filters_;
};

#endif
