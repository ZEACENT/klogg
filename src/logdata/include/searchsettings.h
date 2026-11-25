#ifndef KLOGG_SEARCHSETTINGS_H
#define KLOGG_SEARCHSETTINGS_H

#include <cstdint>

#include "configuration.h"

// Capture on the request owner's thread, never in a dispatched operation.
struct SearchSettings {
    int readBufferSizeLines = 10000;
    uint32_t matchingThreads = 1;
    bool useBlockScan = true;
    RegexpEngine backend = RegexpEngine::QRegularExpression;

    static SearchSettings capture();
};

#endif
