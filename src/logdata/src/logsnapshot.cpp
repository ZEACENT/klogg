#include "logsnapshot.h"

#include <utility>

namespace klogg {
BoundedSnapshotLine LogSnapshot::readLineBounded( LineNumber index, uint64_t maxSourceBytes,
                                                 uint64_t maxTextUnits ) const
{
    if ( index >= lineCount() ) {
        throw SnapshotError( "Snapshot row is out of bounds" );
    }
    const auto bytes = sourceBytesForLine( index );
    if ( !bytes ) {
        throw SnapshotReadUnsupported( "Snapshot has no bounded record extent" );
    }
    if ( *bytes > maxSourceBytes ) {
        throw SnapshotLimitError( "Snapshot record exceeds source-byte budget" );
    }
    auto text = lineString( index );
    if ( static_cast<uint64_t>( text.size() ) > maxTextUnits ) {
        throw SnapshotLimitError( "Snapshot record exceeds decoded-text budget" );
    }
    return { std::move( text ), *bytes };
}
} // namespace klogg
