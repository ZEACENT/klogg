#include <catch2/catch.hpp>
#include <future>
#include <memory>

#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QTemporaryDir>

#include "abstractcrawlerwidget.h"
#include "mainwindow.h"
#include "crawlerwidget.h"
#include "logdata.h"
#include "preparedfilesource.h"
#include "session.h"
#include "test_utils.h"

namespace {
struct WindowPublicationFixture {
    QTemporaryDir root;
    std::shared_ptr<Session> session = std::make_shared<Session>();
    MainWindow first{ WindowSession{ session, QStringLiteral( "PublishFirst" ), 0 } };
    MainWindow second{ WindowSession{ session, QStringLiteral( "PublishSecond" ), 1 } };
    QString path;
    WindowPublicationFixture()
    {
        REQUIRE( root.isValid() );
        path = root.filePath( "source.log" );
        QFile file{ path };
        REQUIRE( file.open( QIODevice::WriteOnly ) );
        REQUIRE( file.write( "evidence\n" ) == 9 );
    }
    std::unique_ptr<Session::PreparedDocument> folder()
    {
        return session->prepareFolder( root.path(), { path } );
    }
};
}

TEST_CASE( "Native window publication installs every inventory before notifying any window",
           "[window-publication]" )
{
    WindowPublicationFixture fixture;
    fixture.first.show();
    fixture.second.show();
    QWidget editorWindow;
    QLineEdit editor{ &editorWindow };
    editorWindow.show();
    editor.setFocus();
    REQUIRE( waitUiState( [&] { return QApplication::focusWidget() == &editor; } ) );
    auto firstDocument = fixture.folder();
    auto secondDocument = fixture.folder();
    REQUIRE( firstDocument );
    REQUIRE( secondDocument );
    const auto firstId = dynamic_cast<AbstractCrawlerWidget*>( firstDocument->view() )->runtimeTabId();
    const auto secondId = dynamic_cast<AbstractCrawlerWidget*>( secondDocument->view() )->runtimeTabId();
    auto first = fixture.first.beginPublication();
    auto second = fixture.second.beginPublication();
    REQUIRE( first );
    REQUIRE( second );
    CHECK_FALSE( fixture.first.beginPublication() );
    int notifications = 0;
    QObject::connect( &fixture.first, &QWidget::windowTitleChanged, &fixture.first, [&] {
        ++notifications;
        const auto other = fixture.second.inventorySnapshot();
        REQUIRE( other.tabs.size() == 1 );
        CHECK( other.currentTabId == secondId );
    } );
    REQUIRE( first->install( firstDocument ) );
    REQUIRE( second->install( secondDocument ) );
    CHECK_FALSE( firstDocument );
    CHECK_FALSE( secondDocument );
    CHECK( notifications == 0 );
    first->finalize();
    second->finalize();
    CHECK( fixture.first.inventorySnapshot().currentTabId == firstId );
    CHECK( fixture.second.inventorySnapshot().currentTabId == secondId );
    first->release();
    second->release();
    first->notify();
    second->notify();
    CHECK( notifications > 0 );
    CHECK( QApplication::focusWidget() == &editor );
}

TEST_CASE( "Publishing a prepared file preserves the surviving current document",
           "[window-publication]" )
{
    WindowPublicationFixture fixture;
    auto folder = fixture.folder();
    {
        auto publication = fixture.first.beginPublication();
        REQUIRE( publication->install( folder ) );
        publication->finalize();
        publication->release();
        publication->notify();
    }
    const auto currentId = fixture.first.inventorySnapshot().currentTabId;
    const auto settings = IndexingSettings::capture();
    AtomicFlag cancelled;
    auto result = std::async( std::launch::async, [&] {
        return prepareFileSource( fixture.path, settings, cancelled );
    } ).get();
    REQUIRE( result.source );
    auto file = fixture.session->prepareFile( result.source, [] {
        return new CrawlerWidget( nullptr, CrawlerWidget::InitialFocusPolicy::Preserve );
    } );
    REQUIRE( file );
    REQUIRE( file->startLoading() );
    auto* source = dynamic_cast<LogData*>( file->source() );
    REQUIRE( source );
    auto publication = fixture.first.beginPublication();
    REQUIRE( publication->install( file ) );
    publication->finalize();
    publication->release();
    publication->notify();
    const auto inventory = fixture.first.inventorySnapshot();
    REQUIRE( inventory.tabs.size() == 2 );
    CHECK( inventory.currentTabId == currentId );
    CHECK( inventory.tabs.back().kind == DocumentKind::File );
    CHECK( source->getLineString( 0_lnum ) == QStringLiteral( "evidence" ) );
    REQUIRE( source->activatePrepared() );
}

TEST_CASE( "Window publication rejects foreign candidates and outlives an empty owner",
           "[window-publication]" )
{
    WindowPublicationFixture fixture;
    auto otherSession = std::make_shared<Session>();
    auto foreign = otherSession->prepareFolder( fixture.root.path(), { fixture.path } );
    REQUIRE( foreign );
    auto window = std::make_unique<MainWindow>( WindowSession{ fixture.session, "Transient", 2 } );
    auto publication = window->beginPublication();
    REQUIRE( publication );
    CHECK_FALSE( publication->install( foreign ) );
    REQUIRE( foreign );
    CHECK( window->inventorySnapshot().tabs.empty() );
    window.reset();
    auto own = fixture.folder();
    CHECK_FALSE( publication->install( own ) );
    REQUIRE( own );
    publication->finalize();
    publication->release();
    publication->notify();
    CHECK( fixture.session->openedDocuments().empty() );
}

TEST_CASE( "Window publication observers can destroy the publication itself",
           "[window-publication]" )
{
    WindowPublicationFixture fixture;
    auto document = fixture.folder();
    auto publication = fixture.first.beginPublication();
    REQUIRE( publication );
    REQUIRE( publication->install( document ) );
    publication->finalize();
    publication->release();
    QObject::connect( &fixture.first, &QWidget::windowTitleChanged, &fixture.first,
                      [&] { publication.reset(); } );
    auto* active = publication.get();
    active->notify();
    CHECK_FALSE( publication );
    CHECK( fixture.first.inventorySnapshot().tabs.size() == 1 );
}

TEST_CASE( "Native window extraction leaves a detached graph for asynchronous retirement",
           "[window-publication]" )
{
    WindowPublicationFixture fixture;
    auto document = fixture.folder();
    REQUIRE( document );
    QPointer<QWidget> view{ dynamic_cast<QWidget*>( document->view() ) };
    {
        auto publication = fixture.first.beginPublication();
        REQUIRE( publication );
        REQUIRE( publication->install( document ) );
        publication->finalize();
        publication->release();
        publication->notify();
    }
    auto publication = fixture.first.beginPublication();
    REQUIRE( publication );
    auto detached = publication->extract( dynamic_cast<AbstractCrawlerWidget*>( view.data() ) );
    REQUIRE( detached );
    REQUIRE( view );
    CHECK_FALSE( view->parent() );
    CHECK_FALSE( view->isVisible() );
    publication->finalize();
    CHECK( fixture.first.inventorySnapshot().tabs.empty() );
    CHECK( fixture.session->openedDocuments().empty() );
    publication->release();
    publication->notify();
    REQUIRE( fixture.session->retirePrepared( detached ) );
    REQUIRE( waitUiState( [&] { return view.isNull(); } ) );
}
