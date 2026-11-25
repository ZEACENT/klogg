#include <catch2/catch.hpp>

#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>

#include "abstractlogview.h"
#include "ownedtaskpoolgate.h"
#include <memory>
#include <stdexcept>

#include "crawlerwidget.h"
#include "foldercrawlerwidget.h"
#include "logdata.h"
#include "session.h"
#include "test_utils.h"
#include "viewinterface.h"

namespace {
struct Fixture {
    QTemporaryDir root;
    std::shared_ptr<Session> session = std::make_shared<Session>();
    WindowSession window{ session, QStringLiteral( "PreparedSources" ), 0 };
    QString path;
    Fixture()
    {
        REQUIRE( root.isValid() );
        path = root.filePath( "evidence.log" );
        QFile file{ path };
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        REQUIRE( file.write( "one\ntwo\n" ) == 8 );
    }
};

class RejectingView final : public CrawlerWidget {
  protected:
    void doSetSavedSearches( SavedSearches* ) override
    {
        throw std::runtime_error( "controlled view initialization failure" );
    }
};
}

TEST_CASE( "Prepared file resources load privately before one-time session adoption",
           "[session-preparation]" )
{
    Fixture fixture;
    QWidget editorWindow;
    QLineEdit editor{ &editorWindow };
    editorWindow.show();
    editor.setFocus();
    REQUIRE( waitUiState( [&] { return QApplication::focusWidget() == &editor; } ) );
    const auto* focus = QApplication::focusWidget();
    auto prepared = fixture.window.prepareFile( fixture.path, [] { return new CrawlerWidget; } );
    REQUIRE( prepared );
    auto* view = dynamic_cast<CrawlerWidget*>( prepared->view() );
    REQUIRE( view );
    REQUIRE( prepared->source() );
    CHECK_FALSE( view->isVisible() );
    CHECK( prepared->info().documentId == fixture.path );
    CHECK( prepared->info().kind == DocumentKind::File );
    CHECK( fixture.session->openedDocuments().empty() );
    CHECK( fixture.window.openedDocuments().empty() );
    CHECK_FALSE( fixture.session->getViewIfOpen( fixture.path ) );
    CHECK_FALSE( fixture.window.adoptPrepared( prepared ) ); // File attachment has not started.
    SafeQSignalSpy loaded( prepared->source(), &SearchableLogData::loadingFinished );
    REQUIRE( prepared->startLoading() );
    REQUIRE( loaded.safeWait( 10000 ) );
    REQUIRE( prepared->source()->getNbLine() == 2_lcount );
    CHECK_FALSE( prepared->startLoading() );
    CHECK( fixture.session->openedDocuments().empty() );
    CHECK( fixture.window.openedDocuments().empty() );
    CHECK( QApplication::focusWidget() == focus );
    std::unique_ptr<ViewInterface> published{ fixture.window.adoptPrepared( prepared ) };
    REQUIRE( published.get() == view );
    CHECK_FALSE( prepared );
    CHECK( fixture.session->openedDocuments().size() == 1 );
    CHECK( fixture.window.openedDocuments().size() == 1 );
    CHECK( fixture.session->getViewIfOpen( fixture.path ) == view );
    CHECK_FALSE( fixture.window.adoptPrepared( prepared ) );
    fixture.window.close( published.get() );
    CHECK( fixture.window.openedDocuments().empty() );
}

TEST_CASE( "Prepared folder resources remain unpublished and discarded graphs release their views",
           "[session-preparation]" )
{
    Fixture fixture;
    const auto folderPath = fixture.root.path() + QLatin1Char( '/' );
    auto prepared = fixture.window.prepareFolder( folderPath, { fixture.path } );
    REQUIRE( prepared );
    QPointer<FolderCrawlerWidget> view{ dynamic_cast<FolderCrawlerWidget*>( prepared->view() ) };
    REQUIRE( view );
    CHECK_FALSE( prepared->source() );
    CHECK( prepared->info().kind == DocumentKind::Folder );
    CHECK_FALSE( prepared->info().displayName.isEmpty() );
    CHECK( fixture.window.openedDocuments().empty() );
    SECTION( "discard private resources" ) {
        prepared.reset();
        CHECK_FALSE( view );
        CHECK( fixture.session->openedDocuments().empty() );
    }
    SECTION( "another application session cannot adopt them" ) {
        Session other;
        CHECK_FALSE( other.adoptPrepared( prepared ) );
        REQUIRE( prepared );
        CHECK( other.openedDocuments().empty() );
        CHECK( fixture.session->openedDocuments().empty() );
    }
    SECTION( "publish to the owning session" ) {
        std::unique_ptr<ViewInterface> published{ fixture.window.adoptPrepared( prepared ) };
        REQUIRE( published );
        CHECK_FALSE( prepared );
        CHECK( fixture.window.openedDocuments().size() == 1 );
        CHECK( fixture.session->getDocumentKind( published.get() ) == DocumentKind::Folder );
        fixture.window.close( published.get() );
    }
}

