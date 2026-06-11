/*
 * Copyright (C) 2014 Nicolas Bonnefon and other contributors
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

#ifndef TABBEDCRAWLERWIDGET_H
#define TABBEDCRAWLERWIDGET_H

#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <qhash.h>
#include <qobjectdefs.h>
#include <qtabbar.h>
#include <qwidget.h>

#include "loadingstatus.h"
#include "tabgroup.h"

class QMenu;
class QPaintEvent;

// Connection status of a live log stream tab, used to color the dot indicator.
enum class LiveTabStatus {
    None, // Normal file tab (no live source)
    Connected, // Live stream connected
    Disconnected, // Live stream disconnected
    Error // Live stream error
};

// This class represents glogg's main widget, a tabbed
// group of CrawlerWidgets.
// This is a very slightly customised QTabWidget, with
// a particular style.

class CrawlerTabBar : public QTabBar {
  Q_OBJECT

  public:
    explicit CrawlerTabBar( QWidget* parent = nullptr );
    ~CrawlerTabBar() override;

  Q_SIGNALS:
    void showTabContextMenu( int tab, QPoint point );
    void tabDragStarted();
    void tabDragFinished();

  protected:
    void mousePressEvent( QMouseEvent* ) override;
    void mouseReleaseEvent( QMouseEvent* ) override;
    void paintEvent( QPaintEvent* event ) override;
    void resizeEvent( QResizeEvent* event ) override;
    void showEvent( QShowEvent* event ) override;

  private Q_SLOTS:
    void handleTabMoved( int from, int to );

  private:
    void syncTabButtonGeometry();
    void scheduleTabButtonGeometrySync();
    void updateShapeMask();

    bool leftButtonPressed_ = false;
    bool tabMovedWhilePressed_ = false;
    QTimer syncGeometryTimer_;
};

class TabbedCrawlerWidget : public QTabWidget {
    Q_OBJECT
  public:
    TabbedCrawlerWidget();

    template <typename T>
    int addCrawler( T* crawler, const QString& documentId, const QString& displayName = {},
                    const QString& toolTip = {} )
    {
        const auto index = QTabWidget::addTab( crawler, QString{} );

        connect( crawler, &T::dataStatusChanged, this,
                 [ this, documentId ]( DataStatus status ) {
                     const auto tabsCount = count();
                     for ( int i = 0; i < tabsCount; ++i ) {
                         if ( tabPathAt( i ) == documentId ) {
                             setTabDataStatus( i, status );
                             return;
                         }
                     }
                 } );

        addTabBarItem( index, documentId, displayName, toolTip );

        return index;
    }

    void removeCrawler( int index );
    void updateCrawler( int index, const QString& displayName, const QString& toolTip );
    void selectNextTab();
    void selectPreviousTab();

    // Set the live connection status (icon color) for the tab number 'index'
    void setLiveTabStatus( int index, LiveTabStatus status );

    // Set the data status (icon) for the tab number 'index'
    void setTabDataStatus( int index, DataStatus status );

    static QIcon generateColoredDotIcon( LiveTabStatus liveStatus, DataStatus dataStatus );

  Q_SIGNALS:
    void tabsReordered();

  protected:
    void keyPressEvent( QKeyEvent* event ) override;
    void mouseReleaseEvent( QMouseEvent* event ) override;
    void changeEvent( QEvent* event ) override;

  private:
    void addTabBarItem( int index, const QString& documentId, const QString& displayName,
                        const QString& toolTip );
    QString tabPathAt( int index ) const;
    int tabIndexForPath( const QString& tabPath ) const;
    void setTabVisibleCompat( int index, bool visible );
    void handleTabMoved( int from, int to );
    void handleTabDragFinished();
    void handleDragSettleTimeout();
    void applyPendingDragGrouping();
    QString resolveDropTargetGroupId( int droppedIndex, const QString& droppedTabPath,
                                      const QString& currentGroupId ) const;
    QString resolveCollapsedAnchorPath( const QString& groupId, const TabGroup& group );
    bool updateGroupChip( int tabIndex, const TabGroup* group );
    void clearGroupChip( int tabIndex );
    void populateGroupActions( QMenu* menu, const QString& groupId );

    void updateTabBarStyle();
    void loadIcons();
    void updateIcon( int index );

  public Q_SLOTS:
    void onGroupsChanged();

  private Q_SLOTS:
    void showContextMenu( int tab, QPoint globalPoint );

  private:
    void buildGroupSubmenu( QMenu* menu, int tabIndex );
    void createNewGroupDialog( int tabIndex );

  private:
    QIcon olddata_icon_;
    QIcon newdata_icon_;
    QIcon newfiltered_icon_;

    // Colored icons for live stream tabs: [liveStatus][dataStatus]
    QIcon live_icons_[ 4 ][ 3 ];

    QString draggedTabPath_;
    bool tabDragInProgress_ = false;
    bool pendingGroupsRefresh_ = false;
    bool reorderChangedDuringDrag_ = false;
    QTimer dragSettleTimer_;
    QHash<QString, QString> collapsedAnchorByGroup_;
    QHash<QString, bool> collapsedStateByGroup_;

    CrawlerTabBar myTabBar_;
};

#endif
