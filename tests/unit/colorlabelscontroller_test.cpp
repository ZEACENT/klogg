#include <catch2/catch.hpp>

#include <QTemporaryDir>
#include <QUuid>
#include <QWidget>
#include <memory>
#include <limits>

#include "abstractlogview.h"
#include "colorlabelscontroller.h"
#include "crawlerwidget.h"
#include "foldercrawlerwidget.h"
#include "logfiltereddata.h"
#include "savedsearches.h"
#include "searchtoolbar.h"
#include "reviewtransaction.h"
#include "logmainview.h"
#include "overviewwidget.h"
#include "quickfindpattern.h"
#include "streaminglogdata.h"

struct ColorLabelCommandViewAccess;
template <>
struct AbstractLogView::access_by<ColorLabelCommandViewAccess> {
    static const std::vector<QuickHighlighters>& labels( const AbstractLogView& view )
    {
        return view.quickHighlighters_;
    }
};

namespace {
struct LabelViews {
    QTemporaryDir directory;
    StreamingLogData data{ QUuid::createUuid().toString( QUuid::WithoutBraces ), directory.path() };
    QuickFindPattern pattern;
    QWidget parent;
    OverviewWidget overview{ &parent };
    std::unique_ptr<LogMainView> main;
    std::unique_ptr<LogMainView> results;
    ColorLabelsController controller{ &parent, [ this ] { return main.get(); } };

    LabelViews()
    {
        REQUIRE( directory.isValid() );
        data.appendUtf8( "Error alpha\nWarning beta\n" );
        main = std::make_unique<LogMainView>( &data, &pattern, nullptr, &overview, &parent );
        results = std::make_unique<LogMainView>( &data, &pattern, nullptr, &overview, &parent );
        controller.watchView( main.get() );
        controller.watchView( results.get() );
        main->selectAndDisplayLine( 0_lnum );
        results->selectAndDisplayLine( 1_lnum );
    }

    static bool hasLabel( const AbstractLogView& view, size_t label, const QString& text )
    {
        const auto& entries = AbstractLogView::access_by<ColorLabelCommandViewAccess>::labels( view );
        for ( const auto& entry : entries.at( label ) ) {
            if ( entry.text == text ) {
                return true;
            }
        }
        return false;
    }
};
} // namespace

TEST_CASE( "Color-label GUI intents share one tab state without changing selections",
           "[colorlabels][control-characterization]" )
{
    LabelViews views;
    REQUIRE( views.main->getSelectedText() == QStringLiteral( "Error alpha" ) );
    REQUIRE( views.results->getSelectedText() == QStringLiteral( "Warning beta" ) );

    // A signal from the results pane must use its selection, not the active-view
    // fallback. Both projections and later-created panes share that same state.
    Q_EMIT views.results->addColorLabel( 2 );
    CHECK( LabelViews::hasLabel( *views.main, 2, QStringLiteral( "Warning beta" ) ) );
    CHECK( LabelViews::hasLabel( *views.results, 2, QStringLiteral( "Warning beta" ) ) );
    CHECK_FALSE( LabelViews::hasLabel( *views.main, 2, QStringLiteral( "Error alpha" ) ) );

    LogMainView late{ &views.data, &views.pattern, nullptr, &views.overview, &views.parent };
    views.controller.watchView( &late );
    CHECK( LabelViews::hasLabel( late, 2, QStringLiteral( "Warning beta" ) ) );

    Q_EMIT views.results->removeColorLabel();
    CHECK_FALSE( LabelViews::hasLabel( late, 2, QStringLiteral( "Warning beta" ) ) );
    CHECK_FALSE( LabelViews::hasLabel( *views.main, 2, QStringLiteral( "Warning beta" ) ) );
    CHECK( views.main->getSelectedText() == QStringLiteral( "Error alpha" ) );
    CHECK( views.results->getSelectedText() == QStringLiteral( "Warning beta" ) );
}

