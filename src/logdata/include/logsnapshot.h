#ifndef KLOGG_LOGSNAPSHOT_H
#define KLOGG_LOGSNAPSHOT_H

#include <QString>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

#include "linetypes.h"

namespace klogg {

// A row's identity is independent of its current position in any presentation.
// member distinguishes folder members or capture segments within one incarnation.
struct LogRowId {
    enum class Origin : std::uint8_t { IndexedFile, Capture, Concatenated, Memory };
    Origin origin = Origin::Memory;
    uint64_t incarnation = 0;
    uint64_t member = 0;
    uint64_t row = 0;

    bool operator==( const LogRowId& other ) const
    {
        return origin == other.origin && incarnation == other.incarnation
               && member == other.member && row == other.row;
    }
    bool operator!=( const LogRowId& other ) const { return !( *this == other ); }
    bool operator<( const LogRowId& other ) const
    {
        if ( origin != other.origin ) return origin < other.origin;
        if ( incarnation != other.incarnation ) return incarnation < other.incarnation;
        if ( member != other.member ) return member < other.member;
        return row < other.row;
    }
};

enum class SnapshotScope : std::uint8_t { PresentedRows, Matches };

struct SnapshotRow {
    LogRowId identity;
    // One-based source label, independent of zero-based identity and view ordinals.
    LineNumber sourceLineNumber;
    bool copyable = true;
    bool match = false;
};

class SnapshotError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Implementations own immutable content/row membership and frozen text settings.
// Readers may run off the GUI thread. They must not call back into a mutable
// AbstractLogData or convert read failures into warning strings masquerading as data.
class LogSnapshot {
  public:
    using Ptr = std::shared_ptr<const LogSnapshot>;
    virtual ~LogSnapshot() = default;
    virtual LinesCount lineCount() const = 0;
    virtual SnapshotRow row( LineNumber index ) const = 0;
    virtual QString lineString( LineNumber index ) const = 0;
    // A positive proof that former source identities keep their interpretation
    // and presented positions, including identities that were hidden. Only newly
    // appended source identities may appear; unknown means false.
    virtual bool preservesPresentationOf( const LogSnapshot& ) const { return false; }
    virtual OptionalLineNumber rowForIdentity( const LogRowId& identity ) const
    {
        const LineNumber candidate{ identity.row };
        return identity.row < lineCount().get() && row( candidate ).identity == identity
                   ? OptionalLineNumber{ candidate } : OptionalLineNumber{};
    }
    // Presented-row adapters may omit synthetic headers while retaining their
    // original view positions. This mapping is frozen, never a mutable UI callback.
    virtual OptionalLineNumber presentedRowForIdentity( const LogRowId& identity ) const
    {
        return rowForIdentity( identity );
    }
};

} // namespace klogg

#endif
