#ifndef KLOGG_MATCHSELECTIONJOB_H
#define KLOGG_MATCHSELECTIONJOB_H

#include <atomic>
#include <memory>
#include <optional>
#include <QRegularExpression>

#include "logsnapshot.h"
#include "matchprojection.h"
#include "selection.h"

namespace klogg {

struct MatchSelectionStep {
    enum class Outcome : std::uint8_t { More, Complete, Failed };
    uint64_t processed = 0;
    Outcome outcome = Outcome::More;
    QString error;
    std::shared_ptr<Selection> selection;
    std::optional<QString> clipboard;
    bool withLineNumbers = false;
};

// One serial worker at a time owns the matcher and its text. The GUI only
// observes completed steps and the cancellation flag; no mutable source is read.
class MatchSelectionJob {
public:
    MatchSelectionJob( LogSnapshot::Ptr source, LogSnapshot::Ptr presentation,
                       QRegularExpression expression,
                       std::shared_ptr<std::atomic<bool>> cancelled );
    MatchSelectionStep step();

private:
    LogSnapshot::Ptr source_;
    LogSnapshot::Ptr presentation_;
    QRegularExpression expression_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    LineNumber nextRow_ = 0_lnum;
    bool readingLine_ = false;
    QString text_;
    MatchTextColumns columns_{ QString{} };
    QRegularExpressionMatchIterator iterator_;
    std::vector<Selection::CapturedPortion> portions_;
};

} // namespace klogg
#endif
