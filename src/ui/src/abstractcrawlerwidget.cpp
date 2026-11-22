#include "abstractcrawlerwidget.h"

#include "abstractlogview.h"

std::vector<AbstractCrawlerWidget::PaneInventoryEntry> AbstractCrawlerWidget::paneInventory() const
{
    std::vector<PaneInventoryEntry> result;
    for ( const auto& pane : reviewPanes() ) {
        if ( !pane.view ) continue;
        result.push_back( { pane.view->runtimePaneId(), pane.kind, pane.title,
            pane.kind == Pane::Main || pane.view == reviewPane( Pane::Results ),
            pane.available ? pane.view->paneMetadata() : PaneMetadata{},
            pane.marksOwnerId, pane.appliedFilter } );
    }
    return result;
}

const AbstractLogView* AbstractCrawlerWidget::findReviewPane( const QString& paneId ) const
{
    if ( paneId.isEmpty() ) return nullptr;
    for ( const auto& pane : reviewPanes() ) {
        if ( pane.view && pane.available && pane.view->runtimePaneId() == paneId ) return pane.view;
    }
    return nullptr;
}

AbstractCrawlerWidget::PaneMetadata AbstractCrawlerWidget::paneMetadata( const QString& paneId ) const
{
    const auto* view = findReviewPane( paneId );
    return view ? view->paneMetadata() : PaneMetadata{};
}

klogg::LogSnapshot::Ptr AbstractCrawlerWidget::createPaneSnapshot( const QString& paneId,
                                                                klogg::SnapshotScope scope ) const
{
    const auto* view = findReviewPane( paneId );
    return view ? view->createPaneSnapshot( scope ) : klogg::LogSnapshot::Ptr{};
}

AbstractCrawlerWidget::PaneMetadata AbstractCrawlerWidget::paneMetadata( Pane pane ) const
{
    const auto* view = reviewPane( pane );
    return view ? view->paneMetadata() : PaneMetadata{};
}

klogg::LogSnapshot::Ptr
AbstractCrawlerWidget::createPaneSnapshot( Pane pane, klogg::SnapshotScope scope ) const
{
    const auto* view = reviewPane( pane );
    return view ? view->createPaneSnapshot( scope ) : klogg::LogSnapshot::Ptr{};
}