TEST_CASE( "Typed label edits publish one shared state without using GUI selection",
           "[colorlabels][control-commands]" )
{
    using Edit = ColorLabelsController::Edit;
    using Result = ColorLabelsController::EditResult;
    LabelViews views;
    const auto before = views.controller.snapshot();
    int notifications = 0;
    QObject::connect( &views.controller, &ColorLabelsController::labelsChanged,
                      &views.parent, [ & ] {
        ++notifications;
        CHECK( LabelViews::hasLabel( *views.main, 0, QStringLiteral( "request" ) ) );
        CHECK( LabelViews::hasLabel( *views.results, 1, QStringLiteral( "timeout" ) ) );
        CHECK( views.controller.snapshot().revision == before.revision + 1 );
    } );
    const std::vector<Edit> changes{
        { Edit::Kind::Set, 0, { QStringLiteral( "request" ) }, { false, true } },
        { Edit::Kind::Set, 1, { QStringLiteral( "timeout" ) }, { true, false } }
    };
    REQUIRE( views.controller.apply( changes, before.revision ) == Result::Applied );
    CHECK( notifications == 1 );
    const auto after = views.controller.snapshot();
    REQUIRE( after.labels[ 0 ].size() == 1 );
    CHECK_FALSE( after.labels[ 0 ].front().ignoreCase );
    CHECK( after.labels[ 0 ].front().wholeWord );
    CHECK( views.main->getSelectedText() == QStringLiteral( "Error alpha" ) );
    CHECK( views.results->getSelectedText() == QStringLiteral( "Warning beta" ) );
    CHECK( before.labels[ 0 ].isEmpty() ); // an observation is a value snapshot

    CHECK( views.controller.apply( changes, after.revision ) == Result::Unchanged );
    CHECK( notifications == 1 );
    CHECK( views.controller.snapshot().revision == after.revision );
}

TEST_CASE( "Label edits validate the complete batch before changing shared state",
           "[colorlabels][control-commands]" )
{
    using Edit = ColorLabelsController::Edit;
    using Result = ColorLabelsController::EditResult;
    LabelViews views;
    const auto before = views.controller.snapshot();
    int notifications = 0;
    QObject::connect( &views.controller, &ColorLabelsController::labelsChanged,
                      &views.parent, [ & ] { ++notifications; } );

    const auto badIndex = GENERATE( size_t{ 9 }, std::numeric_limits<size_t>::max() );
    CHECK( views.controller.apply( {
        { Edit::Kind::Set, 0, { QStringLiteral( "valid" ) }, {} },
        { Edit::Kind::Set, badIndex, { QStringLiteral( "invalid" ) }, {} }
    } ) == Result::InvalidArgument );
    CHECK( views.controller.snapshot().revision == before.revision );
    CHECK( notifications == 0 );
    CHECK_FALSE( LabelViews::hasLabel( *views.main, 0, QStringLiteral( "valid" ) ) );
}

TEST_CASE( "Explicit label edits reject oversized input rather than silently truncate",
           "[colorlabels][control-commands]" )
{
    using Edit = ColorLabelsController::Edit;
    using Result = ColorLabelsController::EditResult;
    LabelViews views;
    QStringList texts;
    for ( size_t i = 0; i <= ColorLabelsManager::MaxBulkLabelTexts; ++i ) {
        texts.append( QString::number( static_cast<qulonglong>( i ) ) );
    }
    CHECK( views.controller.apply( {
        { Edit::Kind::Set, 0, { QStringLiteral( "valid" ) }, {} },
        { Edit::Kind::Set, 1, texts, {} }
    } ) == Result::ResourceLimit );
    CHECK( views.controller.snapshot().revision == 0 );
    CHECK( views.controller.snapshot().labels[ 0 ].isEmpty() );
    CHECK( views.controller.snapshot().labels[ 1 ].isEmpty() );
}

