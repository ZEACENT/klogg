#include <catch2/catch.hpp>

#include <QWidget>
#include <memory>
#include <stdexcept>

#include "colorlabelscontroller.h"
#include "reviewtransaction.h"
#include "searchtoolbar.h"

namespace {
using Edit = ColorLabelsController::Edit;
using Status = ReviewTransaction::Status;

Edit label( const QString& text, size_t color = 0 )
{
    return { Edit::Kind::Set, color, { text }, { false, true } };
}

struct MarkOwner : QObject, klogg::ReviewMarksOwner {
    klogg::ReviewMarksSnapshot state;
    bool exceedsBudget = false;
    QObject* reviewMarksObject() override { return this; }
    uint64_t reviewMarksRevision() const override { return state.revision; }
    std::optional<klogg::ReviewMarksSnapshot> reviewMarksSnapshot() const override
    {
        return exceedsBudget ? std::optional<klogg::ReviewMarksSnapshot>{} : state;
    }
};

struct Owners {
    QWidget parent;
    ColorLabelsController first{ &parent, {} };
    ColorLabelsController second{ &parent, {} };
    ColorLabelsController unrelated{ &parent, {} };
    SearchToolbar filter{ &parent };
};
} // namespace

TEST_CASE( "An asynchronous stage reservation blocks prefix publication without closing the transaction",
           "[review-transaction]" )
{
    Owners owners;
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( owners.first, { label( "pending" ) } ) == Status::Open );
    int cancellations = 0;
    REQUIRE( transaction.beginAsyncStage( [&] {
        ++cancellations;
        CHECK_FALSE( transaction.asyncStagePending() );
        CHECK( transaction.abort() == transaction.status() );
    } ) == Status::Open );
    CHECK( transaction.asyncStagePending() );
    CHECK( transaction.beginAsyncStage() == Status::ResourceLimit );
    CHECK( transaction.commit() == Status::Open );
    CHECK( owners.first.snapshot().revision == 0 );
    SECTION( "completed staging can commit" ) {
        transaction.finishAsyncStage();
        CHECK( transaction.commit() == Status::Committed );
        CHECK( owners.first.snapshot().revision == 1 );
    }
    SECTION( "failed staging rejects the prefix" ) {
        transaction.failAsyncStage( Status::TargetClosed );
        CHECK( transaction.commit() == Status::TargetClosed );
        CHECK( owners.first.snapshot().revision == 0 );
    }
    SECTION( "late completion does not reopen an aborted transaction" ) {
        CHECK( transaction.abort() == Status::Aborted );
        transaction.finishAsyncStage();
        transaction.failAsyncStage( Status::TargetClosed );
        CHECK( transaction.commit() == Status::Aborted );
    }
    CHECK_FALSE( transaction.asyncStagePending() );
    CHECK( cancellations == ( transaction.status() == Status::Committed ? 0 : 1 ) );
}

TEST_CASE( "Destroying an unfinished transaction releases its participant even if cancellation throws",
           "[review-transaction]" )
{
    int cancellations = 0;
    int releases = 0;
    {
        ReviewTransaction transaction;
        REQUIRE( transaction.stageParticipant( {
            [] { return Status::Open; }, [] { return true; },
            [&] { ++releases; throw std::runtime_error( "release" ); }, [] {} } ) == Status::Open );
        REQUIRE( transaction.beginAsyncStage( [&] {
            ++cancellations;
            throw std::runtime_error( "cancel" );
        } ) == Status::Open );
    }
    CHECK( cancellations == 1 );
    CHECK( releases == 1 );
}

