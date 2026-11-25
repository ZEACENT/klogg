#include "mainwindow.h"

#include <utility>

#include "abstractcrawlerwidget.h"
#include "crawlerwidget.h"
#include "foldercrawlerwidget.h"
#include "configuration.h"
#include "preparedfilesource.h"

std::unique_ptr<Session::PreparedDocument> MainWindow::prepareFile(
    std::unique_ptr<PreparedFileSource>& source, bool followFile )
{
    if ( !source ) return {};
    const auto context = previousFileContext( source->path() );
    CrawlerWidget* crawler = nullptr;
    auto document = session_.prepareFile( source, [&crawler] {
        // The session factory adopts the view immediately on return.
        crawler = std::make_unique<CrawlerWidget>( nullptr, CrawlerWidget::InitialFocusPolicy::Preserve ).release();
        return crawler;
    } );
    if ( !document ) return {};
    try {
        if ( !context.isEmpty() ) document->view()->setViewContext( context );
        const auto& config = Configuration::get();
        if ( config.anyFileWatchEnabled() && ( followFile || config.followFileOnLoad() ) )
            crawler->followSet( true );
    } catch ( ... ) {
        session_.owner()->retirePrepared( document );
        throw;
    }
    return document;
}

std::unique_ptr<Session::PreparedDocument> MainWindow::prepareFolder(
    const QString& path, const std::vector<QString>& files )
{
    return session_.prepareFolder( path, files );
}

void MainWindow::recordPublishedDocument( const QString& path, DocumentKind kind, bool opened )
{
    if ( kind == DocumentKind::File ) addRecentFile( path );
    else if ( kind == DocumentKind::Folder && opened ) addRecentFolder( path );
}

struct MainWindow::Publication::State {
    QPointer<MainWindow> owner;
    std::unique_ptr<TabbedCrawlerWidget::Publication> tabs;
    bool finalized = false;
    bool released = false;
    bool notified = false;
};

MainWindow::Publication::Publication(
    MainWindow& owner, std::unique_ptr<TabbedCrawlerWidget::Publication> tabs )
    : state_( std::make_unique<State>() )
{
    state_->owner = &owner;
    state_->tabs = std::move( tabs );
}

MainWindow::Publication::~Publication()
{
    if ( state_->tabs ) state_->tabs->release();
}

bool MainWindow::Publication::install( std::unique_ptr<Session::PreparedDocument>& document )
{
    const auto owner = state_->owner;
    if ( !owner || state_->finalized || !document ) return false;
    const auto info = document->info();
    auto* file = dynamic_cast<CrawlerWidget*>( document->view() );
    auto* folder = dynamic_cast<FolderCrawlerWidget*>( document->view() );
    auto* widget = dynamic_cast<QWidget*>( document->view() );
    if ( !widget || widget->parent()
         || ( info.kind != DocumentKind::File && info.kind != DocumentKind::Folder )
         || ( info.kind == DocumentKind::File && !file )
         || ( info.kind == DocumentKind::Folder && !folder ) ) return false;
    if ( !owner->session_.adoptPrepared( document ) ) return false;
    const auto insert = [&]( auto* crawler ) {
        owner->mainTabWidget_.addCrawler( crawler, info.documentId, info.displayName,
            info.toolTip, info.toolTip, TabbedCrawlerWidget::SelectionPolicy::PreserveCurrent );
    };
    if ( file ) insert( file );
    else insert( folder );
    return true;
}

std::unique_ptr<Session::PreparedDocument> MainWindow::Publication::extract(
    AbstractCrawlerWidget* document )
{
    const auto owner = state_->owner;
    if ( !owner || state_->finalized || !document ) return {};
    auto* widget = dynamic_cast<QWidget*>( document );
    const auto index = owner->mainTabWidget_.indexOf( widget );
    if ( index < 0 || owner->session_.getDocumentKind( document ) == DocumentKind::AdbLogcat ) return {};
    auto bindings = widget->findChildren<QObject*>();
    bindings.prepend( widget );
    owner->mainTabWidget_.removeCrawler( index );
    widget->hide();
    widget->setParent( nullptr );
    auto detached = owner->session_.extractDocument( document );
    // The graph remains alive for retirement, but none of its descendant views
    // may keep forwarding selection/scratchpad/configuration to its old window.
    for ( auto* binding : bindings ) {
        QObject::disconnect( binding, nullptr, owner, nullptr );
        QObject::disconnect( owner, nullptr, binding, nullptr );
    }
    return detached;
}

void MainWindow::Publication::finalize()
{
    const auto owner = state_->owner;
    if ( !owner || state_->finalized ) return;
    state_->finalized = true;
    // Global group state may have been installed silently since insertion.
    owner->mainTabWidget_.onGroupsChanged();
    if ( owner ) owner->refreshInventory();
}

void MainWindow::Publication::release()
{
    if ( !state_->finalized || state_->released ) return;
    state_->released = true;
    state_->tabs->release();
}

void MainWindow::Publication::notify()
{
    if ( !state_->released || state_->notified ) return;
    state_->notified = true;
    const auto owner = state_->owner;
    // A native observer can destroy this publication or its window. Keep the
    // facade barrier local and never inspect publication members after emitting.
    auto tabs = std::move( state_->tabs );
    tabs->notify();
    if ( owner ) owner->syncCrawlerPresentationActivity();
    if ( owner ) owner->updateOpenedFilesMenu();
    if ( owner ) owner->scheduleSessionPersistence();
}

std::unique_ptr<MainWindow::Publication> MainWindow::beginPublication()
{
    auto tabs = mainTabWidget_.beginPublication();
    if ( !tabs ) return {};
    return std::unique_ptr<Publication>( new Publication( *this, std::move( tabs ) ) );
}
