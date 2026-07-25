/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KLOGG_HIGHLIGHTCOMPOSE_H
#define KLOGG_HIGHLIGHTCOMPOSE_H

#include "containers.h"
#include "highlightedmatch.h"
#include "linetypes.h"

#include <QColor>

// Logical highlight layers for a single rendered line, in ASCENDING priority:
// a higher layer wins the fill on an overlap. Encoding the priority explicitly
// (rather than as the implicit order of addMatches() calls in drawTextArea) makes
// the z-order contract independent of how sources are gathered and immune to
// refactors that reshuffle call sites.
enum class HighlightLayer {
    Ansi,             // embedded ANSI color spans (lowest)
    SearchMatch,      // the gray "highlight matches" search-match overlay
    Highlighter,      // the configured HighlighterSet rules
    QuickHighlighter, // quick highlighters / color labels
    QuickFind,        // QuickFind pattern
    Selection,        // the active text selection (highest)
};

// A highlight source for composeLineHighlights(): a layer plus the matches that
// source produced against the (already-untabified) line. Matches may overlap
// across sources; composeLineHighlights() resolves overlaps by priority plus the
// search-match/highlighter blend rule.
struct HighlightSource {
    HighlightLayer layer;
    klogg::vector<HighlightedMatch> matches;
};

// Weight of the search-match gray when it is blended over a configured
// Highlighter / QuickHighlighter on an overlap. Alpha compositing:
//   out = gray * alpha + highlighter * (1 - alpha)
// so the highlighter stays dominant (1 - alpha) while the gray stays visible.
constexpr double kSearchMatchBlendAlpha = 0.35;

// Blend the search-match gray over a highlighter background. If either color is
// invalid (a source that sets only a foreground), the valid color wins unchanged.
QColor blendSearchMatchOverHighlighter( const QColor& highlighterBack,
                                        const QColor& searchMatchBack );

// Compose per-source highlight matches into a single non-overlapping
// HighlightedMatchRanges for one line, applying the documented priority and the
// search-match/highlighter blend rule:
//   - Higher layer wins the fill
//     (Ansi < SearchMatch < Highlighter < QuickHighlighter < QuickFind < Selection).
//   - Where SearchMatch overlaps Highlighter or QuickHighlighter, the fill is
//     blendSearchMatchOverHighlighter(highlighterBack, grayBack) so BOTH the
//     highlighter color and the search match stay visible instead of one masking
//     the other. foreColor is taken from the highest layer.
//   - All other overlaps: the higher layer wins at full opacity (e.g. Selection
//     and QuickFind still fully cover a search match).
// Sources must already be in the same (untabified) column space. The result is
// sorted by start column with adjacent same-color segments merged.
HighlightedMatchRanges composeLineHighlights( const klogg::vector<HighlightSource>& sources );

#endif // KLOGG_HIGHLIGHTCOMPOSE_H