TEST_CASE( "Only one fallible participant can publish a transaction-visible filter draft",
           "[review-transaction]" )
{
    Owners owners;
    int releases = 0;
    int validations = 0;
    ReviewTransaction transaction;
    const auto base = transaction.filterDraft( owners.filter );
    REQUIRE( base );
    auto filter = base->filter;
    filter.matchCase = !filter.matchCase;
    ReviewTransaction::Participant participant{
        [&] { ++validations; return Status::Open; },
        [&] { return owners.filter.installFilterDraft( *base, filter ); },
        [&] { ++releases; }, [] {} };
    REQUIRE( transaction.stageParticipant( participant ) == Status::Open );
    REQUIRE( transaction.stageFilterDraft( owners.filter, filter ) == Status::Open );
    const auto staged = transaction.filterDraft( owners.filter );
    REQUIRE( staged );
    CHECK( staged->filter == filter );
    CHECK( staged->revision == base->revision );
    CHECK( owners.filter.filterDraftSnapshot().filter == base->filter );
    SECTION( "original dependency is still checked" ) {
        owners.filter.setMatchCase( filter.matchCase );
        owners.filter.setMatchCase( base->filter.matchCase );
        CHECK( transaction.commit() == Status::Conflict );
        CHECK( validations == 0 );
    }
    SECTION( "second participant rejects whole candidate" ) {
        CHECK( transaction.stageParticipant( participant ) == Status::ResourceLimit );
        CHECK( transaction.commit() == Status::ResourceLimit );
        CHECK( validations == 0 );
    }
    SECTION( "abort before validation releases staged resources" ) {
        CHECK( transaction.abort() == Status::Aborted );
        CHECK( validations == 0 );
    }
    SECTION( "only the participant installs the original dependency's draft" ) {
        CHECK( transaction.commit() == Status::Committed );
        CHECK( validations == 1 );
        CHECK( owners.filter.filterDraftSnapshot().filter == filter );
    }
    CHECK( releases == 1 );
}

TEST_CASE( "Mark read dependencies are repeatable and reject ABA edits before label publication",
           "[review-transaction][review-marks-transaction]" )
{
    Owners owners;
    MarkOwner marks;
    ReviewTransaction transaction;
    transaction.stageLabels( owners.first, { label( "staged" ) } );
    const auto captured = transaction.marks( marks );
    REQUIRE( captured );
    CHECK( captured->groups.empty() );
    marks.state.revision += 2; // Human add/remove returns to the same collection.
    const auto repeated = transaction.marks( marks );
    REQUIRE( repeated );
    CHECK( repeated->revision == captured->revision );
    CHECK( transaction.commit() == Status::Conflict );
    CHECK( owners.first.snapshot().labels.front().isEmpty() );
}

TEST_CASE( "Closed or oversized mark owners fail the entire candidate without dereferencing retired models",
           "[review-transaction][review-marks-transaction]" )
{
    Owners owners;
    auto marks = std::make_unique<MarkOwner>();
    ReviewTransaction transaction;
    transaction.stageLabels( owners.first, { label( "staged" ) } );
    SECTION( "owner lifetime" ) {
        REQUIRE( transaction.marks( *marks ) );
        marks.reset();
        CHECK( transaction.commit() == Status::TargetClosed );
    }
    SECTION( "bounded snapshot failure" ) {
        marks->exceedsBudget = true;
        CHECK_FALSE( transaction.marks( *marks ) );
        CHECK( transaction.commit() == Status::ResourceLimit );
    }
    CHECK( owners.first.snapshot().labels.front().isEmpty() );
}

TEST_CASE( "Mark read owners share transaction admission and unrelated owners do not conflict",
           "[review-transaction][review-marks-transaction]" )
{
    Owners owners;
    MarkOwner unrelated;
    ReviewTransaction transaction;
    transaction.stageLabels( owners.first, { label( "staged" ) } );
    unrelated.state.revision += 1;
    CHECK( transaction.commit() == Status::Committed );
    ReviewTransaction bounded;
    std::vector<std::unique_ptr<MarkOwner>> marks;
    for ( size_t i = 0; i < ReviewTransaction::MaxOwners; ++i ) {
        marks.push_back( std::make_unique<MarkOwner>() );
        REQUIRE( bounded.marks( *marks.back() ) );
    }
    CHECK( bounded.marks( *marks.front() ) ); // Repeat reads do not consume another slot.
    CHECK_FALSE( bounded.marks( unrelated ) );
    CHECK( bounded.status() == Status::ResourceLimit );
}

TEST_CASE( "Review transactions read their own label writes without publishing",
           "[review-transaction]" )
{
    Owners owners;
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( owners.first, { label( "one" ) } ) == Status::Open );
    REQUIRE( transaction.stageLabels( owners.first, { label( "two", 1 ) } ) == Status::Open );
    const auto staged = transaction.labels( owners.first );
    REQUIRE( staged );
    REQUIRE( staged->labels[ 0 ].size() == 1 );
    REQUIRE( staged->labels[ 1 ].size() == 1 );
    CHECK( owners.first.snapshot().labels[ 0 ].isEmpty() );
    CHECK( owners.first.snapshot().revision == 0 );
    transaction.abort();
    CHECK( transaction.status() == Status::Aborted );
    CHECK( transaction.commit() == Status::Aborted );
    CHECK( owners.first.snapshot().labels[ 0 ].isEmpty() );
}

