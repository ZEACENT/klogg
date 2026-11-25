#include <catch2/catch.hpp>

#include <QFutureInterface>
#include <QWidget>
#include "configuration.h"
#include "reviewtransaction.h"
#include "test_utils.h"

namespace {
using Status = ReviewTransaction::Status;
using Preparation = ReviewTransaction::Preparation;

struct Owners {
    ScopedRegexpEngine engine{ RegexpEngine::QRegularExpression };
    std::optional<HighlighterSetCollection> saved;
    QWidget parent;
    ColorLabelsController labels{ &parent, {} };
    HighlighterSet existing = HighlighterSet::createNewSet( "Existing" );
    HighlighterSet proposed = HighlighterSet::fromDescription( "shared-evidence", "Evidence",
        { Highlighter{ "ERROR", false, true, QColor( Qt::black ), QColor( Qt::yellow ) } } );
    Owners()
    {
        auto& global = HighlighterSetCollection::getSynced();
        saved.emplace( global );
        global.setHighlighterSets( { existing } );
        global.deactivateAll();
    }
    ~Owners()
    {
        auto& global = HighlighterSetCollection::get();
        global = *saved;
        global.save();
    }
    static ColorLabelsController::Edit label()
    {
        return { ColorLabelsController::Edit::Kind::Set, 0, { "evidence" }, {} };
    }
};

struct DeferredPreparation {
    QFutureInterface<void> completion;
    std::promise<Preparation> result;
    std::optional<HighlighterSetCollection::Snapshot> captured;
    RegexpEngine engine = RegexpEngine::QRegularExpression;
    std::shared_ptr<std::atomic<bool>> cancelled;
    int calls = 0;

    ReviewTransaction::Preparer callback()
    {
        return [this]( ReviewTransaction::PreparationInput input ) {
            ++calls;
            captured = std::move( input.highlighters );
            engine = input.engine;
            cancelled = std::move( input.cancelled );
            completion.reportStarted();
            return ReviewTransaction::PreparationTask{ completion.future(), result.get_future() };
        };
    }
    void finish()
    {
        REQUIRE( captured );
        auto update = std::make_shared<HighlighterSetCollection::PreparedUpdate>(
            HighlighterSetCollection::prepareUpdate( *captured, engine ) );
        result.set_value( Preparation{ std::move( update ), Status::Open, {}, {} } );
        completion.reportFinished();
    }
};
}

TEST_CASE( "Global highlighter transactions read candidates without changing shared GUI state",
           "[review-transaction][review-highlighter-transaction]" )
{
    Owners owners;
    ReviewTransaction transaction;
    const auto before = HighlighterSetCollection::get().snapshot();
    REQUIRE( transaction.highlighters() );
    CHECK( transaction.stageHighlighters( { owners.proposed }, { owners.proposed.id() } ) == Status::Open );
    const auto candidate = transaction.highlighters();
    REQUIRE( candidate );
    CHECK( candidate->sets == QList<HighlighterSet>{ owners.proposed } );
    CHECK( candidate->activeSetIds == QStringList{ owners.proposed.id() } );
    CHECK( candidate->revision == before.revision );
    CHECK( HighlighterSetCollection::get().snapshot().sets == before.sets );
    CHECK( transaction.abort() == Status::Aborted );
    CHECK( HighlighterSetCollection::get().snapshot().revision == before.revision );
}

TEST_CASE( "Highlighters and labels publish together after private asynchronous preparation",
           "[review-transaction][review-highlighter-transaction]" )
{
    Owners owners;
    DeferredPreparation deferred;
    ReviewTransaction transaction{ deferred.callback() };
    const auto before = HighlighterSetCollection::get().snapshot();
    REQUIRE( transaction.stageLabels( owners.labels, { Owners::label() } ) == Status::Open );
    REQUIRE( transaction.stageHighlighters( { owners.proposed }, { owners.proposed.id() } ) == Status::Open );
    REQUIRE( transaction.commit() == Status::Preparing );
    CHECK( transaction.status() == Status::Preparing );
    CHECK( deferred.calls == 1 );
    CHECK( transaction.commit() == Status::Preparing );
    CHECK( deferred.calls == 1 );
    CHECK( owners.labels.snapshot().revision == 0 );
    CHECK( HighlighterSetCollection::get().snapshot().revision == before.revision );
    int notifications = 0;
    QObject::connect( &owners.labels, &ColorLabelsController::labelsChanged, &owners.parent, [&] {
        ++notifications;
        CHECK( HighlighterSetCollection::get().snapshot().activeSetIds == QStringList{ owners.proposed.id() } );
        CHECK( transaction.status() == Status::Committed );
        CHECK( transaction.commit() == Status::Committed );
    } );
    deferred.finish();
    CHECK( transaction.commit() == Status::Committed );
    CHECK( transaction.committedHighlighters() );
    CHECK( notifications == 1 );
    CHECK( owners.labels.snapshot().revision == 1 );
    CHECK( HighlighterSetCollection::get().snapshot().revision == before.revision + 1 );
}

