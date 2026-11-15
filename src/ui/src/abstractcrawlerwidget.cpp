#include "abstractcrawlerwidget.h"

#include "abstractlogview.h"

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
