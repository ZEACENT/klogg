#ifndef KLOGG_REVIEWTRANSACTION_H
#define KLOGG_REVIEWTRANSACTION_H

#include <atomic>
#include <functional>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <QPointer>
#include <QFuture>

#include "highlighterset.h"
#include "colorlabelscontroller.h"
#include "searchtoolbar.h"
#include "reviewmarks.h"
#include "logsnapshot.h"
#include "asyncworkbudget.h"

// Owner-thread review transaction for labels, source-backed marks and global
// highlighters, with filter-draft read dependencies. Native projections/matchers
// prepare privately off-thread; final validation and installation never yield.
// Source/filter-result adoption is not transactional yet. There is no
// filesystem/DB durability promise.
class ReviewTransaction {
  public:
    enum class Status : uint8_t {
        Open, Preparing, Committed, Aborted, Conflict, TargetClosed, InvalidArgument,
        ResourceLimit, PreparationFailed
    };
    static constexpr size_t MaxOwners = 64;
    static constexpr size_t MaxConcurrentPreparations = 2;

    struct Preparation {
        std::shared_ptr<HighlighterSetCollection::PreparedUpdate> update;
        Status failure = Status::PreparationFailed;
        std::vector<std::shared_ptr<klogg::PreparedReviewMarks>> marks{};
        klogg::AsyncWorkBudget::Lease permit{};
    };
    struct PreparationInput {
        std::optional<HighlighterSetCollection::Snapshot> highlighters;
        RegexpEngine engine{};
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::vector<klogg::ReviewMarksPreparer> marks{};
        klogg::AsyncWorkBudget::Lease permit{};
    };
    using Preparer = std::function<std::optional<QFuture<Preparation>>( PreparationInput )>;

    explicit ReviewTransaction( Preparer prepare = {} );
    ~ReviewTransaction();
    ReviewTransaction( const ReviewTransaction& ) = delete;
    ReviewTransaction& operator=( const ReviewTransaction& ) = delete;

    // Reads see the captured base plus this transaction's own writes. Reads do
    // not refresh dependencies: otherwise an intervening human edit could hide.
    std::optional<ColorLabelsController::Snapshot> labels( ColorLabelsController& owner );
    std::optional<SearchToolbar::FilterDraftSnapshot> filterDraft( SearchToolbar& owner );
    Status stageLabels( ColorLabelsController& owner,
                        const std::vector<ColorLabelsController::Edit>& edits );
    std::optional<HighlighterSetCollection::Snapshot> highlighters();
    std::optional<klogg::ReviewMarksSnapshot> marks( klogg::ReviewMarksOwner& owner );
    struct MarkEdit {
        enum class Kind : uint8_t { Add, Remove, Replace, Clear };
        Kind kind = Kind::Add;
        std::vector<klogg::LogRowId> rows;
    };
    Status stageMarks( klogg::ReviewMarksOwner& owner, const MarkEdit& edit );
    Status stageHighlighters( QList<HighlighterSet> sets, QStringList activeSetIds );
    bool committedHighlighters() const { return highlightersCommitted_; }
    std::optional<QFuture<Preparation>> preparationFuture() const { return preparation_; }
    // A pending private preparation returns Preparing. The application pumps
    // commit again on completion, revalidates every owner, then publishes once.
    Status commit( const std::function<void()>& published = {} );
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
    struct Marks {
        klogg::ReviewMarksOwner* owner = nullptr;
        QPointer<QObject> lifetime;
        klogg::ReviewMarksSnapshot snapshot;
        klogg::ReviewMarksSnapshot candidate;
        struct Proof {
            klogg::LogRowId identity;
            klogg::ReviewMarkLocation location;
        };
        std::vector<Proof> proofs;
    };
    struct Highlighters {
        HighlighterSetCollection::Snapshot base;
        HighlighterSetCollection::Snapshot candidate;
    };
    Labels* captureLabels( ColorLabelsController& owner );
    Marks* captureMarks( klogg::ReviewMarksOwner& owner );
    size_t ownerCount() const;
    Status finish( Status status );

    Status status_ = Status::Open;
    size_t commandCount_ = 0;
    size_t textUnits_ = 0;
    size_t textCount_ = 0;
    size_t markProofCount_ = 0;
    size_t markProofTextUnits_ = 0;
    std::vector<Labels> labels_;
    std::vector<Filter> filters_;
    std::vector<Marks> marks_;
    std::optional<Highlighters> highlighters_;
    Preparer preparer_;
    std::optional<QFuture<Preparation>> preparation_;
    std::shared_ptr<std::atomic<bool>> preparationCancelled_;
    std::optional<RegexpEngine> preparationEngine_;
    klogg::AsyncWorkBudget::Lease preparationPermit_;
    bool highlightersCommitted_ = false;
};

#endif
