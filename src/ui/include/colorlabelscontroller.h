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

#ifndef KLOGG_COLORLABELSCONTROLLER_H
#define KLOGG_COLORLABELSCONTROLLER_H

#include <cstdint>
#include <functional>
#include <optional>
#include <map>
#include <utility>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "colorlabelsmanager.h"

class AbstractLogView;
class QWidget;
class QShortcut;
class ReviewTransaction;

// Owns one tab's ColorLabelsManager and keeps the quick highlighters of every
// view the tab hosts in sync with it.
//
// The AbstractLogView base builds the "Color labels" context menu and emits
// addColorLabel/removeColorLabel/clearColorLabels/quickColorLabelDefaultsChanged
// for ANY view, but nothing happens unless the host widget connects those
// signals to a manager and pushes the resulting quick highlighters back into
// the views with setQuickHighlighters. CrawlerWidget used to do that by hand
// (and FolderCrawlerWidget did not, leaving folder-mode color labels dead);
// this component is the single shared implementation so every tab kind gets
// identical behavior by composition.
//
// Usage: the host constructs one controller, calls watchView() for every view
// it hosts (including late-created ones, e.g. keep-results panes -- they are
// seeded with the current labels), and calls registerShortcuts() from its own
// registerShortcuts(). The shortcuts (1..9 add, 0 remove, Cmd/Ctrl+D next,
// Cmd/Ctrl+Shift+0 clear) are registered with Qt::WidgetWithChildrenShortcut on
// the host widget, exactly as CrawlerWidget did.
class ColorLabelsController : public QObject {
    Q_OBJECT

  public:
    // shortcutsParent: the tab widget the label shortcuts are parented to and
    // whose children they are active for. activeViewProvider resolves the view
    // whose selection a shortcut-triggered label applies to (the host's
    // activeView()); signal-triggered labels always use the emitting view.
    ColorLabelsController( QWidget* shortcutsParent,
                           std::function<AbstractLogView*()> activeViewProvider );

    struct Snapshot {
        uint64_t revision = 0;
        ColorLabelsManager::QuickHighlightersCollection labels;
    };

    struct Edit {
        enum class Kind : uint8_t { Set, Remove, Clear };
        Kind kind = Kind::Set;
        size_t label = 0; // zero-based palette index; used only by Set
        QStringList texts;
        QuickHighlighterDefaults defaults;
    };

    enum class EditResult : uint8_t { Applied, Unchanged, Conflict, InvalidArgument, ResourceLimit };
    static constexpr size_t MaxEdits = 128;
    static constexpr size_t MaxEditTextUnits = size_t{ 1024 } * 1024;

    // Owner-thread value observation; neither reads selection nor changes focus.
    Snapshot snapshot() const;
    // Validate the entire batch, prepare with the existing manager, then publish
    // once. This is one tab's label scope, not a workspace-wide transaction.
    EditResult apply( const std::vector<Edit>& edits,
                      std::optional<uint64_t> expectedRevision = {} );

    // Connect the view's color-label signals and seed it with the labels
    // already set (no-op labels for the first view). Safe for views that are
    // destroyed later: QPointer guards drop them from the sync set.
    void watchView( AbstractLogView* view );

    // (Re-)register the widget-level label shortcuts from Configuration.
    // Re-entrant: previously created shortcuts are destroyed first, mirroring
    // CrawlerWidget::registerShortcuts.
    void registerShortcuts();

  Q_SIGNALS:
    // Emitted only after the owner and every live view have the committed state.
    void labelsChanged();

  private:
    friend class ReviewTransaction;
    struct PreparedPublication {
        ColorLabelsManager state;
        std::vector<std::pair<QPointer<AbstractLogView>,
                              ColorLabelsManager::QuickHighlightersCollection>> views;
        bool changed = false;
    };
    static std::optional<EditResult> validateEdits( const std::vector<Edit>& edits,
                                                   size_t labelCount, size_t& textUnits,
                                                   size_t& textCount );
    PreparedPublication preparePublication( ColorLabelsManager state ) const;
    void install( PreparedPublication prepared );
    static void applyEdit( ColorLabelsManager& manager, const Edit& edit );
    void applyGuiEdit( const Edit& edit );
    EditResult publish( ColorLabelsManager prepared );
    void addColorLabelToSelection( size_t label, const AbstractLogView* sourceView );
    void addNextColorLabelToSelection( const AbstractLogView* sourceView );
    void removeColorLabelFromSelection( const AbstractLogView* sourceView );
    void clearColorLabels();
    void setQuickColorLabelDefaults( bool ignoreCase, bool wholeWord );
    // The texts to (un)label: one entry per selected line of the emitting view
    // when known, else of the host-active view. Multi-line selections yield
    // per-line entries so every selected line gets labelled (an entry holding
    // the joined multi-line blob could never match a single log line). Empty
    // when there is nothing to do.
    QStringList selectedTexts( const AbstractLogView* sourceView ) const;

    ColorLabelsManager manager_;
    uint64_t revision_ = 0;
    QWidget* shortcutsParent_;
    std::function<AbstractLogView*()> activeViewProvider_;
    std::vector<QPointer<AbstractLogView>> views_;
    std::map<QString, QShortcut*> shortcuts_;
};

#endif // KLOGG_COLORLABELSCONTROLLER_H