TEST_CASE( "Retired private documents cannot start or publish and release on the GUI thread",
           "[session-preparation]" )
{
    Fixture fixture;
    std::unique_ptr<Session::PreparedDocument> prepared;
    bool started = false;
    SECTION( "unstarted file" ) {
        prepared = fixture.window.prepareFile( fixture.path, [] { return new CrawlerWidget; } );
    }
    SECTION( "loading file" ) {
        prepared = fixture.window.prepareFile( fixture.path, [] { return new CrawlerWidget; } );
        REQUIRE( prepared );
        REQUIRE( prepared->startLoading() );
        started = true;
    }
    SECTION( "folder" ) {
        prepared = fixture.window.prepareFolder( fixture.root.path(), { fixture.path } );
    }
    REQUIRE( prepared );
    QPointer<QObject> view{ dynamic_cast<QObject*>( prepared->view() ) };
    REQUIRE( view );
    const auto retired = prepared->retire();
    if ( started ) CHECK_FALSE( retired.isFinished() );
    CHECK_FALSE( prepared->startLoading() );
    // Keep regression cleanup valid even when the pre-fix adoption succeeds.
    std::unique_ptr<ViewInterface> wronglyPublished{ fixture.window.adoptPrepared( prepared ) };
    CHECK_FALSE( wronglyPublished );
    if ( wronglyPublished ) fixture.window.close( wronglyPublished.get() );
    CHECK( fixture.session->openedDocuments().empty() );
    REQUIRE( waitUiState( [&] { return retired.isFinished(); } ) );
    if ( prepared ) {
        CHECK( prepared->retire().isFinished() );
        prepared.reset();
        CHECK_FALSE( view );
    }
}

TEST_CASE( "Session retains cancelled private documents until every owned task has retired",
           "[session-preparation][document-retirement]" )
{
    Fixture fixture;
    auto prepared = fixture.window.prepareFile( fixture.path, [] {
        return new CrawlerWidget( nullptr, CrawlerWidget::InitialFocusPolicy::Preserve );
    } );
    REQUIRE( prepared );
    SafeQSignalSpy loaded{ prepared->source(), &SearchableLogData::loadingFinished };
    REQUIRE( prepared->startLoading() );
    REQUIRE( loaded.safeWait( 10000 ) );
    QPointer<CrawlerWidget> view{ dynamic_cast<CrawlerWidget*>( prepared->view() ) };
    QPointer<SearchableLogData> source{ prepared->source() };
    REQUIRE( view );
    bool viewDestroyedBeforeSource = false;
    bool destroyedOnOwnerThread = false;
    QObject observer;
    QObject::connect( view, &QObject::destroyed, &observer, [&] {
        viewDestroyedBeforeSource = source != nullptr;
        destroyedOnOwnerThread = QThread::currentThread() == observer.thread();
    } );
    fixture.session->quickFindPattern()->changeSearchPattern( QStringLiteral( "one" ) );
    ScopedOwnedTaskPoolGate gate;
    REQUIRE( gate.entered.tryAcquire( 1, 5000 ) );
    const auto panes = view->findChildren<AbstractLogView*>();
    REQUIRE_FALSE( panes.empty() );
    for ( auto* pane : panes ) pane->searchForward();
    REQUIRE( fixture.session->retirePrepared( prepared ) );
    CHECK_FALSE( prepared );
    CHECK_FALSE( fixture.session->retirePrepared( prepared ) );
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    CHECK( view );
    CHECK( source );
    CHECK( fixture.session->openedDocuments().empty() );
    CHECK( fixture.window.openedDocuments().empty() );
    gate.release.release();
    REQUIRE( waitUiState( [&] {
        QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
        return view.isNull() && source.isNull();
    } ) );
    CHECK( viewDestroyedBeforeSource );
    CHECK( destroyedOnOwnerThread );
}