TEST_CASE( "Review transactions install every label owner before notifying any observer",
           "[review-transaction]" )
{
    Owners owners;
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( owners.first, { label( "one" ) } ) == Status::Open );
    REQUIRE( transaction.stageLabels( owners.second, { label( "two", 1 ) } ) == Status::Open );
    int notifications = 0;
    const auto observe = [ & ] {
        ++notifications;
        REQUIRE( owners.first.snapshot().labels[ 0 ].size() == 1 );
        REQUIRE( owners.second.snapshot().labels[ 1 ].size() == 1 );
        CHECK( transaction.status() == Status::Committed );
        CHECK( transaction.commit() == Status::Committed ); // never replay on reentry
    };
    QObject::connect( &owners.first, &ColorLabelsController::labelsChanged, &owners.parent, observe );
    QObject::connect( &owners.second, &ColorLabelsController::labelsChanged, &owners.parent, observe );
    CHECK( transaction.commit() == Status::Committed );
    CHECK( notifications == 2 );
    CHECK( owners.first.snapshot().revision == 1 );
    CHECK( owners.second.snapshot().revision == 1 );
    CHECK( transaction.commit() == Status::Committed );
    CHECK( notifications == 2 );
}

TEST_CASE( "Conflicting GUI edits prevent every staged owner from publishing",
           "[review-transaction]" )
{
    Owners owners;
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( owners.first, { label( "one" ) } ) == Status::Open );
    REQUIRE( transaction.stageLabels( owners.second, { label( "two" ) } ) == Status::Open );
    REQUIRE( owners.second.apply( { label( "human", 2 ) } )
             == ColorLabelsController::EditResult::Applied );
    CHECK( transaction.commit() == Status::Conflict );
    CHECK( owners.first.snapshot().revision == 0 );
    CHECK( owners.second.snapshot().labels[ 0 ].isEmpty() );
    REQUIRE( owners.second.snapshot().labels[ 2 ].size() == 1 );
    CHECK( owners.second.snapshot().labels[ 2 ].front().text == QStringLiteral( "human" ) );
}

TEST_CASE( "Review transactions validate read dependencies but ignore unrelated owners",
           "[review-transaction]" )
{
    Owners owners;
    ReviewTransaction transaction;
    REQUIRE( transaction.filterDraft( owners.filter ) );
    REQUIRE( transaction.labels( owners.second ) );
    REQUIRE( transaction.stageLabels( owners.first, { label( "one" ) } ) == Status::Open );
    SECTION( "read-only filter ABA conflicts" )
    {
        const bool original = owners.filter.isMatchCase();
        owners.filter.setMatchCase( !original );
        owners.filter.setMatchCase( original );
        CHECK( transaction.commit() == Status::Conflict );
        CHECK( owners.first.snapshot().revision == 0 );
    }
    SECTION( "read-only labels conflict" )
    {
        owners.second.apply( { label( "human" ) } );
        CHECK( transaction.commit() == Status::Conflict );
        CHECK( owners.first.snapshot().revision == 0 );
    }
    SECTION( "unrelated state does not conflict" )
    {
        owners.unrelated.apply( { label( "human" ) } );
        CHECK( transaction.commit() == Status::Committed );
        CHECK( owners.first.snapshot().revision == 1 );
    }
}

TEST_CASE( "Invalid staged commands abort the whole review transaction",
           "[review-transaction]" )
{
    Owners owners;
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( owners.first, { label( "valid" ) } ) == Status::Open );
    CHECK( transaction.stageLabels( owners.second, { label( "invalid", 9 ) } )
           == Status::InvalidArgument );
    CHECK( transaction.commit() == Status::InvalidArgument );
    CHECK( owners.first.snapshot().revision == 0 );
    CHECK( owners.second.snapshot().revision == 0 );
}

TEST_CASE( "Closing any dependency aborts without touching surviving targets",
           "[review-transaction]" )
{
    Owners owners;
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( owners.first, { label( "one" ) } ) == Status::Open );
    SECTION( "closed label owner" )
    {
        auto target = std::make_unique<ColorLabelsController>( &owners.parent, nullptr );
        REQUIRE( transaction.stageLabels( *target, { label( "two" ) } ) == Status::Open );
        target.reset();
    }
    SECTION( "closed filter owner" )
    {
        auto target = std::make_unique<SearchToolbar>( &owners.parent );
        REQUIRE( transaction.filterDraft( *target ) );
        target.reset();
    }
    CHECK( transaction.commit() == Status::TargetClosed );
    CHECK( owners.first.snapshot().revision == 0 );
}

