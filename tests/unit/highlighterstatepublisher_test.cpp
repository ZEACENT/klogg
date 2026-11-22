#include <catch2/catch.hpp>
#include <memory>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUuid>

#include "abstractlogview.h"
#include "crawlerwidget.h"
#include "foldercrawlerwidget.h"
#include "highlightersmenu.h"
#include "highlightersdialog.h"
#include "highlighterset.h"
#include "highlighterstatepublisher.h"
#include "logfiltereddata.h"
#include "quickfindpattern.h"
#include "recentfiles.h"
#include "savedsearches.h"
#include "streaminglogdata.h"
#include "test_utils.h"

struct HighlighterPublicationViewAccess {};
template <> struct AbstractLogView::access_by<HighlighterPublicationViewAccess> {
    static int refreshes( const AbstractLogView* view ) { return view->forceRefreshCountForTest_; }
};

namespace {
struct GlobalState {
    ScopedRegexpEngine engine{ RegexpEngine::QRegularExpression };
    HighlighterSetCollection saved{ HighlighterSetCollection::getSynced() };
    HighlighterSet set = HighlighterSet::createNewSet( "Global evidence" );
    GlobalState()
    {
        SavedSearches::getSynced();
        RecentFiles::getSynced();
        HighlighterSetCollection::get().setHighlighterSets( { set } );
        HighlighterSetCollection::get().deactivateAll();
    }
    ~GlobalState()
    {
        HighlighterSetCollection::get() = saved;
        HighlighterSetCollection::get().save();
    }
};
QAction* actionFor( HighlightersMenu& menu, const QString& id )
{
    for ( auto* action : menu.actions() ) if ( action->data().toString() == id ) return action;
    return nullptr;
}
}

TEST_CASE( "A highlighter menu publishes once to every live and folder view without activation",
           "[highlighter-publication-ui]" )
{
    GlobalState global;
    QTemporaryDir directory;
    REQUIRE( directory.isValid() );
    auto data = std::make_shared<StreamingLogData>( QUuid::createUuid().toString( QUuid::WithoutBraces ), directory.path() );
    data->appendUtf8( "evidence\n" );
    std::shared_ptr<LogFilteredData> filtered = data->getNewFilteredData();
    SavedSearches searches;
    CrawlerWidget live;
    live.setData( data, filtered );
    live.setQuickFindPattern( std::make_shared<QuickFindPattern>() );
    live.setSavedSearches( &searches );
    FolderCrawlerWidget folder;
    FolderCrawlerWidget otherFolder;
    auto views = live.findChildren<AbstractLogView*>();
    views.append( folder.findChildren<AbstractLogView*>() );
    views.append( otherFolder.findChildren<AbstractLogView*>() );
    REQUIRE( views.size() >= 6 );
    std::vector<int> refreshes;
    std::vector<LineNumber> topLines;
    for ( const auto* view : views ) {
        refreshes.push_back( AbstractLogView::access_by<HighlighterPublicationViewAccess>::refreshes( view ) );
        topLines.push_back( view->getTopLine() );
    }
    HighlightersMenu menu( "First" );
    menu.createHighlightersMenu();
    menu.populateHighlightersMenu();
    HighlightersMenu otherMenu( "Second" );
    otherMenu.createHighlightersMenu();
    otherMenu.populateHighlightersMenu();
    auto* action = actionFor( menu, global.set.id() );
    REQUIRE( action );
    QPointer<QAction> retainedAction = action;
    auto* const focus = QApplication::focusWidget();
    auto* const active = QApplication::activeWindow();
    QSignalSpy publications( &HighlighterStatePublisher::instance(), &HighlighterStatePublisher::changed );
    action->trigger();
    CHECK( publications.size() == 1 );
    CHECK( retainedAction ); // A triggered action must not be rebuilt on its own stack.
    CHECK( HighlighterSetCollection::get().activeSetIds() == QStringList{ global.set.id() } );
    size_t index = 0;
    for ( const auto* view : views ) {
        CHECK( AbstractLogView::access_by<HighlighterPublicationViewAccess>::refreshes( view ) == refreshes[ index ] + 1 );
        CHECK( view->getTopLine() == topLines[ index ] );
        ++index;
    }
    CHECK( QApplication::focusWidget() == focus );
    CHECK( QApplication::activeWindow() == active );
    Q_EMIT otherMenu.aboutToShow();
    auto* mirrored = actionFor( otherMenu, global.set.id() );
    REQUIRE( mirrored );
    CHECK( mirrored->isChecked() );
}

TEST_CASE( "The human highlighter dialog publishes before accepted observers can retrieve state",
           "[highlighter-publication-ui][highlighter-dialog-publication]" )
{
    GlobalState global;
    HighlighterSetCollection::get().save();
    HighlightersDialog dialog;
    QSignalSpy publications( &HighlighterStatePublisher::instance(), &HighlighterStatePublisher::changed );
    int accepted = 0;
    QObject::connect( &dialog, &QDialog::accepted, &dialog, [&] {
        ++accepted;
        CHECK( publications.size() == 1 );
    } );
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    REQUIRE( buttons );
    REQUIRE( buttons->button( QDialogButtonBox::Ok ) );
    buttons->button( QDialogButtonBox::Ok )->click();
    CHECK( accepted == 1 );
    CHECK( publications.size() == 1 );
}

TEST_CASE( "Highlighter publication persists before notifying observers and refreshes future menus",
           "[highlighter-publication-ui]" )
{
    GlobalState global;
    HighlightersMenu menu( "Existing menu" );
    menu.createHighlightersMenu();
    menu.populateHighlightersMenu();
    const auto replacement = HighlighterSet::createNewSet( "Replacement" );
    auto& owner = HighlighterSetCollection::get();
    owner.setHighlighterSets( { replacement } );
    owner.activateSet( replacement.id() );
    const auto revision = owner.revision();
    int notified = 0;
    QObject context;
    QObject::connect( &HighlighterStatePublisher::instance(), &HighlighterStatePublisher::changed, &context, [&] {
        ++notified;
        CHECK( HighlighterSetCollection::getSynced().activeSetIds() == QStringList{ replacement.id() } );
        CHECK( owner.revision() == revision );
    } );
    CHECK( HighlighterStatePublisher::instance().publish() );
    CHECK( notified == 1 );
    Q_EMIT menu.aboutToShow();
    CHECK_FALSE( actionFor( menu, global.set.id() ) );
    auto* action = actionFor( menu, replacement.id() );
    REQUIRE( action );
    CHECK( action->isChecked() );
}