TEST_CASE( "A fallible participant and prepared annotations publish all or none",
           "[review-transaction][review-highlighter-transaction]" )
{
    Owners owners;
    DeferredPreparation deferred;
    const auto before = HighlighterSetCollection::get().revision();
    Status validation = Status::Open;
    bool installable = true;
    QStringList phases;
    ReviewTransaction transaction{ deferred.callback() };
    REQUIRE( transaction.stageLabels( owners.labels, { Owners::label() } ) == Status::Open );
    REQUIRE( transaction.stageHighlighters( { owners.proposed }, { owners.proposed.id() } ) == Status::Open );
    auto permit = std::make_shared<int>( 1 );
    const std::weak_ptr<int> retained = permit;
    REQUIRE( transaction.stageParticipant( {
        [&, permit = std::move( permit )] { CHECK( permit ); phases << "validate"; return validation; },
        [&] {
            phases << "install";
            CHECK( transaction.commit() == Status::Preparing );
            CHECK( transaction.abort() == Status::Preparing );
            CHECK( HighlighterSetCollection::get().revision() == before );
            CHECK( owners.labels.snapshot().revision == 0 );
            return installable;
        },
        [&] { phases << "release"; },
        [&] {
            phases << "notify";
            CHECK_FALSE( retained.expired() ); // No early admission for reentrant observers.
            CHECK( transaction.abort() == Status::Committed );
            CHECK( owners.labels.snapshot().revision == 1 );
            CHECK( HighlighterSetCollection::get().revision() == before + 1 );
        } } ) == Status::Open );
    REQUIRE( transaction.commit() == Status::Preparing );
    REQUIRE( transaction.commit() == Status::Preparing );
    CHECK( phases.isEmpty() ); // Never acquire a native guard across event-loop turns.
    deferred.finish();
    SECTION( "validation failure releases even without installation" ) {
        validation = Status::TargetClosed;
        CHECK( transaction.commit() == validation );
        CHECK( phases == QStringList{ "validate", "release" } );
    }
    SECTION( "fallible adoption leaves annotations untouched" ) {
        installable = false;
        CHECK( transaction.commit() == Status::Conflict );
        CHECK( phases == QStringList{ "validate", "install", "release" } );
    }
    SECTION( "terminal observers can see all owners" ) {
        CHECK( transaction.commit( [&] {
            CHECK( transaction.status() == Status::Committed );
            phases << "published";
        } ) == Status::Committed );
        CHECK( phases == QStringList{ "validate", "install", "release", "published", "notify" } );
    }
    if ( transaction.status() != Status::Committed ) {
        CHECK( owners.labels.snapshot().revision == 0 );
        CHECK( HighlighterSetCollection::get().revision() == before );
    }
}

TEST_CASE( "Human edits during highlighter preparation reject every staged owner",
           "[review-transaction][review-highlighter-transaction]" )
{
    Owners owners;
    DeferredPreparation deferred;
    ReviewTransaction transaction{ deferred.callback() };
    REQUIRE( transaction.stageLabels( owners.labels, { Owners::label() } ) == Status::Open );
    REQUIRE( transaction.stageHighlighters( { owners.proposed }, { owners.proposed.id() } ) == Status::Open );
    REQUIRE( transaction.commit() == Status::Preparing );
    SECTION( "global highlighter ABA" ) {
        auto& global = HighlighterSetCollection::get();
        global.activateSet( owners.existing.id() );
        global.deactivateSet( owners.existing.id() );
    }
    SECTION( "label edit" ) {
        auto edit = Owners::label();
        edit.texts = QStringList{ QStringLiteral( "human" ) };
        REQUIRE( owners.labels.apply( { edit } ) == ColorLabelsController::EditResult::Applied );
    }
    const auto humanHighlighters = HighlighterSetCollection::get().snapshot();
    const auto humanLabels = owners.labels.snapshot();
    deferred.finish();
    CHECK( transaction.commit() == Status::Conflict );
    CHECK_FALSE( transaction.committedHighlighters() );
    CHECK( owners.labels.snapshot().revision == humanLabels.revision );
    CHECK( HighlighterSetCollection::get().snapshot().revision == humanHighlighters.revision );
    CHECK( HighlighterSetCollection::get().snapshot().sets == humanHighlighters.sets );
}