TEST_CASE( "Private retirement rejects foreign graphs and shutdown drains accepted graphs",
           "[session-preparation][document-retirement]" )
{
    Fixture fixture;
    auto prepared = fixture.window.prepareFolder( fixture.root.path(), { fixture.path } );
    REQUIRE( prepared );
    QPointer<QObject> view{ dynamic_cast<QObject*>( prepared->view() ) };
    Session other;
    CHECK_FALSE( other.retirePrepared( prepared ) );
    REQUIRE( prepared );
    REQUIRE( view );
    REQUIRE( fixture.session->retirePrepared( prepared ) );
    CHECK_FALSE( prepared );
    fixture.session->drainRetiredDocuments();
    CHECK_FALSE( view );
    fixture.session->drainRetiredDocuments();
    CHECK( fixture.session->openedDocuments().empty() );
}

TEST_CASE( "Retirement cannot take a graph that is still owned by a Qt presentation parent",
           "[session-preparation][document-retirement]" )
{
    Fixture fixture;
    QWidget presentation;
    auto prepared = fixture.window.prepareFolder( fixture.root.path(), { fixture.path } );
    REQUIRE( prepared );
    QPointer<QWidget> view{ dynamic_cast<QWidget*>( prepared->view() ) };
    REQUIRE( view );
    view->setParent( &presentation );
    const bool accepted = fixture.session->retirePrepared( prepared );
    // Detach even on RED so the old queue and the presentation cannot both delete it.
    if ( view ) view->setParent( nullptr );
    CHECK_FALSE( accepted );
    REQUIRE( prepared );
    REQUIRE( fixture.session->retirePrepared( prepared ) );
    fixture.session->drainRetiredDocuments();
    CHECK_FALSE( view );
}

TEST_CASE( "Retired graph cleanup tolerates reentrant draining and owner destruction",
           "[session-preparation][document-retirement]" )
{
    QTemporaryDir root;
    REQUIRE( root.isValid() );
    auto session = std::make_unique<Session>();
    auto first = session->prepareFolder( root.path(), { root.filePath( "first.log" ) } );
    auto second = session->prepareFolder( root.path(), { root.filePath( "second.log" ) } );
    REQUIRE( first );
    REQUIRE( second );
    QPointer<QObject> firstView{ dynamic_cast<QObject*>( first->view() ) };
    QPointer<QObject> secondView{ dynamic_cast<QObject*>( second->view() ) };
    QObject observer;
    const bool destroyOwner = GENERATE( false, true );
    QObject::connect( firstView, &QObject::destroyed, &observer, [&] {
        if ( destroyOwner ) session.reset();
        else session->drainRetiredDocuments();
    } );
    REQUIRE( session->retirePrepared( first ) );
    REQUIRE( session->retirePrepared( second ) );
    REQUIRE( waitUiState( [&] {
        QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
        return firstView.isNull() && secondView.isNull();
    } ) );
    CHECK( static_cast<bool>( session ) != destroyOwner );
}

TEST_CASE( "Window membership never closes another window's duplicate document",
           "[session-preparation][document-retirement]" )
{
    Fixture fixture;
    WindowSession other{ fixture.session, QStringLiteral( "OtherDuplicateSources" ), 1 };
    std::unique_ptr<ViewInterface> first{
        fixture.window.openFolder( fixture.root.path(), { fixture.path } ) };
    std::unique_ptr<ViewInterface> second{
        other.openFolder( fixture.root.path(), { fixture.path } ) };
    REQUIRE( first );
    REQUIRE( second );
    other.close( first.get() );
    CHECK( fixture.session->openedDocuments().size() == 2 );
    CHECK( fixture.window.openedDocuments().size() == 1 );
    CHECK( other.openedDocuments().size() == 1 );
}

