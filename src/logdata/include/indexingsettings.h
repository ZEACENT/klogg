#ifndef KLOGG_INDEXINGSETTINGS_H
#define KLOGG_INDEXINGSETTINGS_H

#include <cstddef>

// Capture on the owner thread before dispatching indexing. Workers consume these
// values rather than reading preferences while the human edits them.
struct IndexingSettings {
    int defaultEncodingMib = -1;
    std::size_t prefetchBlockCount = 1;
    bool compressedIndex = true;
    bool fastModificationDetection = true;

    static IndexingSettings capture();
};

#endif
