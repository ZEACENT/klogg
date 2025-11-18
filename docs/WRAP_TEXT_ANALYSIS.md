# Wrap Text Analysis and Implementation Details

## Overview
This document analyzes the implementation of "Wrap text" in Klogg, focusing on the `AbstractLogView` and `FilteredView` classes. It details the mechanism for displaying wrapped lines, calculating scroll ranges, and handling bottom alignment, which has been a source of bugs (e.g., last line visibility).

## Implementation Details

### 1. Wrapped Line Calculation
- **File**: `src/ui/src/abstractlogview.cpp`
- **Function**: `getNbBottomWrappedVisibleLines()`
- **Logic**:
  - Iterates backwards from the last line of the log.
  - Uses `WrappedString` to simulate wrapping of each line based on `getNbVisibleCols()`.
  - Accumulates wrapped line counts until they fill `getNbVisibleLines()` (viewport height in characters).
  - Returns the count of *logical* lines that fit in the viewport when wrapped.
  - This count is referred to as `unwrappedLinesAtBottom`.

### 2. Scroll Range
- **Function**: `updateScrollBars()`
- **Logic**:
  - Sets the vertical scrollbar range to `[0, TotalLines - unwrappedLinesAtBottom]`.
  - The maximum scroll value corresponds to the first line of the "last page" of content.

### 3. Bottom Alignment (The "Lock" Mechanism)
- **Goal**: When at the bottom of the log, ensure the last line is flush with the bottom of the viewport, even if the content doesn't exactly fill the viewport height (e.g., due to variable wrapped line heights or fractional line visibility).
- **Mechanism**:
  - **Detection**: In `scrollContentsBy()`, if `scrollPosition >= lastTopLine` (where `lastTopLine = Total - unwrappedLinesAtBottom`), the view enters "Bottom Alignment" mode.
  - **State**: `lastLineAligned_` is set to `true`. `firstLine_` is forced to `lastTopLine`.
  - **Rendering**:
    - In `paintEvent()`, if `lastLineAligned_` is true:
      - Calculates `effectiveHeight` (actual pixel height of drawn text).
      - Calculates `drawingTopOffset_ = -(effectiveHeight - viewportHeight)`.
      - Shifts the painting position so the bottom of the text aligns with the bottom of the viewport.
    - In `drawTextArea()`, a special check `!(useTextWrap_ && (isLastLineInFile || lastLineAligned_))` prevents the optimization loop from breaking early, ensuring enough lines are drawn to cover the viewport even after the upward shift.

## Identified Bugs

### 1. Last Line Invisible / Out of Bottom on Resize
- **Symptom**: When the Filter Result Window (or any log view) is resized (specifically, height changed), the last line may disappear or be pushed out of view.
- **Cause**: 
  - `scrollContentsBy()` (which sets `lastLineAligned_` and anchors `firstLine_`) is NOT called during a simple resize event unless the scrollbar value changes.
  - `resizeEvent()` calls `updateDisplaySize()`, which updates scrollbar ranges but does not re-evaluate the `firstLine_` position.
  - When the window grows, `getNbBottomWrappedVisibleLines()` increases, so `lastTopLine` (ideal top line) decreases (moves up).
  - However, `firstLine_` remains at the old, lower value.
  - `paintEvent` draws from the old `firstLine_`. The content is insufficient to fill the new larger viewport.
  - The "Bottom Alignment" shift (`offset`) becomes large positive (or less negative), pushing the text down (or creating a gap at top), potentially misaligning or confusing the user if they expected to see more context.
  - Specifically, if resizing "shorter", the range increases (max value increases). The current scrollbar value (clamped) might be less than the new max. `scrollContentsBy` might see `scrollPosition < lastTopLine` and disable `lastLineAligned_`, causing a switch to Top Alignment which draws `firstLine_` at Y=0. If `firstLine_` was the old `lastTopLine` (for a taller window), it starts too low for the short window, causing the bottom lines to be cut off.

### 2. Static Text / Overshoot
- **Symptom**: Scrolling near the bottom feels sticky or static.
- **Analysis**:
  - The transition between "Bottom Aligned" (shifted layout) and "Top Aligned" (standard layout) can cause visual jumps or discontinuities because `AbstractLogView` scrolls by logical lines, but "Bottom Alignment" adjusts by pixels.
  - If `getNbBottomWrappedVisibleLines` calculation is slightly off (e.g. due to integer division of `visibleLines`), `lastTopLine` might be conservative.
  - If `verticalScrollBar` resolution is low (logical lines), we can't scroll smoothly through a large wrapped line.

## Proposed Fix
To fix the "Last line invisible" issue:
1. Modify `updateDisplaySize()` (called on resize).
2. Detect if the view was previously bottom-aligned (`lastLineAligned_` is true).
3. If so, force the scrollbar value to the new maximum. This triggers `scrollContentsBy`, which recalculates `lastTopLine` and re-anchors `firstLine_` correctly, preserving the bottom alignment.

## Verification Plan
1. Open Filter Result Window (or small log file).
2. Scroll to bottom.
3. Resize window height (make smaller, make larger).
4. Verify last line stays visible and anchored to bottom.
5. Verify scrolling up from bottom works (text moves).