TEST_CASE( "Human label edits invalidate stale commands even after returning to empty state",
           "[colorlabels][control-commands]" )
{
    using Edit = ColorLabelsController::Edit;
    using Result = ColorLabelsController::EditResult;
    LabelViews views;
    const auto before = views.controller.snapshot();
    Q_EMIT views.main->addColorLabel( 0 );
    const auto labelled = views.controller.snapshot();
    CHECK( labelled.revision == before.revision + 1 );
    REQUIRE( labelled.labels[ 0 ].size() == 1 );
    CHECK( labelled.labels[ 0 ].front().text == QStringLiteral( "Error alpha" ) );
    Q_EMIT views.main->removeColorLabel();
    CHECK( views.controller.snapshot().revision == labelled.revision + 1 );

    const std::vector<Edit> changes{
        { Edit::Kind::Set, 2, { QStringLiteral( "replacement" ) }, {} }
    };
    CHECK( views.controller.apply( changes, before.revision ) == Result::Conflict );
    CHECK( views.controller.snapshot().labels[ 2 ].isEmpty() );
    CHECK( views.controller.apply( changes, views.controller.snapshot().revision )
           == Result::Applied );
    Q_EMIT views.main->clearColorLabels();
    CHECK( views.controller.snapshot().labels[ 2 ].isEmpty() );
    CHECK_FALSE( LabelViews::hasLabel( *views.results, 2, QStringLiteral( "replacement" ) ) );
}

TEST_CASE( "Label command limits reject invalid requests without notifications",
           "[colorlabels][control-commands]" )
{
    using Edit = ColorLabelsController::Edit;
    using Result = ColorLabelsController::EditResult;
    QWidget parent;
    ColorLabelsController controller{ &parent, {} };
    int notifications = 0;
    QObject::connect( &controller, &ColorLabelsController::labelsChanged,
                      &parent, [ & ] { ++notifications; } );

    SECTION( "bounded command count" )
    {
        const std::vector<Edit> edits( ColorLabelsController::MaxEdits + 1 );
        CHECK( controller.apply( edits ) == Result::ResourceLimit );
    }
    SECTION( "bounded UTF-16 input size" )
    {
        const QString large( static_cast<int>( ColorLabelsController::MaxEditTextUnits ) + 1,
                             QLatin1Char( 'x' ) );
        CHECK( controller.apply( { { Edit::Kind::Set, 0, { large }, {} } } )
               == Result::ResourceLimit );
    }
    SECTION( "unknown operation is not a successful no-op" )
    {
        CHECK( controller.apply( { { static_cast<Edit::Kind>( 255 ), 0, {}, {} } } )
               == Result::InvalidArgument );
    }
    CHECK( notifications == 0 );
    CHECK( controller.snapshot().revision == 0 );
}

TEST_CASE( "Label commands skip retired views and seed later projections",
           "[colorlabels][control-commands]" )
{
    using Edit = ColorLabelsController::Edit;
    using Result = ColorLabelsController::EditResult;
    LabelViews views;
    views.results.reset();
    REQUIRE( views.controller.apply( {
        { Edit::Kind::Set, 3, { QStringLiteral( "prepared" ) }, {} }
    } ) == Result::Applied );
    LogMainView late{ &views.data, &views.pattern, nullptr, &views.overview, &views.parent };
    views.controller.watchView( &late );
    CHECK( LabelViews::hasLabel( late, 3, QStringLiteral( "prepared" ) ) );
    CHECK( views.controller.apply( {
        { Edit::Kind::Remove, 0, { QStringLiteral( "prepared" ) }, {} }
    } ) == Result::Applied );
    CHECK_FALSE( LabelViews::hasLabel( late, 3, QStringLiteral( "prepared" ) ) );
}

