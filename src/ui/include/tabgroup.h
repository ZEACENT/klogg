/*
 * Copyright (C) 2024 Anton Filimonov and other contributors
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

#ifndef TABGROUP_H
#define TABGROUP_H

#include <cstdint>
#include <memory>

#include <QColor>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUuid>

#include "persistable.h"

struct TabGroup {
    QString id;
    QString name;
    QColor color;
    bool collapsed = false;
    QStringList tabPaths;

    TabGroup() = default;
    TabGroup( const QString& groupName, const QColor& groupColor )
        : id( QUuid::createUuid().toString( QUuid::WithoutBraces ) )
        , name( groupName )
        , color( groupColor )
    {
    }

    bool operator==( const TabGroup& other ) const
    {
        return id == other.id;
    }
};

class TabGroupManager : public QObject, public Persistable<TabGroupManager> {
    Q_OBJECT

  public:
    static const char* persistableName()
    {
        return "TabGroupManager";
    }

    TabGroupManager() = default;

    QList<TabGroup> groups() const;
    const TabGroup* groupById( const QString& groupId ) const;
    QString groupIdForTab( const QString& tabPath ) const;
    const TabGroup* groupForTab( const QString& tabPath ) const;

    void createGroup( const QString& name, const QColor& color );
    void deleteGroup( const QString& groupId );
    void renameGroup( const QString& groupId, const QString& newName );
    void setGroupColor( const QString& groupId, const QColor& color );
    void toggleCollapsed( const QString& groupId );

    void addTabToGroup( const QString& groupId, const QString& tabPath );
    void removeTabFromGroup( const QString& tabPath );
    void moveTabToGroup( const QString& tabPath, const QString& newGroupId );

    void ungroupAll( const QString& groupId );

    // Private value preparation, silent one-shot install, then explicit notify.
    // A workspace coordinator reconciles every tab facade/inventory between
    // install and notify. Discarding an installed update does not roll it back.
    class PreparedRemoval {
      public:
        ~PreparedRemoval();
        PreparedRemoval( const PreparedRemoval& ) = delete;
        PreparedRemoval& operator=( const PreparedRemoval& ) = delete;
        bool isCurrent() const noexcept;
        bool install() noexcept;
        void notify();
      private:
        friend class TabGroupManager;
        struct State;
        explicit PreparedRemoval( std::unique_ptr<State> state );
        std::unique_ptr<State> state_;
    };
    std::unique_ptr<PreparedRemoval> prepareTabRemovals( const QStringList& paths );

    void retrieveFromStorage( QSettings& settings );
    void saveToStorage( QSettings& settings ) const;

  Q_SIGNALS:
    void groupsChanged();
    void groupCollapsedChanged( const QString& groupId, bool collapsed );

  private:
    TabGroup* mutableGroupById( const QString& groupId );
    void advanceMutationIdentity();
    QList<TabGroup> groups_;
    // Retained identity catches ABA without a wrapping public counter. Only
    // revision-owning commands may mutate groups; lookup results are read-only.
    std::shared_ptr<const uint8_t> mutationIdentity_ = std::make_shared<const uint8_t>( 0 );
};

Q_DECLARE_METATYPE( TabGroup )

#endif // TABGROUP_H