TEST_CASE( "Review transaction budgets apply across staging calls",
           "[review-transaction]" )
{
    Owners owners;
    ReviewTransaction transaction;
    SECTION( "command budget cannot be bypassed with many small batches" )
    {
        for ( size_t i = 0; i < ColorLabelsController::MaxEdits; ++i ) {
            REQUIRE( transaction.stageLabels( owners.first, { label( "same" ) } ) == Status::Open );
        }
        CHECK( transaction.stageLabels( owners.second, { label( "one too many" ) } )
               == Status::ResourceLimit );
    }
    SECTION( "text budget is shared across owners" )
    {
        const QString text( static_cast<int>( ColorLabelsController::MaxEditTextUnits / 2 ),
                            QLatin1Char( 'x' ) );
        REQUIRE( transaction.stageLabels( owners.first, { label( text ) } ) == Status::Open );
        REQUIRE( transaction.stageLabels( owners.second, { label( text ) } ) == Status::Open );
        CHECK( transaction.stageLabels( owners.first, { label( "x" ) } ) == Status::ResourceLimit );
    }
    CHECK( transaction.commit() == Status::ResourceLimit );
    CHECK( owners.first.snapshot().revision == 0 );
    CHECK( owners.second.snapshot().revision == 0 );
}

TEST_CASE( "The bulk-label budget covers a whole batch and transaction",
           "[review-transaction][colorlabels]" )
{
    Owners owners;
    QStringList texts;
    for ( size_t i = 0; i < ColorLabelsManager::MaxBulkLabelTexts; ++i ) {
        texts.append( QString::number( static_cast<qulonglong>( i ) ) );
    }
    const Edit full{ Edit::Kind::Set, 0, texts, {} };
    const auto extra = label( "extra" );
    SECTION( "one explicit batch" )
    {
        CHECK( owners.first.apply( { full, extra } )
               == ColorLabelsController::EditResult::ResourceLimit );
    }
    SECTION( "separate transaction staging calls" )
    {
        ReviewTransaction transaction;
        REQUIRE( transaction.stageLabels( owners.first, { full } ) == Status::Open );
        CHECK( transaction.stageLabels( owners.second, { extra } ) == Status::ResourceLimit );
        CHECK( transaction.commit() == Status::ResourceLimit );
    }
    CHECK( owners.first.snapshot().revision == 0 );
    CHECK( owners.second.snapshot().revision == 0 );
}

TEST_CASE( "Review reads retain their observed base instead of refreshing preconditions",
           "[review-transaction]" )
{
    Owners owners;
    ReviewTransaction transaction;
    const auto labels = transaction.labels( owners.first );
    const auto filter = transaction.filterDraft( owners.filter );
    REQUIRE( labels );
    REQUIRE( filter );
    owners.first.apply( { label( "new" ) } );
    owners.filter.setMatchCase( !filter->filter.matchCase );
    const auto labelsAgain = transaction.labels( owners.first );
    const auto filterAgain = transaction.filterDraft( owners.filter );
    REQUIRE( labelsAgain );
    REQUIRE( filterAgain );
    CHECK( labelsAgain->revision == labels->revision );
    CHECK( labelsAgain->labels[ 0 ].isEmpty() );
    CHECK( filterAgain->revision == filter->revision );
    CHECK( filterAgain->filter == filter->filter );
    CHECK( transaction.commit() == Status::Conflict );
}

TEST_CASE( "An observer may close a committed owner before its deferred notification",
           "[review-transaction]" )
{
    Owners owners;
    auto closing = std::make_unique<ColorLabelsController>( &owners.parent, nullptr );
    ReviewTransaction transaction;
    REQUIRE( transaction.stageLabels( owners.first, { label( "first" ) } ) == Status::Open );
    REQUIRE( transaction.stageLabels( *closing, { label( "second" ) } ) == Status::Open );
    QObject::connect( &owners.first, &ColorLabelsController::labelsChanged,
                      &owners.parent, [ & ] {
        REQUIRE( closing->snapshot().labels[ 0 ].size() == 1 );
        closing.reset();
    } );
    CHECK( transaction.commit() == Status::Committed );
    CHECK_FALSE( closing );
}
