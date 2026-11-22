#include "highlightersmenu.h"

#include "highlighterstatepublisher.h"
#include <qaction.h>

HighlightersMenu::HighlightersMenu( const QString& title, QWidget* parent )
    : HoverMenu( title, parent )
    , highLighters_{ nullptr }
{
    connect( this, &QMenu::aboutToShow, this, [this] {
        if ( contentDirty_ ) {
            clearHighlightersMenu();
            createHighlightersMenu();
            populateHighlightersMenu();
        }
        updateActionsStatus();
    } );
    connect( &HighlighterStatePublisher::instance(), &HighlighterStatePublisher::changed,
             this, [this] {
        // Do not replace actions while a menu is open or emitting triggered().
        // The next presentation reads the authoritative collection afresh.
        contentDirty_ = true;
    } );
}

void HighlightersMenu::applySelectionHighlighters( QAction* action ) const
{
    saveCurrentHighlighterFromAction( action );

    updateActionsStatus();
}

void HighlightersMenu::clearHighlightersMenu()
{
    clear();

    if ( highLighters_ ) {
        highLighters_->deleteLater();
        highLighters_ = nullptr;
    }
    contentDirty_ = true;
}

void HighlightersMenu::createHighlightersMenu()
{
    highLighters_ = new QActionGroup( this );
    highLighters_->setExclusive( false );
    connect( highLighters_, &QActionGroup::triggered, this,
             &HighlightersMenu::applySelectionHighlighters );
}

void HighlightersMenu::populateHighlightersMenu()
{
    const auto& highlightersCollection = HighlighterSetCollection::get();
    const auto& highlighterSets = highlightersCollection.highlighterSets();
    const auto& activeSetIds = highlightersCollection.activeSetIds();

    auto noneAction = addAction( tr( "None" ) );
    noneAction->setActionGroup( highLighters_ );
    noneAction->setEnabled( !activeSetIds.isEmpty() );

    addSeparator();

    for ( const auto& highlighter : highlighterSets ) {
        auto setAction = addAction( highlighter.name() );
        setAction->setActionGroup( highLighters_ );
        setAction->setCheckable( true );
        setAction->setChecked( activeSetIds.contains( highlighter.id() ) );
        setAction->setData( highlighter.id() );
    }
    contentDirty_ = false;
}

void HighlightersMenu::saveCurrentHighlighterFromAction( const QAction* action ) const
{
    auto setId = action->data().toString();
    auto& highlighterSets = HighlighterSetCollection::get();

    if ( setId.isEmpty() ) {
        highlighterSets.deactivateAll();
    }
    else if ( action->isChecked() ) {
        highlighterSets.activateSet( setId );
    }
    else {
        highlighterSets.deactivateSet( setId );
    }

    HighlighterStatePublisher::instance().publish();
}

void HighlightersMenu::updateActionsStatus() const
{
    const auto activeSets = HighlighterSetCollection::get().activeSetIds();

    const auto selectNone = activeSets.isEmpty();

    for ( auto* action : highLighters_->actions() ) {
        const auto actionSet = action->data().toString();
        // update "None" action state
        if ( actionSet.isEmpty() ) {
            action->setEnabled( !selectNone );
        }
        // update highlighter action state
        action->setChecked( activeSets.contains( actionSet )
                            || ( actionSet.isEmpty() && selectNone ) );
    }
}

void HighlightersMenu::addAction( QAction* action, bool seq )
{
    addAction( action );
    if ( seq ) {
        addSeparator();
    }
}
