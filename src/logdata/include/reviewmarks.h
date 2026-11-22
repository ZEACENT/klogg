#ifndef KLOGG_REVIEWMARKS_H
#define KLOGG_REVIEWMARKS_H

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <cstdint>
#include <optional>
#include <vector>
#include <QList>
#include <QString>
#include <QUuid>
#include "linetypes.h"

class QObject;

namespace klogg {

struct LogRowId;
struct ReviewMarkLocation {
    QString memberPath;
    LineNumber row = 0_lnum;
    bool operator==( const ReviewMarkLocation& other ) const
    {
        return memberPath == other.memberPath && row == other.row;
    }
    bool operator!=( const ReviewMarkLocation& other ) const { return !( *this == other ); }
};

// Stored mark coordinates, not evidence addresses. Source identity validation
// belongs to source-backed mutation preparation, never to path/index guessing.
struct ReviewMarksSnapshot {
    enum class Scope : std::uint8_t { SourceModel, FolderTab };
    struct Group {
        QString memberPath; // Empty for a single source-model owner.
        QList<LineNumber> rows;
        bool operator==( const Group& other ) const
        {
            return memberPath == other.memberPath && rows == other.rows;
        }
    };
    uint64_t revision = 0;
    Scope scope = Scope::SourceModel;
    std::vector<Group> groups;
};

// Shared admission for one transaction's private mark projections. Charges are
// conservative metadata-work estimates, not a bound on process RSS or source leases.
struct ReviewMarksBudget {
    static constexpr size_t MaxBytes = size_t{ 64 } * 1024 * 1024;
    size_t remainingBytes = MaxBytes;
    bool charge( size_t bytes )
    {
        if ( bytes > remainingBytes ) return false;
        remainingBytes -= bytes;
        return true;
    }
};

class ReviewMarksPublication {
  public:
    virtual ~ReviewMarksPublication() = default;
    virtual void install() noexcept = 0;
    virtual void release() noexcept = 0;
    virtual void notify() = 0;
};
class PreparedReviewMarks {
  public:
    virtual ~PreparedReviewMarks() = default;
    // Owner-thread only. Acquire every participating owner before installing any.
    virtual std::unique_ptr<ReviewMarksPublication> acquire() = 0;
};
using ReviewMarksPreparer = std::function<std::shared_ptr<PreparedReviewMarks>( const std::atomic<bool>& )>;

// A narrow facade over the existing authoritative store. Implementations retain
// their native representation (compressed source rows or per-file folder marks).
// No state is duplicated here; snapshots are bounded owner-thread values.
class ReviewMarksOwner {
  public:
    static constexpr std::size_t MaxMarks = 4096;
    static constexpr std::size_t MaxMembers = 128;
    static constexpr std::size_t MaxTextUnits = std::size_t{ 1024 } * 1024;
    ReviewMarksOwner() = default;
    ReviewMarksOwner( const ReviewMarksOwner& ) = delete;
    ReviewMarksOwner& operator=( const ReviewMarksOwner& ) = delete;
    virtual ~ReviewMarksOwner() = default;
    QString reviewMarksId() const { return id_; }
    virtual QObject* reviewMarksObject() = 0;
    virtual uint64_t reviewMarksRevision() const = 0;
    // Null means resource admission failed, before copying the collection.
    virtual std::optional<ReviewMarksSnapshot> reviewMarksSnapshot() const = 0;
    // Resolve only identities still retained by this owner's current sources.
    // Visibility does not affect existence. Never reopen a path or guess by row.
    virtual std::optional<ReviewMarkLocation> resolveReviewMark( const LogRowId& ) const { return {}; }
    // Capture on the owner thread; the returned function runs only on detached
    // values. Empty means admission failed. Publication is always owner-thread.
    virtual ReviewMarksPreparer prepareReviewMarks( const ReviewMarksSnapshot&, ReviewMarksBudget& ) { return {}; }

  private:
    const QString id_ = QUuid::createUuid().toString( QUuid::WithoutBraces );
};

} // namespace klogg
#endif
