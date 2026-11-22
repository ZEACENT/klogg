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

// Owner-thread atomic publication of annotations and a prepared document/filter.
// Native projections/matchers prepare privately off-thread. Final validation and
// installation never yield. There is no filesystem/DB durability promise.
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
        std::vector<std::shared_ptr<klogg::PreparedReviewMarks>> marks;
        klogg::AsyncWorkBudget::Lease permit;
    };
    struct PreparationInput {
        std::optional<HighlighterSetCollection::Snapshot> highlighters;
        RegexpEngine engine{};
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::vector<klogg::ReviewMarksPreparer> marks;
        klogg::AsyncWorkBudget::Lease permit;
    };
    using Preparer = std::function<std::optional<QFuture<Preparation>>( PreparationInput )>;

    explicit ReviewTransaction( Preparer prepare = {} );
    ~ReviewTransaction();
    ReviewTransaction( const ReviewTransaction& ) = delete;
    ReviewTransaction& operator=( const ReviewTransaction& ) = delete;

    // At most one visible, potentially fallible installation. A private document
    // may absorb many edits before its one adoption. All callbacks are required.
    // validate returns Open on success; it may acquire guards in the final turn.
    // install must leave visible state untouched on false/exception. Neither may
    // yield, notify observers, or invalidate dependencies/annotations. In
    // particular an adapter may not enter models whose mark guards will be held
    // during install. release cannot throw; each admitted participant receives it
    // exactly once even if never validated. Rejected participants remain unstaged.
    // notify is success-only, after all guards are released and state is terminal.
    struct Participant {
        std::function<Status()> validate;
        std::function<bool()> install;
        std::function<void()> release;
        std::function<void()> notify;
    };
    Status stageParticipant( Participant participant );
    // Reserve the Open transaction while an async command builds a private
    // candidate. A concurrent reservation returns nonterminal ResourceLimit;
    // commit returns Open (retry after completion), never publishing a prefix.
    // Optional nonthrowing cancellation runs once on abort/failure, not success.
    Status beginAsyncStage( std::function<void()> cancel = {} );
    void finishAsyncStage();
    void failAsyncStage( Status failure );
    bool asyncStagePending() const { return asyncStagePending_; }

    // Reads see the captured base plus this transaction's own writes. Reads do
    // not refresh dependencies: otherwise an intervening human edit could hide.
    std::optional<ColorLabelsController::Snapshot> labels( ColorLabelsController& owner );
    std::optional<SearchToolbar::FilterDraftSnapshot> filterDraft( SearchToolbar& owner );
    // Read overlay only: the staged participant owns the actual toolbar install.
    Status stageFilterDraft( SearchToolbar& owner, const SearchToolbar::FilterSpec& candidate );
    Status stageLabels( ColorLabelsController& owner,
                        const std::vector<ColorLabelsController::Edit>& edits );
    std::optional<HighlighterSetCollection::Snapshot> highlighters();
    std::optional<klogg::ReviewMarksSnapshot> marks( klogg::ReviewMarksOwner& owner );
    struct MarkEdit {
        enum class Kind : uint8_t { Add, Remove, Replace, Clear };
        Kind kind = Kind::Add;
        std::vector<klogg::LogRowId> rows;
    };
    // privateOwner is exclusively for an unexposed, participant-owned document
    // retired on failure. Its projection must be installed and unlocked before
    // adoption, which can synchronously read it. Visible marks always install last.
    Status stageMarks( klogg::ReviewMarksOwner& owner, const MarkEdit& edit, bool privateOwner = false );
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
        std::optional<bool> privateOwner;
    };
    struct Highlighters {
        HighlighterSetCollection::Snapshot base;
        HighlighterSetCollection::Snapshot candidate;
    };
    Labels* captureLabels( ColorLabelsController& owner );
    Filter* captureFilter( SearchToolbar& owner );
    Highlighters* captureHighlighters();
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
    std::optional<Participant> participant_;
    std::function<void()> cancelAsyncStage_;
    bool asyncStagePending_ = false;
    bool publishing_ = false;
    bool highlightersCommitted_ = false;
};

#endif
