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
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KLOGG_VIEWSIGNALWIRING_H
#define KLOGG_VIEWSIGNALWIRING_H

#include <functional>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>

#include "linetypes.h"

class AbstractLogView;
class SearchToolbar;

// Shared wiring for the context-menu / view signals every crawler-style tab
// must honor, so CrawlerWidget and FolderCrawlerWidget cannot drift (the
// folder mode shipped with these signals connected to nothing -- dead menu
// entries for scratchpad, search composition, splitter save, font zoom,
// exitView, highlightersChange, hover-highlight).
//
// Behavior splits into two kinds:
//  - Fully shared, implemented here: add/exclude/replace-search composition
//    (via the host's SearchToolbar) and Ctrl+wheel font zoom (Configuration
//    font stepping fanned out to every registered view).
//  - Host-specific, injected as Hooks: scratchpad forwarding, splitter-size
//    persistence, exitView focus swap, configuration re-apply, and hover
//    highlight. A null hook means the signal is left unconnected (capability
//    opt-out).
//
// Usage: the host constructs one ViewSignalWiring and calls wireView() for
// every view it creates (main + each filtered/results view), plus wireHover()
// for filtered/results views only (mirroring single-file, where hovering the
// filtered view's margin highlights the match in the minimap).
class ViewSignalWiring : public QObject {
    Q_OBJECT

  public:
    struct Hooks {
        // Forward the emitting view's selection to the scratchpad.
        std::function<void( const QString& )> sendToScratchpad;
        std::function<void( const QString& )> replaceScratchpad;
        // Persist the host splitter's sizes as the default.
        std::function<void()> saveSplitterSizes;
        // Move focus to the host's other view (Space).
        std::function<void( AbstractLogView* fromView )> exitView;
        // Re-apply Configuration (view-menu Highlighters edits take effect).
        std::function<void()> applyConfiguration;
        // Highlight the hovered line in the minimap / clear the highlight.
        std::function<void( AbstractLogView*, LineNumber )> hoveredOverLine;
        std::function<void()> leftHoveringZone;
    };

    ViewSignalWiring( QObject* parent, SearchToolbar* searchToolbar, Hooks hooks );

    // Connect the shared signal set for one host view and register it for font
    // fan-out. Call for every view the host creates (QPointer-guarded against
    // views destroyed later, e.g. closed keep-results panes).
    void wireView( AbstractLogView* view );
    // Additionally connect the hover -> minimap signals (filtered/results
    // views only).
    void wireHover( AbstractLogView* view );

  private:
    void addToSearch( const QString& selection );
    void excludeFromSearch( const QString& selection );
    void replaceSearch( const QString& selection );
    void changeFontSize( bool increase );

    SearchToolbar* searchToolbar_;
    Hooks hooks_;
    std::vector<QPointer<AbstractLogView>> views_;
};

#endif // KLOGG_VIEWSIGNALWIRING_H