TEST_CASE( "Window extraction transfers only its exact detached document graph",
           "[session-preparation][document-retirement]" )
{
    Fixture fixture;
    WindowSession other{ fixture.session, QStringLiteral( "OtherPreparedSources" ), 1 };
    const bool folder = GENERATE( false, true );
    auto prepare = [&]( WindowSession& window ) {
        if ( folder ) return window.prepareFolder( fixture.root.path(), { fixture.path } );
        auto candidate = window.prepareFile( fixture.path, [] {
            return new CrawlerWidget( nullptr, CrawlerWidget::InitialFocusPolicy::Preserve );
        } );
        REQUIRE( candidate );
        SafeQSignalSpy loaded{ candidate->source(), &SearchableLogData::loadingFinished };
        REQUIRE( candidate->startLoading() );
        REQUIRE( loaded.safeWait( 10000 ) );
        return candidate;
    };
    auto firstCandidate = prepare( fixture.window );
    auto secondCandidate = prepare( other );
    std::unique_ptr<ViewInterface> first{ fixture.window.adoptPrepared( firstCandidate ) };
    std::unique_ptr<ViewInterface> second{ other.adoptPrepared( secondCandidate ) };
    REQUIRE( first );
    REQUIRE( second );
    QPointer<QWidget> view{ dynamic_cast<QWidget*>( first.get() ) };
    REQUIRE( view );
    CHECK_FALSE( other.extractDocument( first.get() ) );
    CHECK( fixture.window.openedDocuments().size() == 1 );
    CHECK( other.openedDocuments().size() == 1 );
    CHECK( fixture.session->openedDocuments().size() == 2 );
    {
        QWidget presentation;
        view->setParent( &presentation );
        CHECK_FALSE( fixture.window.extractDocument( first.get() ) );
        view->setParent( nullptr );
    }
    auto extracted = fixture.window.extractDocument( first.get() );
    if ( extracted ) first.release(); // Ownership transferred only on success.
    REQUIRE( extracted );
    CHECK( dynamic_cast<QWidget*>( extracted->view() ) == view.data() );
    CHECK( view );
    CHECK( fixture.window.openedDocuments().empty() );
    CHECK( other.openedDocuments().size() == 1 );
    CHECK( fixture.session->openedDocuments().size() == 1 );
    CHECK_FALSE( fixture.window.extractDocument( extracted->view() ) );
    CHECK_FALSE( extracted->startLoading() );
    CHECK_FALSE( fixture.window.adoptPrepared( extracted ) );
    REQUIRE( fixture.session->retirePrepared( extracted ) );
    REQUIRE( waitUiState( [&] {
        QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
        return view.isNull();
    } ) );
    CHECK( fixture.session->getViewIfOpen( folder ? fixture.root.path() : fixture.path )
           == second.get() );
    other.close( second.get() );
    CHECK( fixture.session->openedDocuments().empty() );
}

TEST_CASE( "Failed file factories do not leave phantom window membership on a later successful open",
           "[session-preparation]" )
{
    Fixture fixture;
    CHECK_THROWS( fixture.window.open( fixture.path, []() -> ViewInterface* {
        throw std::runtime_error( "controlled factory failure" );
    } ) );
    CHECK( fixture.session->openedDocuments().empty() );
    CHECK( fixture.window.openedDocuments().empty() );
    std::unique_ptr<SafeQSignalSpy> loaded;
    std::unique_ptr<ViewInterface> view{ fixture.window.open( fixture.path, [&] {
        auto* widget = new CrawlerWidget;
        loaded = std::make_unique<SafeQSignalSpy>( widget, &CrawlerWidget::loadingFinished );
        return widget;
    } ) };
    REQUIRE( view );
    REQUIRE( loaded );
    REQUIRE( loaded->safeWait( 10000 ) );
    CHECK( fixture.session->openedDocuments().size() == 1 );
    CHECK( fixture.window.openedDocuments().size() == 1 );
    fixture.window.close( view.get() );
}

TEST_CASE( "Failed view initialization destroys its unpublished graph without session insertion",
           "[session-preparation]" )
{
    Fixture fixture;
    QPointer<CrawlerWidget> created;
    CHECK_THROWS( fixture.window.open( fixture.path, [&] {
        auto* widget = new RejectingView;
        created = widget;
        return widget;
    } ) );
    CHECK_FALSE( created );
    CHECK( fixture.session->openedDocuments().empty() );
    CHECK( fixture.window.openedDocuments().empty() );
    // Clean up the pre-fix leak when this regression is run against the old path.
    if ( created ) delete created.data();
}
