#ifndef KLOGG_SEARCHPREPARATION_H
#define KLOGG_SEARCHPREPARATION_H

#include <cstdint>
#include <optional>

#include "logfiltereddataworker.h"
#include "searchsettings.h"

// Value-owned input: no mutable source, QObject owner, or preference references.
// Backend is explicit; execution settings are captured before dispatch.
struct FullSearchPreparationRequest {
    SearchableLogData::ReadSnapshot::Ptr source;
    RegularExpressionPattern pattern;
    SearchSettings settings;
    LineNumber startLine = 0_lnum;
    std::optional<LineNumber> endLine;
};

struct SearchPreparation {
    enum class Status : std::uint8_t { Ready, Cancelled, InvalidInput, CompileFailure, Failed };
    Status status = Status::InvalidInput;
    CompletedSearchResult::Ptr result;
};

// Worker-only, synchronous preparation. Return occurs after native search tasks
// have drained. Only Ready carries a complete result; nothing is published and
// publication identities remain zero. Cancellation is cooperative at native
// chunk boundaries; the caller must keep the flag alive until this call returns.
SearchPreparation prepareFullSearch( FullSearchPreparationRequest request, AtomicFlag& cancelled );

#endif