TEST_CASE( "Closing or cancelling pending review work cannot publish global highlighters",
           "[review-transaction][review-highlighter-transaction]" )
{
    Owners owners;
    DeferredPreparation deferred;
    ReviewTransaction transaction{ deferred.callback() };
    auto target = std::make_unique<ColorLabelsController>( &owners.parent, nullptr );
    const auto before = HighlighterSetCollection::get().snapshot();
    REQUIRE( transaction.stageLabels( *target, { Owners::label() } ) == Status::Open );
    REQUIRE( transaction.stageHighlighters( { owners.proposed }, { owners.proposed.id() } ) == Status::Open );
    REQUIRE( transaction.commit() == Status::Preparing );
    Status expected = Status::TargetClosed;
    SECTION( "closed tab owner" ) { target.reset(); }
    SECTION( "abort before compilation finishes" ) {
        CHECK( transaction.abort() == Status::Aborted );
        REQUIRE( deferred.cancelled );
        CHECK( deferred.cancelled->load() );
        expected = Status::Aborted;
    }
    deferred.finish();
    CHECK( transaction.commit() == expected );
    CHECK( HighlighterSetCollection::get().snapshot().revision == before.revision );
    CHECK( HighlighterSetCollection::get().snapshot().sets == before.sets );
}

TEST_CASE( "Default highlighter preparation completes and publishes before label observers",
           "[review-transaction][review-highlighter-transaction]" )
{
    Owners owners;
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( owners.labels, { Owners::label() } ) == Status::Open );
    REQUIRE( transaction.stageHighlighters( { owners.proposed }, { owners.proposed.id() } ) == Status::Open );
    REQUIRE( transaction.commit() == Status::Preparing );
    int publications = 0;
    QObject::connect( &owners.labels, &ColorLabelsController::labelsChanged, &owners.parent, [&] {
        CHECK( publications == 1 );
        CHECK( transaction.status() == Status::Committed );
    } );
    const auto publish = [&] {
        ++publications;
        CHECK( transaction.committedHighlighters() );
        CHECK( owners.labels.snapshot().revision == 1 );
        CHECK( HighlighterSetCollection::get().activeSetIds() == QStringList{ owners.proposed.id() } );
    };
    REQUIRE( waitUiState( [&] { return transaction.commit( publish ) != Status::Preparing; } ) );
    CHECK( transaction.status() == Status::Committed );
    CHECK( publications == 1 );
    CHECK( transaction.commit( publish ) == Status::Committed );
    CHECK( publications == 1 );
}

TEST_CASE( "Failed private highlighter preparation leaves all owners unchanged",
           "[review-transaction][review-highlighter-transaction]" )
{
    Owners owners;
    DeferredPreparation deferred;
    ReviewTransaction transaction{ deferred.callback() };
    const auto proposed = HighlighterSet::fromDescription( "invalid", "Invalid",
        { Highlighter{ "(", false, true, QColor( Qt::black ), QColor( Qt::yellow ) } } );
    Status expected = Status::PreparationFailed;
    const auto before = HighlighterSetCollection::get().snapshot();
    REQUIRE( transaction.stageLabels( owners.labels, { Owners::label() } ) == Status::Open );
    REQUIRE( transaction.stageHighlighters( { proposed }, { proposed.id() } ) == Status::Open );
    REQUIRE( transaction.commit() == Status::Preparing );
    SECTION( "invalid compiled pattern" ) {
        expected = Status::InvalidArgument;
        deferred.finish();
    }
    SECTION( "completion without an outcome must not wait" ) { deferred.completion.reportFinished(); }
    SECTION( "abandoned producer" ) {
        deferred.result = std::promise<Preparation>{};
        deferred.completion.reportFinished();
    }
    CHECK( transaction.commit() == expected );
    CHECK( owners.labels.snapshot().revision == 0 );
    CHECK( HighlighterSetCollection::get().revision() == before.revision );
}

TEST_CASE( "Read-only global dependencies do not compile but still detect conflicts",
           "[review-transaction][review-highlighter-transaction]" )
{
    Owners owners;
    DeferredPreparation deferred;
    ReviewTransaction transaction{ deferred.callback() };
    REQUIRE( transaction.highlighters() );
    REQUIRE( transaction.stageLabels( owners.labels, { Owners::label() } ) == Status::Open );
    HighlighterSetCollection::get().activateSet( owners.existing.id() );
    CHECK( transaction.commit() == Status::Conflict );
    CHECK( deferred.calls == 0 );
    CHECK( owners.labels.snapshot().revision == 0 );
}
