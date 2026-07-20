/*
 * Copyright (C) 2021 Anton Filimonov and other contributors
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
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KLOGG_COLORLABELSMANAGER_H
#define KLOGG_COLORLABELSMANAGER_H

#include "abstractlogview.h"
#include <QStringList>
#include <optional>
#include <qobject.h>
#include <qobjectdefs.h>
#include <vector>

class ColorLabelsManager {
  public:
    using QuickHighlightersCollection = std::vector<AbstractLogView::QuickHighlighters>;

    // Cap on the number of distinct texts labelled in one bulk action (e.g. a
    // multi-line selection). Every entry becomes one regex evaluated per drawn
    // line per paint, so unbounded bulk labelling would degrade painting;
    // oversized selections are truncated with a LOG_WARNING. (Marks have no
    // such cap: they are line-index flags, not per-line regexes.)
    static constexpr size_t MaxBulkLabelTexts = 1000;

    QuickHighlightersCollection colorLabels() const;

    // The QStringList overloads apply the operation to each distinct
    // (non-empty) text; the QString overloads are the single-text case.
    QuickHighlightersCollection setColorLabel( size_t label, const QString& text,
                                               QuickHighlighterDefaults defaults );
    QuickHighlightersCollection setColorLabel( size_t label, const QStringList& texts,
                                               QuickHighlighterDefaults defaults );
    // Cycles ONCE per call: every text lands under the single next label.
    QuickHighlightersCollection setNextColorLabel( const QString& text,
                                                   QuickHighlighterDefaults defaults );
    QuickHighlightersCollection setNextColorLabel( const QStringList& texts,
                                                   QuickHighlighterDefaults defaults );
    // Removes every entry matching ANY of the texts.
    QuickHighlightersCollection removeColorLabel( const QString& text );
    QuickHighlightersCollection removeColorLabel( const QStringList& texts );

    std::optional<size_t> currentColorLabelForText( const QString& text ) const;

    QuickHighlightersCollection clear();

  private:
    QuickHighlightersCollection updateColorLabel( size_t label, const QString& text,
                                                  QuickHighlighterDefaults defaults );
    // The label a cycle action targets for the given reference text, honoring
    // the useInCycle configuration and the current cycle position.
    std::optional<size_t> nextCycleLabel( const QString& referenceText ) const;
    // Drop empties, dedup preserving order, truncate to MaxBulkLabelTexts.
    static QStringList sanitizeBulkTexts( const QStringList& texts );

    QuickHighlightersCollection quickHighlighters_ = QuickHighlightersCollection{9};
    std::optional<size_t> currentLabel_;
};

#endif
