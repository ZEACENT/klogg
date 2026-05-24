/*
 * Copyright (C) 2014, 2015 Nicolas Bonnefon and other contributors
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

#include "tabbedcrawlerwidget.h"

#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QSet>
#include <QSizePolicy>
#include <QToolButton>
#include <QVariant>
#include <qobjectdefs.h>
#include <qpoint.h>

#include "crawlerwidget.h"

#include "clipboard.h"
#include "configuration.h"
#include "dispatch_to.h"
#include "iconloader.h"
#include "log.h"
#include "openfilehelper.h"
#include "styles.h"
#include "tabgroup.h"
#include "tabgroupdropresolver.h"
#include "tabnamemapping.h"

namespace {
constexpr QLatin1String PathKey = QLatin1String( "path", 4 );
constexpr QLatin1String TitleKey = QLatin1String( "title", 5 );
constexpr QLatin1String ToolTipKey = QLatin1String( "toolTip", 7 );
constexpr QLatin1String StatusKey = QLatin1String( "status", 6 );
constexpr QLatin1String GroupIdKey = QLatin1String( "groupId", 7 );
constexpr QLatin1String GroupColorKey = QLatin1String( "groupColor", 10 );
constexpr QLatin1String GroupLeaderKey = QLatin1String( "groupLeader", 11 );
constexpr const char GroupChipPropertyKey[] = "kloggGroupChip";

QColor adjustedGroupFillColor( const QColor& groupColor, bool isCurrentTab )
{
    QColor fillColor = groupColor;
    fillColor.setAlpha( isCurrentTab ? 92 : 64 );
    return fillColor;
}

QColor groupChipTextColor( const QColor& groupColor )
{
    const int brightness
        = static_cast<int>( 0.299 * groupColor.red() + 0.587 * groupColor.green()
                            + 0.114 * groupColor.blue() );
    return brightness < 128 ? QColor( Qt::white ) : QColor( Qt::black );
}

QIcon makeGroupColorIcon( const QColor& color )
{
    QPixmap colorIcon( 10, 10 );
    colorIcon.fill( color );
    return QIcon( colorIcon );
}

QString liveStatusSuffix( const QString& title )
{
    QStringList suffixes{
        QCoreApplication::translate( "MainWindow", " [disconnected]" ),
        QCoreApplication::translate( "MainWindow", " [error]" ),
        QStringLiteral( " [disconnected]" ),
        QStringLiteral( " [error]" ),
    };
    suffixes.removeDuplicates();

    for ( const auto& suffix : suffixes ) {
        if ( !suffix.isEmpty() && title.endsWith( suffix ) ) {
            return suffix;
        }
    }

    return {};
}

QString tabLabelWithoutLiveStatus( QString label )
{
    const auto suffix = liveStatusSuffix( label );
    if ( !suffix.isEmpty() ) {
        label.chop( suffix.size() );
    }
    return label;
}

QString tabLabelWithLiveStatus( const QString& label, const QString& storedTitle )
{
    const auto suffix = liveStatusSuffix( storedTitle );
    return suffix.isEmpty() ? label : tabLabelWithoutLiveStatus( label ) + suffix;
}

bool isGroupChipWidget( const QWidget* widget )
{
    return widget != nullptr && widget->property( GroupChipPropertyKey ).toBool();
}

bool shouldUseDarkTabIcons( const QWidget* widget, const Configuration& config )
{
    if ( config.style() == StyleManager::DarkStyleKey
         || config.style() == StyleManager::DarkWindowsStyleKey ) {
        return true;
    }
    if ( !widget ) {
        return false;
    }
    const QColor windowColor = widget->palette().color( QPalette::Window );
    return ( windowColor.red() + windowColor.green() + windowColor.blue() ) <= 384;
}
} // namespace

CrawlerTabBar::CrawlerTabBar( QWidget* parent )
    : QTabBar( parent )
{
    connect( this, &QTabBar::tabMoved, this, &CrawlerTabBar::handleTabMoved );
}

TabbedCrawlerWidget::TabbedCrawlerWidget()
    : QTabWidget()
    , newdata_icon_()
    , newfiltered_icon_()
{
    updateTabBarStyle();

    setTabBar( &myTabBar_ );
    myTabBar_.hide();

    myTabBar_.setContextMenuPolicy( Qt::CustomContextMenu );
    connect( &myTabBar_, &CrawlerTabBar::showTabContextMenu, this,
             &TabbedCrawlerWidget::showContextMenu );
    connect( &myTabBar_, &QTabBar::tabMoved, this, &TabbedCrawlerWidget::handleTabMoved );
    connect( &myTabBar_, &CrawlerTabBar::tabDragStarted, this, [ this ] {
        tabDragInProgress_ = true;
        pendingGroupsRefresh_ = false;
        reorderChangedDuringDrag_ = false;
    } );
    connect( &myTabBar_, &CrawlerTabBar::tabDragFinished, this,
             &TabbedCrawlerWidget::handleTabDragFinished );
    dragSettleTimer_.setSingleShot( true );
    connect( &dragSettleTimer_, &QTimer::timeout, this,
             &TabbedCrawlerWidget::handleDragSettleTimeout );

    // Connect to group manager for tab appearance updates
    auto& groupManager = TabGroupManager::getSynced();
    connect( &groupManager, &TabGroupManager::groupsChanged, this,
             &TabbedCrawlerWidget::onGroupsChanged );

    dispatchToMainThread( [ this ] { loadIcons(); } );
}

void TabbedCrawlerWidget::loadIcons()
{
    IconLoader iconLoader{ this };
    olddata_icon_ = iconLoader.load( "olddata_icon" );
    newdata_icon_ = iconLoader.load( "newdata_icon" );
    newfiltered_icon_ = iconLoader.load( "newfiltered_icon" );
    for ( int tab = 0; tab < count(); ++tab ) {
        updateIcon( tab );
    }
}

void TabbedCrawlerWidget::updateTabBarStyle()
{
    const auto& config = Configuration::get();
    if ( config.style() == StyleManager::ModernKey ) {
        myTabBar_.setStyleSheet( "" );
        return;
    }

    QString tabStyle = "QTabBar::tab { height: 24px; }";
    QString tabCloseButtonStyle = " QTabBar::close-button {\
              height: 12px; width: 12px;\
              subcontrol-origin: padding;\
              subcontrol-position: right;\
              %1}";

    QString backgroundImage;
    QString backgroundHoverImage;

    const bool useDarkIcons = shouldUseDarkTabIcons( this, config );
    if ( useDarkIcons ) {
        backgroundImage = ":/images/icons8-close-window_inverse.svg";
        backgroundHoverImage = ":/images/icons8-close-window-hover_inverse.svg";
    }

#if defined( Q_OS_MAC )
    // work around Qt MacOSX bug missing tab close icons
    // see: https://bugreports.qt.io/browse/QTBUG-61092
    // still broken in document mode in Qt.5.12.2 !!!!
    if ( !useDarkIcons ) {
        backgroundImage
            = ":/qt-project.org/styles/commonstyle/images/standardbutton-closetab-16.png";
        backgroundHoverImage
            = ":/qt-project.org/styles/commonstyle/images/standardbutton-closetab-hover-16.png";
    }
#elif defined( Q_OS_WIN )
    if ( !useDarkIcons && config.style() == StyleManager::FusionKey ) {
        backgroundImage = ":/images/icons8-close-window.svg";
        backgroundHoverImage = ":/images/icons8-close-window-hover.svg";
    }
#endif

    if ( !backgroundImage.isEmpty() ) {
        const QString backgroundImageTemplate = " image: url(%1);";
        QString tabCloseButtonHoverStyle = " QTabBar::close-button:hover { %1 }";
        backgroundImage = backgroundImageTemplate.arg( backgroundImage );
        backgroundHoverImage = backgroundImageTemplate.arg( backgroundHoverImage );
        tabCloseButtonHoverStyle = tabCloseButtonHoverStyle.arg( backgroundHoverImage );
        tabCloseButtonStyle = tabCloseButtonStyle.arg( backgroundImage );
        tabCloseButtonStyle.append( tabCloseButtonHoverStyle );
    }
    else {
        tabCloseButtonStyle = tabCloseButtonStyle.arg( "" );
    }

    myTabBar_.setStyleSheet( tabStyle.append( tabCloseButtonStyle ) );
}

void TabbedCrawlerWidget::changeEvent( QEvent* event )
{
    if ( event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange ) {
        updateTabBarStyle();
        dispatchToMainThread( [ this ] { loadIcons(); } );
    }

    QWidget::changeEvent( event );
}

void TabbedCrawlerWidget::addTabBarItem( int index, const QString& documentId,
                                         const QString& displayName, const QString& toolTip )
{
    const auto tabLabel = displayName.isEmpty() ? QFileInfo( documentId ).fileName() : displayName;
    const auto tabName = TabNameMapping::getSynced().tabName( documentId );
    const auto nativeToolTip
        = toolTip.isEmpty() ? QDir::toNativeSeparators( documentId ) : QDir::toNativeSeparators( toolTip );

    myTabBar_.setTabIcon( index, olddata_icon_ );
    myTabBar_.setTabText( index, tabName.isEmpty() ? tabLabel : tabName );
    myTabBar_.setTabToolTip( index, nativeToolTip );

    QVariantMap tabData;
    tabData[ PathKey ] = documentId;
    tabData[ TitleKey ] = tabLabel;
    tabData[ ToolTipKey ] = nativeToolTip;
    tabData[ StatusKey ] = static_cast<int>( DataStatus::OLD_DATA );

    myTabBar_.setTabData( index, tabData );

    setCurrentIndex( index );

    if ( count() > 1 )
        myTabBar_.show();

    onGroupsChanged();
}

void TabbedCrawlerWidget::removeCrawler( int index )
{
    QTabWidget::removeTab( index );

    if ( count() <= 1 )
        myTabBar_.hide();

    onGroupsChanged();
}

void TabbedCrawlerWidget::updateCrawler( int index, const QString& displayName,
                                         const QString& toolTip )
{
    if ( index < 0 || index >= count() ) {
        return;
    }

    auto tabData = myTabBar_.tabData( index ).toMap();
    tabData[ TitleKey ] = displayName;
    tabData[ ToolTipKey ] = QDir::toNativeSeparators( toolTip );
    myTabBar_.setTabData( index, tabData );
    myTabBar_.setTabToolTip( index, QDir::toNativeSeparators( toolTip ) );

    const auto documentId = tabData.value( PathKey ).toString();
    const auto customName = TabNameMapping::getSynced().tabName( documentId );
    if ( customName.isEmpty() ) {
        myTabBar_.setTabText( index, displayName );
    }
    else {
        myTabBar_.setTabText( index, tabLabelWithLiveStatus( customName, displayName ) );
    }
}

void TabbedCrawlerWidget::mouseReleaseEvent( QMouseEvent* event )
{
    LOG_DEBUG << "TabbedCrawlerWidget::mouseReleaseEvent";

    if ( event->button() == Qt::MiddleButton ) {
        int tab = this->myTabBar_.tabAt( event->pos() );
        if ( -1 != tab ) {
            Q_EMIT tabCloseRequested( tab );
            event->accept();
        }
    }

    event->ignore();
}

QString TabbedCrawlerWidget::tabPathAt( int index ) const
{
    return myTabBar_.tabData( index ).toMap()[ PathKey ].toString();
}

int TabbedCrawlerWidget::tabIndexForPath( const QString& tabPath ) const
{
    if ( tabPath.isEmpty() ) {
        return -1;
    }

    for ( int i = 0; i < count(); ++i ) {
        if ( tabPathAt( i ) == tabPath ) {
            return i;
        }
    }

    return -1;
}

void TabbedCrawlerWidget::setTabVisibleCompat( int index, bool visible )
{
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
    myTabBar_.setTabVisible( index, visible );
#else
    Q_UNUSED( visible );
    myTabBar_.setTabEnabled( index, visible );
#endif
}

void TabbedCrawlerWidget::handleTabMoved( int from, int to )
{
    if ( from < 0 || to < 0 || to >= count() ) {
        return;
    }

    const bool dragActive = tabDragInProgress_ || QApplication::mouseButtons().testFlag( Qt::LeftButton );
    if ( !dragActive ) {
        Q_EMIT tabsReordered();
        return;
    }

    if ( draggedTabPath_.isEmpty() ) {
        draggedTabPath_ = tabPathAt( to );
        if ( draggedTabPath_.isEmpty() ) {
            draggedTabPath_ = tabPathAt( from );
        }
    }

    if ( draggedTabPath_.isEmpty() ) {
        return;
    }

    reorderChangedDuringDrag_ = true;
    tabDragInProgress_ = true;
    dragSettleTimer_.start( 30 );
}

void TabbedCrawlerWidget::handleTabDragFinished()
{
    dragSettleTimer_.stop();
    tabDragInProgress_ = false;

    if ( reorderChangedDuringDrag_ ) {
        applyPendingDragGrouping();
        Q_EMIT tabsReordered();
    }
    else if ( pendingGroupsRefresh_ ) {
        pendingGroupsRefresh_ = false;
        onGroupsChanged();
    }

    draggedTabPath_.clear();
    reorderChangedDuringDrag_ = false;
}

void TabbedCrawlerWidget::handleDragSettleTimeout()
{
    if ( QApplication::mouseButtons().testFlag( Qt::LeftButton ) ) {
        dragSettleTimer_.start( 30 );
        return;
    }

    handleTabDragFinished();
}

QString TabbedCrawlerWidget::resolveDropTargetGroupId( int droppedIndex,
                                                       const QString& droppedTabPath,
                                                       const QString& currentGroupId ) const
{
    auto& groupManager = TabGroupManager::get();
    const auto groupOfNeighbor = [ this, &groupManager,
                                   &droppedTabPath ]( int index ) -> QString {
        if ( index < 0 || index >= count() ) {
            return {};
        }

        const auto neighborPath = tabPathAt( index );
        if ( neighborPath.isEmpty() || neighborPath == droppedTabPath ) {
            return {};
        }

        return groupManager.groupIdForTab( neighborPath );
    };

    const auto leftGroupId = groupOfNeighbor( droppedIndex - 1 );
    const auto rightGroupId = groupOfNeighbor( droppedIndex + 1 );
    return resolveTabDropTargetGroupId( currentGroupId, leftGroupId, rightGroupId );
}

void TabbedCrawlerWidget::applyPendingDragGrouping()
{
    if ( draggedTabPath_.isEmpty() ) {
        if ( pendingGroupsRefresh_ ) {
            pendingGroupsRefresh_ = false;
            onGroupsChanged();
        }
        return;
    }

    const auto droppedIndex = tabIndexForPath( draggedTabPath_ );
    if ( droppedIndex < 0 ) {
        if ( pendingGroupsRefresh_ ) {
            pendingGroupsRefresh_ = false;
            onGroupsChanged();
        }
        return;
    }

    auto& groupManager = TabGroupManager::get();
    const auto previousGroupId = groupManager.groupIdForTab( draggedTabPath_ );
    const auto targetGroupId
        = resolveDropTargetGroupId( droppedIndex, draggedTabPath_, previousGroupId );
    if ( previousGroupId != targetGroupId ) {
        groupManager.moveTabToGroup( draggedTabPath_, targetGroupId );
        groupManager.save();
    }
    else {
        onGroupsChanged();
    }

    if ( pendingGroupsRefresh_ ) {
        pendingGroupsRefresh_ = false;
        onGroupsChanged();
    }
}

void CrawlerTabBar::mousePressEvent( QMouseEvent* mouseEvent )
{
    if ( mouseEvent->button() == Qt::LeftButton ) {
        leftButtonPressed_ = true;
        tabMovedWhilePressed_ = false;
    }

    QTabBar::mousePressEvent( mouseEvent );
}

void CrawlerTabBar::mouseReleaseEvent( QMouseEvent* mouseEvent )
{
    if ( mouseEvent->button() == Qt::RightButton ) {
        int tab = tabAt( mouseEvent->pos() );
        if ( tab != -1 ) {
            Q_EMIT showTabContextMenu( tab, mapToGlobal( mouseEvent->pos() ) );
            mouseEvent->accept();
            return;
        }
    }

    const bool shouldEmitDragFinished
        = mouseEvent->button() == Qt::LeftButton && leftButtonPressed_ && tabMovedWhilePressed_;
    QTabBar::mouseReleaseEvent( mouseEvent );

    if ( mouseEvent->button() == Qt::LeftButton ) {
        leftButtonPressed_ = false;
        tabMovedWhilePressed_ = false;
        if ( shouldEmitDragFinished ) {
            Q_EMIT tabDragFinished();
        }
    }
}

void CrawlerTabBar::handleTabMoved( int from, int to )
{
    Q_UNUSED( from );
    Q_UNUSED( to );

    if ( !leftButtonPressed_ ) {
        return;
    }

    if ( !tabMovedWhilePressed_ ) {
        Q_EMIT tabDragStarted();
    }
    tabMovedWhilePressed_ = true;
}

void CrawlerTabBar::paintEvent( QPaintEvent* event )
{
    QTabBar::paintEvent( event );

    QPainter painter( this );
    painter.setRenderHint( QPainter::Antialiasing, true );

    for ( int i = 0; i < count(); ++i ) {
#if QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 )
        if ( !isTabVisible( i ) ) {
            continue;
        }
#endif
        const auto tabInfo = tabData( i ).toMap();
        if ( !tabInfo.contains( GroupColorKey ) ) {
            continue;
        }

        const QColor groupColor = tabInfo.value( GroupColorKey ).value<QColor>();
        if ( !groupColor.isValid() ) {
            continue;
        }

        const QRect tabRectValue = tabRect( i ).adjusted( 1, 1, -1, -1 );
        if ( tabRectValue.isEmpty() ) {
            continue;
        }

        painter.setPen( Qt::NoPen );
        painter.setBrush( adjustedGroupFillColor( groupColor, i == currentIndex() ) );
        painter.drawRoundedRect( tabRectValue, 3, 3 );
    }
}

void TabbedCrawlerWidget::showContextMenu( int tab, QPoint globalPoint )
{
    QMenu menu( this );
    auto closeThis = menu.addAction( tr( "Close this" ) );
    auto closeOthers = menu.addAction( tr( "Close others" ) );
    auto closeLeft = menu.addAction( tr( "Close to the left" ) );
    auto closeRight = menu.addAction( tr( "Close to the right" ) );
    auto closeAll = menu.addAction( tr( "Close all" ) );
    menu.addSeparator();
    auto copyFullPath = menu.addAction( tr( "Copy full path" ) );
    auto openContainingFolder = menu.addAction( tr( "Open containing folder" ) );
    menu.addSeparator();

    // Tab grouping submenu
    auto groupMenu = menu.addMenu( tr( "Add to Group" ) );
    buildGroupSubmenu( groupMenu, tab );

    const auto tabPath = tabPathAt( tab );
    auto& groupManager = TabGroupManager::get();
    const auto currentGroupId = groupManager.groupIdForTab( tabPath );
    if ( !currentGroupId.isEmpty() ) {
        auto removeFromGroup = menu.addAction( tr( "Remove from Group" ) );
        connect( removeFromGroup, &QAction::triggered, this, [ tabPath, &groupManager ] {
            groupManager.removeTabFromGroup( tabPath );
            groupManager.save();
        } );

        auto* groupActionsMenu = menu.addMenu( tr( "Group" ) );
        populateGroupActions( groupActionsMenu, currentGroupId );
    }

    menu.addSeparator();
    auto renameTab = menu.addAction( tr( "Rename tab" ) );
    auto resetTabName = menu.addAction( tr( "Reset tab name" ) );

    connect( closeThis, &QAction::triggered, [ tab, this ] { Q_EMIT tabCloseRequested( tab ); } );

    connect( closeOthers, &QAction::triggered, [ tabWidget = widget( tab ), this ] {
        while ( count() != 1 ) {
            for ( int i = 0; i < count(); ++i ) {
                if ( i != indexOf( tabWidget ) ) {
                    Q_EMIT tabCloseRequested( i );
                    break;
                }
            }
        }
    } );

    connect( closeLeft, &QAction::triggered, [ tabWidget = widget( tab ), this ] {
        while ( indexOf( tabWidget ) != 0 ) {
            Q_EMIT tabCloseRequested( 0 );
        }
    } );

    connect( closeRight, &QAction::triggered, [ tab, this ] {
        while ( count() > tab + 1 ) {
            Q_EMIT tabCloseRequested( tab + 1 );
        }
    } );

    connect( closeAll, &QAction::triggered, [ this ] {
        while ( count() ) {
            Q_EMIT tabCloseRequested( 0 );
        }
    } );

    if ( tab == 0 ) {
        closeLeft->setDisabled( true );
    }
    else if ( tab == count() - 1 ) {
        closeRight->setDisabled( true );
    }

    connect( copyFullPath, &QAction::triggered, this,
             [ this, tab ] { sendTextToClipboard( tabToolTip( tab ) ); } );

    connect( openContainingFolder, &QAction::triggered, this,
             [ this, tab ] { showPathInFileExplorer( tabToolTip( tab ) ); } );
    openContainingFolder->setEnabled( QFileInfo( tabToolTip( tab ) ).isAbsolute() );

    connect( renameTab, &QAction::triggered, this, [ this, tab, tabPath ] {
        const auto currentName = TabNameMapping::getSynced().tabName( tabPath );
        const auto storedTitle = myTabBar_.tabData( tab ).toMap().value( TitleKey ).toString();
        const auto defaultName
            = currentName.isEmpty()
                  ? tabLabelWithoutLiveStatus( storedTitle )
                  : tabLabelWithoutLiveStatus( currentName );
        bool isNameEntered = false;
        auto newName = QInputDialog::getText( this, "Rename tab", "Tab name", QLineEdit::Normal,
                                              defaultName, &isNameEntered );
        if ( isNameEntered ) {
            newName = tabLabelWithoutLiveStatus( newName );
            TabNameMapping::getSynced().setTabName( tabPath, newName ).save();

            if ( newName.isEmpty() ) {
                myTabBar_.setTabText( tab,
                                      storedTitle.isEmpty() ? QFileInfo( tabPath ).fileName()
                                                            : storedTitle );
            }
            else {
                myTabBar_.setTabText( tab, tabLabelWithLiveStatus( newName, storedTitle ) );
            }
        }
    } );

    connect( resetTabName, &QAction::triggered, this, [ this, tab, tabPath ] {
        TabNameMapping::getSynced().setTabName( tabPath, "" ).save();
        myTabBar_.setTabText( tab, myTabBar_.tabData( tab ).toMap().value( TitleKey ).toString() );
    } );

    menu.exec( globalPoint );
}

void TabbedCrawlerWidget::keyPressEvent( QKeyEvent* event )
{
    const auto mod = event->modifiers();
    const auto key = event->key();

    LOG_DEBUG << "TabbedCrawlerWidget::keyPressEvent";

    // Ctrl + tab
    if ( ( mod == Qt::ControlModifier && key == Qt::Key_Tab )
         || ( mod == Qt::ControlModifier && key == Qt::Key_PageDown )
         || ( mod == ( Qt::ControlModifier | Qt::AltModifier | Qt::KeypadModifier )
              && key == Qt::Key_Right ) ) {
        setCurrentIndex( ( currentIndex() + 1 ) % count() );
    }
    // Ctrl + shift + tab
    else if ( ( mod == ( Qt::ControlModifier | Qt::ShiftModifier ) && key == Qt::Key_Tab )
              || ( mod == Qt::ControlModifier && key == Qt::Key_PageUp )
              || ( mod == ( Qt::ControlModifier | Qt::AltModifier | Qt::KeypadModifier )
                   && key == Qt::Key_Left ) ) {
        setCurrentIndex( ( currentIndex() - 1 >= 0 ) ? currentIndex() - 1 : count() - 1 );
    }
    // Ctrl + numbers
    else if ( mod == Qt::ControlModifier && ( key >= Qt::Key_1 && key <= Qt::Key_8 ) ) {
        int newIndex = key - Qt::Key_0;
        if ( newIndex <= count() )
            setCurrentIndex( newIndex - 1 );
    }
    // Ctrl + 9
    else if ( mod == Qt::ControlModifier && key == Qt::Key_9 ) {
        setCurrentIndex( count() - 1 );
    }
    else if ( mod == Qt::ControlModifier && ( key == Qt::Key_Q || key == Qt::Key_W ) ) {
        Q_EMIT tabCloseRequested( currentIndex() );
    }
    else {
        QTabWidget::keyPressEvent( event );
    }
}

void TabbedCrawlerWidget::updateIcon( int index )
{
    auto tabData = myTabBar_.tabData( index ).toMap();

    const QIcon* icon;
    switch ( static_cast<DataStatus>( tabData[ StatusKey ].toInt() ) ) {
    case DataStatus::OLD_DATA:
        icon = &olddata_icon_;
        break;
    case DataStatus::NEW_DATA:
        icon = &newdata_icon_;
        break;
    case DataStatus::NEW_FILTERED_DATA:
        icon = &newfiltered_icon_;
        break;
    default:
        return;
    }

    myTabBar_.setTabIcon( index, *icon );
}

void TabbedCrawlerWidget::setTabDataStatus( int index, DataStatus status )
{
    LOG_DEBUG << "TabbedCrawlerWidget::setTabDataStatus " << index;

    auto tabData = myTabBar_.tabData( index ).toMap();
    tabData[ StatusKey ] = static_cast<int>( status );
    myTabBar_.setTabData( index, tabData );

    updateIcon( index );
}

void TabbedCrawlerWidget::buildGroupSubmenu( QMenu* menu, int tabIndex )
{
    auto& groupManager = TabGroupManager::get();
    const auto tabPath = tabPathAt( tabIndex );
    const auto currentGroupId = groupManager.groupIdForTab( tabPath );

    // Add existing groups
    for ( const auto& group : groupManager.groups() ) {
        auto* action = menu->addAction( makeGroupColorIcon( group.color ), group.name );
        action->setCheckable( true );
        action->setChecked( group.id == currentGroupId );

        connect( action, &QAction::triggered, this, [ tabPath, groupId = group.id, &groupManager ] {
            groupManager.addTabToGroup( groupId, tabPath );
            groupManager.save();
        } );
    }

    if ( !groupManager.groups().isEmpty() ) {
        menu->addSeparator();
    }

    // Add "New Group..." action
    auto* newGroupAction = menu->addAction( tr( "New Group..." ) );
    connect( newGroupAction, &QAction::triggered, this,
             [ this, tabIndex ] { createNewGroupDialog( tabIndex ); } );
}

void TabbedCrawlerWidget::createNewGroupDialog( int tabIndex )
{
    bool ok = false;
    const auto groupName
        = QInputDialog::getText( this, tr( "New Tab Group" ), tr( "Group name:" ),
                                 QLineEdit::Normal, tr( "New Group" ), &ok );
    if ( !ok || groupName.trimmed().isEmpty() ) {
        return;
    }

    // Show color picker
    const QColor defaultColor( "#5B8CFF" );
    const QColor color = QColorDialog::getColor( defaultColor, this, tr( "Choose Group Color" ) );
    if ( !color.isValid() ) {
        return;
    }

    auto& groupManager = TabGroupManager::get();
    groupManager.createGroup( groupName.trimmed(), color );

    // Add the current tab to the new group
    const auto tabPath = tabPathAt( tabIndex );
    const auto& groups = groupManager.groups();
    if ( !groups.isEmpty() ) {
        groupManager.addTabToGroup( groups.last().id, tabPath );
    }

    groupManager.save();
}

QString TabbedCrawlerWidget::resolveCollapsedAnchorPath( const QString& groupId,
                                                         const TabGroup& group )
{
    const auto anchorPath = collapsedAnchorByGroup_.value( groupId );
    if ( !anchorPath.isEmpty() && group.tabPaths.contains( anchorPath )
         && tabIndexForPath( anchorPath ) >= 0 ) {
        return anchorPath;
    }

    const int activeTabIndex = currentIndex();
    if ( activeTabIndex >= 0 ) {
        const auto activeTabPath = tabPathAt( activeTabIndex );
        if ( group.tabPaths.contains( activeTabPath ) ) {
            collapsedAnchorByGroup_[ groupId ] = activeTabPath;
            return activeTabPath;
        }
    }

    for ( int i = 0; i < count(); ++i ) {
        const auto candidatePath = tabPathAt( i );
        if ( group.tabPaths.contains( candidatePath ) ) {
            collapsedAnchorByGroup_[ groupId ] = candidatePath;
            return candidatePath;
        }
    }

    collapsedAnchorByGroup_.remove( groupId );
    return {};
}

void TabbedCrawlerWidget::clearGroupChip( int tabIndex )
{
    if ( auto* oldButton = myTabBar_.tabButton( tabIndex, QTabBar::LeftSide );
         isGroupChipWidget( oldButton ) ) {
        myTabBar_.setTabButton( tabIndex, QTabBar::LeftSide, nullptr );
        oldButton->deleteLater();
    }
    if ( auto* oldButton = myTabBar_.tabButton( tabIndex, QTabBar::RightSide );
         isGroupChipWidget( oldButton ) ) {
        myTabBar_.setTabButton( tabIndex, QTabBar::RightSide, nullptr );
        oldButton->deleteLater();
    }
}

bool TabbedCrawlerWidget::updateGroupChip( int tabIndex, const TabGroup* group )
{
    if ( group == nullptr ) {
        clearGroupChip( tabIndex );
        return false;
    }

    clearGroupChip( tabIndex );

    auto* leftButton = myTabBar_.tabButton( tabIndex, QTabBar::LeftSide );
    auto* rightButton = myTabBar_.tabButton( tabIndex, QTabBar::RightSide );

    // Keep group chip on the left; if close button is on the left, move it to the right first.
    if ( leftButton && !isGroupChipWidget( leftButton ) && rightButton == nullptr ) {
        myTabBar_.setTabButton( tabIndex, QTabBar::LeftSide, nullptr );
        myTabBar_.setTabButton( tabIndex, QTabBar::RightSide, leftButton );
        leftButton = nullptr;
        rightButton = myTabBar_.tabButton( tabIndex, QTabBar::RightSide );
    }

    QTabBar::ButtonPosition chipSide = QTabBar::LeftSide;
    if ( leftButton && !isGroupChipWidget( leftButton ) ) {
        chipSide = QTabBar::RightSide;
        if ( rightButton && !isGroupChipWidget( rightButton ) ) {
            return false;
        }
    }

    const auto textColor = groupChipTextColor( group->color );
    const auto styleSheet = QStringLiteral(
                                "QToolButton {"
                                "border: 1px solid transparent;"
                                "border-radius: 4px;"
                                "padding: 1px 4px;"
                                "color: %1;"
                                "background-color: rgba(%2,%3,%4,42);"
                                "}"
                                "QToolButton:hover {"
                                "background-color: rgba(%2,%3,%4,72);"
                                "}" )
                                .arg( textColor.name(), QString::number( group->color.red() ),
                                      QString::number( group->color.green() ),
                                      QString::number( group->color.blue() ) );

    auto* chip = new QToolButton( &myTabBar_ );
    chip->setProperty( GroupChipPropertyKey, true );
    chip->setAutoRaise( true );
    chip->setFocusPolicy( Qt::NoFocus );
    chip->setCursor( Qt::PointingHandCursor );
    chip->setText( group->name );
    chip->setIcon( makeGroupColorIcon( group->color ) );
    chip->setIconSize( QSize( 10, 10 ) );
    chip->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );
    chip->setSizePolicy( QSizePolicy::Minimum, QSizePolicy::Fixed );
    chip->setStyleSheet( styleSheet );
    chip->setToolTip( tr( "Group: %1 (click to collapse/expand)" ).arg( group->name ) );
    chip->ensurePolished();
    const auto chipSizeHint = chip->sizeHint();
    chip->setMinimumWidth( chipSizeHint.width() );
    chip->adjustSize();

    myTabBar_.setTabButton( tabIndex, chipSide, chip );
    chip->updateGeometry();
    myTabBar_.updateGeometry();

    QObject::disconnect( chip, nullptr, this, nullptr );
    connect( chip, &QToolButton::clicked, this, [ groupId = group->id ] {
        auto& groupManager = TabGroupManager::get();
        groupManager.toggleCollapsed( groupId );
        groupManager.save();
    } );

    myTabBar_.update();
    return true;
}

void TabbedCrawlerWidget::onGroupsChanged()
{
    if ( tabDragInProgress_ ) {
        pendingGroupsRefresh_ = true;
        return;
    }

    auto& groupManager = TabGroupManager::get();
    auto& tabNameMapping = TabNameMapping::getSynced();
    const auto groups = groupManager.groups();
    QSet<QString> activeGroupIds;
    QHash<QString, QString> collapsedAnchorPathByGroup;
    QHash<QString, QString> chipOwnerByGroup;
    for ( const auto& group : groups ) {
        activeGroupIds.insert( group.id );
    }

    for ( auto it = collapsedAnchorByGroup_.begin(); it != collapsedAnchorByGroup_.end(); ) {
        if ( !activeGroupIds.contains( it.key() ) ) {
            it = collapsedAnchorByGroup_.erase( it );
        }
        else {
            ++it;
        }
    }

    for ( auto it = collapsedStateByGroup_.begin(); it != collapsedStateByGroup_.end(); ) {
        if ( !activeGroupIds.contains( it.key() ) ) {
            it = collapsedStateByGroup_.erase( it );
        }
        else {
            ++it;
        }
    }

    for ( const auto& group : groups ) {
        const bool wasCollapsed = collapsedStateByGroup_.value( group.id, false );
        if ( group.collapsed ) {
            if ( !wasCollapsed ) {
                const int activeTabIndex = currentIndex();
                if ( activeTabIndex >= 0 ) {
                    const auto activeTabPath = tabPathAt( activeTabIndex );
                    if ( group.tabPaths.contains( activeTabPath ) ) {
                        collapsedAnchorByGroup_[ group.id ] = activeTabPath;
                    }
                }
            }
            const auto anchorPath = resolveCollapsedAnchorPath( group.id, group );
            collapsedAnchorPathByGroup.insert( group.id, anchorPath );
            if ( !anchorPath.isEmpty() ) {
                chipOwnerByGroup.insert( group.id, anchorPath );
            }
        }
        else {
            for ( int i = 0; i < count(); ++i ) {
                const auto candidatePath = tabPathAt( i );
                if ( group.tabPaths.contains( candidatePath ) ) {
                    chipOwnerByGroup.insert( group.id, candidatePath );
                    break;
                }
            }
        }
        collapsedStateByGroup_[ group.id ] = group.collapsed;
    }

    for ( int i = 0; i < count(); ++i ) {
        const auto tabPath = tabPathAt( i );
        const auto* group = groupManager.groupForTab( tabPath );
        auto tabData = myTabBar_.tabData( i ).toMap();

        const auto customName = tabNameMapping.tabName( tabPath );
        const auto storedTitle = tabData.value( TitleKey ).toString();
        const auto storedToolTip = tabData.value( ToolTipKey ).toString();
        const auto originalTabLabel
            = customName.isEmpty()
                  ? ( storedTitle.isEmpty() ? QFileInfo( tabPath ).fileName() : storedTitle )
                  : tabLabelWithLiveStatus( customName, storedTitle );
        const auto originalTooltip
            = storedToolTip.isEmpty() ? QDir::toNativeSeparators( tabPath ) : storedToolTip;

        if ( !group ) {
            setTabVisibleCompat( i, true );
            myTabBar_.setTabToolTip( i, originalTooltip );
            myTabBar_.setTabTextColor( i, palette().color( QPalette::WindowText ) );
            myTabBar_.setTabText( i, originalTabLabel );
            tabData.remove( GroupIdKey );
            tabData.remove( GroupColorKey );
            tabData.remove( GroupLeaderKey );
            myTabBar_.setTabData( i, tabData );
            clearGroupChip( i );
            continue;
        }

        const auto collapsedAnchorPath = collapsedAnchorPathByGroup.value( group->id );
        const bool isAnchor = !collapsedAnchorPath.isEmpty() && tabPath == collapsedAnchorPath;
        const bool shouldBeVisible = !group->collapsed || isAnchor;
        setTabVisibleCompat( i, shouldBeVisible );

        myTabBar_.setTabToolTip( i, tr( "%1\nGroup: %2" ).arg( originalTooltip, group->name ) );
        myTabBar_.setTabTextColor( i, palette().color( QPalette::WindowText ) );

        const auto chipOwnerPath = chipOwnerByGroup.value( group->id );
        const bool isChipOwner = !chipOwnerPath.isEmpty() && chipOwnerPath == tabPath;
        bool chipAttached = false;
        if ( isChipOwner ) {
            chipAttached = updateGroupChip( i, group );
        }
        else {
            clearGroupChip( i );
        }

        if ( group->collapsed && isAnchor ) {
            myTabBar_.setTabText( i, group->name );
        }
        else if ( isChipOwner && !chipAttached ) {
            myTabBar_.setTabText( i, tr( "[%1] %2" ).arg( group->name, originalTabLabel ) );
        }
        else {
            myTabBar_.setTabText( i, originalTabLabel );
        }

        tabData[ GroupIdKey ] = group->id;
        tabData[ GroupColorKey ] = group->color;
        tabData[ GroupLeaderKey ] = isChipOwner;
        myTabBar_.setTabData( i, tabData );
    }

    myTabBar_.update();
}

void TabbedCrawlerWidget::populateGroupActions( QMenu* menu, const QString& groupId )
{
    if ( !menu ) {
        return;
    }

    auto& groupManager = TabGroupManager::get();
    auto* group = groupManager.groupById( groupId );
    if ( !group ) {
        return;
    }

    auto* collapseAction
        = menu->addAction( group->collapsed ? tr( "Expand Group" ) : tr( "Collapse Group" ) );
    connect( collapseAction, &QAction::triggered, this, [ groupId, &groupManager ] {
        groupManager.toggleCollapsed( groupId );
        groupManager.save();
    } );

    menu->addSeparator();

    auto* renameAction = menu->addAction( tr( "Rename Group..." ) );
    connect( renameAction, &QAction::triggered, this, [ this, groupId, groupName = group->name, &groupManager ] {
        bool ok = false;
        const auto newName = QInputDialog::getText( this, tr( "Rename Group" ), tr( "Group name:" ),
                                                    QLineEdit::Normal, groupName, &ok );
        if ( ok && !newName.trimmed().isEmpty() ) {
            groupManager.renameGroup( groupId, newName.trimmed() );
            groupManager.save();
        }
    } );

    auto* colorAction = menu->addAction( tr( "Change Color..." ) );
    connect( colorAction, &QAction::triggered, this, [ this, groupId, groupColor = group->color, &groupManager ] {
        const QColor newColor = QColorDialog::getColor( groupColor, this, tr( "Choose Group Color" ) );
        if ( newColor.isValid() ) {
            groupManager.setGroupColor( groupId, newColor );
            groupManager.save();
        }
    } );

    menu->addSeparator();

    auto* ungroupAction = menu->addAction( tr( "Ungroup All" ) );
    connect( ungroupAction, &QAction::triggered, this, [ groupId, &groupManager ] {
        groupManager.ungroupAll( groupId );
        groupManager.save();
    } );

    auto* closeAllAction = menu->addAction( tr( "Close All in Group" ) );
    connect( closeAllAction, &QAction::triggered, this, [ this, groupId, &groupManager ] {
        const auto* targetGroup = groupManager.groupById( groupId );
        if ( targetGroup ) {
            // Collect tabs to close
            QList<int> tabsToClose;
            for ( int i = 0; i < count(); ++i ) {
                if ( targetGroup->tabPaths.contains( tabPathAt( i ) ) ) {
                    tabsToClose.prepend( i ); // Prepend to close from end
                }
            }
            for ( int tabIndex : tabsToClose ) {
                Q_EMIT tabCloseRequested( tabIndex );
            }
        }
    } );
}
