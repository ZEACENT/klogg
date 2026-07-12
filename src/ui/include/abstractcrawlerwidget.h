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

#ifndef ABSTRACTCRAWLERWIDGET_H
#define ABSTRACTCRAWLERWIDGET_H

#include "viewinterface.h"

#include <optional>

// Common interface for top-level tab document widgets: CrawlerWidget (files and
// live sources) and FolderCrawlerWidget (Open Folder). It declares the
// behavioral hooks MainWindow dispatches uniformly across tab kinds, so a call
// site can hold a single AbstractCrawlerWidget* (obtained via one
// dynamic_cast from QWidget*) instead of qobject_cast-ing to each concrete type
// at every site -- the cast-branch pattern that caused the original folder-tab
// crash (a static_cast<CrawlerWidget*> on a FolderCrawlerWidget tab).
//
// This is a pure mixin and is NOT a QObject. Each concrete widget keeps its own
// QWidget base (QSplitter for CrawlerWidget, QWidget for FolderCrawlerWidget)
// and adds this interface as a second base, exactly as both already multiply
// inherit ViewInterface. Qt forbids two QObject bases, but
// [QObject-descendant] + [non-QObject interface] is the supported pattern.
//
// Hooks default to no-op so introducing the base is a behaviour-neutral
// refactor; concrete widgets override the ones they need (FolderCrawlerWidget
// gains its overrides in later phases).
class AbstractCrawlerWidget : public ViewInterface {
  public:
    // Re-apply Configuration to this tab's views (line numbers, font, overview
    // visibility, text wrap). Called on construction and whenever MainWindow
    // emits optionsChanged (e.g. the View-menu toggles). Default no-op.
    virtual void applyConfiguration() {}

    // Register this tab's view-level shortcuts (keyboard navigation, marks,
    // scratchpad send, color labels). Default no-op.
    virtual void registerShortcuts() {}

    // Return the currently selected text from the active view (Edit -> Copy).
    // Default empty.
    virtual QString getSelectedText() const
    {
        return {};
    }
    // Select all text in the active view (Edit -> Select All). Default no-op.
    virtual void selectAll() {}
    // True if the active view has a partial (non-full-line) selection.
    virtual bool isPartialSelection() const
    {
        return false;
    }
    // Apply the chosen encoding to the displayed document (Edit -> Encoding).
    // std::nullopt means "use the detected encoding". Default no-op.
    virtual void setEncoding( std::optional<int> /*mib*/ )
    {
    }
};

#endif // ABSTRACTCRAWLERWIDGET_H
