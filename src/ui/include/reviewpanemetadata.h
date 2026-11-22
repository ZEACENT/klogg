#ifndef KLOGG_REVIEWPANEMETADATA_H
#define KLOGG_REVIEWPANEMETADATA_H

#include "linetypes.h"
#include "regularexpressionpattern.h"

namespace klogg {

// Query attribution belongs to the result model, not the toolbar draft or a
// deferred view-paint pattern. Completion is native result readiness, not a
// running/stopped/error classification or a promise that a content lease exists.
struct ReviewAppliedFilter {
    RegularExpressionPattern pattern;
    bool complete = false;
};

// Detached owner-thread observation, not a source address or a content lease.
// Counts/ordinals describe the pane's current source model, including synthetic
// folder headers; they need not match a provider snapshot's row indices.
struct ReviewPaneMetadata {
    // Unavailable panes have no selected source; remaining fields are defaults.
    bool available = false;
    LinesCount sourceRowCount = 0_lcount;
    // Stored scroll anchor, absent for an empty source. It may lag a source
    // update: observation never scrolls or rebuilds the visible-line map.
    OptionalLineNumber topPresentedRow;
    bool follow = false;
    bool wrap = false;
    // Qt widget state only, not a guarantee about painted rows or source identity.
    bool visible = false;
    bool focused = false;
};

} // namespace klogg

#endif