TEST_CASE( "File-live and folder tabs expose their existing review control owners",
           "[colorlabels][control-commands][control-tabs]" )
{
    using Edit = ColorLabelsController::Edit;
    using Result = ColorLabelsController::EditResult;
    const bool folder = GENERATE( false, true );
    QTemporaryDir directory;
    REQUIRE( directory.isValid() );
    SavedSearches history;
    auto data = std::make_shared<StreamingLogData>(
        QUuid::createUuid().toString( QUuid::WithoutBraces ), directory.path() );
    data->appendUtf8( "record\n" );
    std::unique_ptr<QWidget> host;
    if ( folder ) {
        auto widget = std::make_unique<FolderCrawlerWidget>();
        widget->setFolder( directory.path(), {} );
        host = std::move( widget );
    }
    else {
        auto widget = std::make_unique<CrawlerWidget>();
        widget->setData( data, data->getNewFilteredData() );
        widget->setQuickFindPattern( std::make_shared<QuickFindPattern>() );
        widget->setSavedSearches( &history );
        host = std::move( widget );
    }
    auto* const tab = dynamic_cast<AbstractCrawlerWidget*>( host.get() );
    REQUIRE( tab != nullptr );
    const auto controls = tab->reviewControls();
    REQUIRE( controls.filter != nullptr );
    REQUIRE( controls.labels != nullptr );
    // These must be the GUI's existing owners, not new automation-side state.
    CHECK( controls.filter == host->findChild<SearchToolbar*>() );
    CHECK( controls.labels == host->findChild<ColorLabelsController*>() );
    const auto draft = controls.filter->filterDraftSnapshot();
    const auto labels = controls.labels->snapshot();
    REQUIRE( controls.labels->apply( {
        { Edit::Kind::Set, 4, { QStringLiteral( "record" ) }, {} }
    }, labels.revision ) == Result::Applied );
    const auto logViews = host->findChildren<AbstractLogView*>();
    REQUIRE( logViews.size() >= 2 );
    for ( const auto* view : logViews ) {
        CHECK( LabelViews::hasLabel( *view, 4, QStringLiteral( "record" ) ) );
    }
    CHECK( controls.filter->filterDraftSnapshot().revision == draft.revision );
    CHECK( controls.filter->filterDraftSnapshot().filter == draft.filter );
}

TEST_CASE( "Review commits publish all view projections including panes opened while staging",
           "[colorlabels][review-transaction]" )
{
    using Edit = ColorLabelsController::Edit;
    using Status = ReviewTransaction::Status;
    LabelViews first;
    LabelViews second;
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( first.controller, {
        { Edit::Kind::Set, 1, { QStringLiteral( "first" ) }, {} }
    } ) == Status::Open );
    REQUIRE( transaction.stageLabels( second.controller, {
        { Edit::Kind::Set, 2, { QStringLiteral( "second" ) }, {} }
    } ) == Status::Open );
    LogMainView late{ &second.data, &second.pattern, nullptr, &second.overview, &second.parent };
    second.controller.watchView( &late );
    CHECK_FALSE( LabelViews::hasLabel( late, 2, QStringLiteral( "second" ) ) );
    int notifications = 0;
    QObject::connect( &first.controller, &ColorLabelsController::labelsChanged,
                      &first.parent, [ & ] {
        ++notifications;
        CHECK( LabelViews::hasLabel( *first.main, 1, QStringLiteral( "first" ) ) );
        CHECK( LabelViews::hasLabel( *first.results, 1, QStringLiteral( "first" ) ) );
        CHECK( LabelViews::hasLabel( *second.main, 2, QStringLiteral( "second" ) ) );
        CHECK( LabelViews::hasLabel( *second.results, 2, QStringLiteral( "second" ) ) );
        CHECK( LabelViews::hasLabel( late, 2, QStringLiteral( "second" ) ) );
    } );
    CHECK( transaction.commit() == Status::Committed );
    CHECK( notifications == 1 );
    CHECK( first.main->getSelectedText() == QStringLiteral( "Error alpha" ) );
    CHECK( second.results->getSelectedText() == QStringLiteral( "Warning beta" ) );
}
