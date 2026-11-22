/*
 * Copyright (C) 2009, 2010 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2019 Anton Filimonov and other contributors
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

#ifndef highlighterSet_H
#define highlighterSet_H

#include <cstdint>

#include <cstddef>
#include <QColor>
#include <QList>
#include <QMetaType>
#include <QRegularExpression>
#include <memory>
#include <optional>
#include <qcolor.h>
#include <qregularexpression.h>

#include "containers.h"
#include "highlightedmatch.h"
#include "persistable.h"
#include "regularexpression.h"
#include "regularexpressionpattern.h"

struct HighlightColor {
    QColor foreColor;
    QColor backColor;
};

// Represents a filter, i.e. a regexp and the colors matching text
// should be rendered in.
class Highlighter {
  public:
    // Construct an uninitialized Highlighter (when reading from a config file)
    Highlighter() = default;
    Highlighter( const QString& pattern, bool ignoreCase, bool onlyMatch, const QColor& foreColor,
                 const QColor& backColor );

    bool matchLine( const QString& line, klogg::vector<HighlightedMatch>& matches ) const;

    // Accessor functions
    QString pattern() const;
    void setPattern( const QString& pattern );
    bool ignoreCase() const;
    void setIgnoreCase( bool ignoreCase );

    bool highlightOnlyMatch() const;
    void setHighlightOnlyMatch( bool onlyMatch );

    bool useRegex() const;
    void setUseRegex( bool useRegex );

    bool variateColors() const;
    void setVariateColors( bool variateColors );

    int colorVariance() const;
    void setColorVariance( int colorVariance );

    const QColor& foreColor() const;
    void setForeColor( const QColor& foreColor );
    const QColor& backColor() const;
    void setBackColor( const QColor& backColor );

    // Reads/writes the current config in the QSettings object passed
    void saveToStorage( QSettings& settings ) const;
    void retrieveFromStorage( QSettings& settings );

    RegularExpressionPattern expressionPattern() const;

    const QRegularExpression& compile() const;

    // Semantic value equality; compiled matcher caches are not application state.
    bool operator==( const Highlighter& other ) const;
    bool operator!=( const Highlighter& other ) const { return !( *this == other ); }

  private:
    std::pair<QColor, QColor> vairateColors( const QString& match ) const;

  private:
    QRegularExpression regexp_;
    
    mutable std::optional<QRegularExpression> optimizedRegexp_;

    bool useRegex_ = true;
    bool highlightOnlyMatch_ = false;
    bool variateColors_ = false;
    int colorVariance_ = 15;

    HighlightColor color_;
};

enum class HighlighterMatchType { NoMatch, WordMatch, LineMatch };

// Represents an ordered set of filters to be applied to each line displayed.
class HighlighterSet {
  public:
    static const char* persistableName()
    {
        return "HighlighterSet";
    }

    static HighlighterSet createNewSet( const QString& name );
    static HighlighterSet fromDescription( QString id, QString name, QList<Highlighter> rules );

    HighlighterSet() = default;

    QString name() const;
    QString id() const;
    QList<Highlighter> highlighters() const;

    bool operator==( const HighlighterSet& other ) const;
    bool operator!=( const HighlighterSet& other ) const { return !( *this == other ); }

    // Returns weither the passed line match a filter of the set,
    // if so, it returns the fore/back colors the line should use.
    HighlighterMatchType matchLine( const QString& line,
                                    HighlightedMatchRanges& matches ) const;

    bool isEmpty() const;

    // Reads/writes the current config in the QSettings object passed
    void saveToStorage( QSettings& settings ) const;
    void retrieveFromStorage( QSettings& settings );

    void compile() const;
    void compile( RegexpEngine engine ) const;

  private:
    explicit HighlighterSet( const QString& name );

  private:
    static constexpr int HighlighterSet_VERSION = 3;
    static constexpr int FilterSet_VERSION = 2;

    QString name_;
    QString id_;
    QList<Highlighter> highlighterList_;

    // To simplify this class interface, HighlightersDialog can access our
    // internal structure directly.
    friend class HighlighterSetEdit;
    friend class HighlighterSetCollection;

    mutable std::shared_ptr<MultiRegularExpression> compiledExpression_;
};

struct QuickHighlighter {
    QString name;
    HighlightColor color;
    bool useInCycle;

    bool operator==( const QuickHighlighter& other ) const;
    bool operator!=( const QuickHighlighter& other ) const { return !( *this == other ); }
};

struct QuickLabelEntry {
    QString text;
    bool ignoreCase = true;
    bool wholeWord = false;
};

struct QuickHighlighterDefaults {
    bool ignoreCase = true;
    bool wholeWord = false;

    bool operator==( const QuickHighlighterDefaults& other ) const
    {
        return ignoreCase == other.ignoreCase && wholeWord == other.wholeWord;
    }
    bool operator!=( const QuickHighlighterDefaults& other ) const { return !( *this == other ); }
};

class HighlighterSetCollection final : public Persistable<HighlighterSetCollection> {
  public:
    static const char* persistableName()
    {
        return "HighlighterSetCollection";
    }

    HighlighterSetCollection() = default;
    // A copy is an independent owner. Assignment commits semantic content to
    // the recipient, never imports a dialog draft's revision or matcher cache.
    HighlighterSetCollection( const HighlighterSetCollection& other );
    HighlighterSetCollection& operator=( const HighlighterSetCollection& other );

    struct Snapshot {
        uint64_t revision = 0;
        QList<HighlighterSet> sets;
        QStringList activeSetIds;
        QList<QuickHighlighter> quickHighlighters;
        QuickHighlighterDefaults quickHighlighterDefaults;
    };

    // Owner-thread, detached value observation of committed global state. Does
    // not synchronize storage, compile matchers, or publish/persist changes.
    // The revision belongs to this collection instance, not its saved config.
    Snapshot snapshot() const;
    // Admit control-plane reads before detaching potentially large GUI imports.
    std::optional<Snapshot> boundedSnapshot() const;
    uint64_t revision() const { return revision_; }

    enum class UpdateResult { Applied, Unchanged, Conflict, InvalidArgument, ResourceLimit };
    static constexpr std::size_t MaxPreparedSets = 128;
    static constexpr std::size_t MaxPreparedRules = 1024;
    static constexpr std::size_t MaxSnapshotPalette = 128;
    static constexpr std::size_t MaxPreparedTextUnits = 1024 * 1024;
    class PreparedUpdate {
      public:
        PreparedUpdate( PreparedUpdate&& other ) noexcept;
        PreparedUpdate& operator=( PreparedUpdate&& other ) noexcept;
        PreparedUpdate( const PreparedUpdate& ) = delete;
        PreparedUpdate& operator=( const PreparedUpdate& ) = delete;
        std::optional<UpdateResult> error() const { return error_; }
      private:
        friend class HighlighterSetCollection;
        PreparedUpdate() = default;
        Snapshot state_;
        HighlighterSet combined_;
        std::optional<UpdateResult> error_;
    };
    // Pure private preparation: callers capture the backend on the owner thread
    // and may build this sealed value on a worker. Installation never compiles.
    static std::optional<UpdateResult> validateUpdate( const Snapshot& state );
    static PreparedUpdate prepareUpdate( Snapshot state, RegexpEngine engine );
    UpdateResult applyPrepared( PreparedUpdate&& prepared, uint64_t expectedRevision );

    QList<HighlighterSet> highlighterSets() const;
    void setHighlighterSets( const QList<HighlighterSet>& highlighters );

    const HighlighterSet& currentActiveSet() const;

    bool hasSet( const QString& setId ) const;
    bool hasSetByName( const QString& name ) const;

    QStringList activeSetIds() const;
    void activateSet( const QString& setId );
    void deactivateSet( const QString& setId );
    void deactivateAll();

    QList<QuickHighlighter> quickHighlighters() const;
    void setQuickHighlighters( const QList<QuickHighlighter>& quickHighlighters );

    QuickHighlighterDefaults quickHighlighterDefaults() const;
    void setQuickHighlighterDefaults( QuickHighlighterDefaults quickHighlighterDefaults );

    // Reads/writes the current config in the QSettings object passed
    void saveToStorage( QSettings& settings ) const;
    void retrieveFromStorage( QSettings& settings );

  private:
    // Only the owner-thread coordinator may separate approval from installation,
    // within one non-yielding publication and with no intervening global writes.
    friend class ReviewTransaction;
    UpdateResult preflightPrepared( const PreparedUpdate& prepared, uint64_t expectedRevision ) const;
    void installApproved( PreparedUpdate&& prepared, UpdateResult approval ) noexcept;
    static constexpr int HighlighterSetCollection_VERSION = 3;
    static bool withinSnapshotLimits( const Snapshot& state );

    void updateCombinedSet();
    void loadFromStorage( QSettings& settings );

  private:
    uint64_t revision_ = 0;
    QList<HighlighterSet> highlighters_;
    QStringList activeSets_;
    HighlighterSet combinedActiveSet_;

    QList<QuickHighlighter> quickHighlighters_;
    QuickHighlighterDefaults quickHighlighterDefaults_;

    // To simplify this class interface, HighlightersDialog can access our
    // internal structure directly.
    friend class HighlightersDialog;
};

#endif
