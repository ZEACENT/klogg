#include <catch2/catch.hpp>

#include <future>
#include "configuration.h"
#include "highlighterset.h"
#include "test_utils.h"

namespace {
using Result = HighlighterSetCollection::UpdateResult;

Highlighter rule( const QString& pattern = QStringLiteral( "ERROR" ) )
{
    return { pattern, false, true, QColor( Qt::black ), QColor( Qt::yellow ) };
}

void sameState( const HighlighterSetCollection::Snapshot& actual,
                const HighlighterSetCollection::Snapshot& expected )
{
    CHECK( actual.revision == expected.revision );
    CHECK( actual.sets == expected.sets );
    CHECK( actual.activeSetIds == expected.activeSetIds );
    CHECK( actual.quickHighlighters == expected.quickHighlighters );
    CHECK( actual.quickHighlighterDefaults == expected.quickHighlighterDefaults );
}
}

TEST_CASE( "Highlighter descriptions preserve shared set identity and ordered rules",
           "[highlighter-publication]" )
{
    const QList<Highlighter> rules{ rule(), rule( QStringLiteral( "WARN" ) ) };
    const auto set = HighlighterSet::fromDescription( "shared-id", "Shared evidence", rules );
    CHECK( set.id() == QStringLiteral( "shared-id" ) );
    CHECK( set.name() == QStringLiteral( "Shared evidence" ) );
    CHECK( set.highlighters() == rules );
}

TEST_CASE( "Highlighter snapshots detach mutable cache owners before crossing threads",
           "[highlighter-publication][highlighter-snapshot-isolation]" )
{
    ScopedRegexpEngine engine( RegexpEngine::QRegularExpression );
    HighlighterSetCollection owner;
    const auto set = HighlighterSet::fromDescription( "shared", "Shared", { rule() } );
    owner.setHighlighterSets( { set } );
    const auto liveSets = owner.highlighterSets();
    const auto captured = owner.snapshot();
    REQUIRE( liveSets.size() == 1 );
    REQUIRE( captured.sets.size() == 1 );
    // Both classes contain mutable lazy matcher caches: copying their QString
    // payload is safe, sharing the containing object with GUI paint is not.
    CHECK( &liveSets.at( 0 ) != &captured.sets.at( 0 ) );
    const auto liveRules = liveSets.at( 0 ).highlighters();
    const auto frozenRules = captured.sets.at( 0 ).highlighters();
    REQUIRE( liveRules.size() == 1 );
    REQUIRE( frozenRules.size() == 1 );
    CHECK( &liveRules.at( 0 ) != &frozenRules.at( 0 ) );
    CHECK( liveRules == frozenRules );
    CHECK( captured.revision == owner.snapshot().revision );
}

TEST_CASE( "Control snapshot admission bounds GUI imports without hiding malformed stored state",
           "[highlighter-publication][highlighter-read-bounds]" )
{
    HighlighterSetCollection owner;
    const auto malformed = HighlighterSet::fromDescription( {}, "Imported", {} );
    owner.setHighlighterSets( { malformed } );
    REQUIRE( owner.boundedSnapshot() );
    SECTION( "set count even when IDs are malformed" ) {
        QList<HighlighterSet> sets;
        for ( int i = 0; i < 129; ++i ) sets.append( malformed );
        owner.setHighlighterSets( sets );
    }
    SECTION( "rule count" ) {
        QList<Highlighter> rules;
        for ( int i = 0; i < 1025; ++i ) rules.append( rule() );
        owner.setHighlighterSets( { HighlighterSet::fromDescription( "id", "Imported", rules ) } );
    }
    SECTION( "text budget" ) {
        owner.setHighlighterSets( { HighlighterSet::fromDescription( "id",
            QString( static_cast<int>( HighlighterSetCollection::MaxPreparedTextUnits ), 'x' ), {} ) } );
    }
    SECTION( "palette cardinality" ) {
        QList<QuickHighlighter> palette;
        for ( int i = 0; i < 129; ++i ) palette.append( { "Imported", { Qt::black, Qt::yellow }, false } );
        owner.setQuickHighlighters( palette );
    }
    CHECK_FALSE( owner.boundedSnapshot() );
}

TEST_CASE( "Highlighters prepare privately with a captured backend and install once",
           "[highlighter-publication]" )
{
    ScopedRegexpEngine engine( RegexpEngine::QRegularExpression );
    HighlighterSetCollection owner;
    owner.setHighlighterSets( { HighlighterSet::createNewSet( "Existing" ) } );
    const auto before = owner.snapshot();
    auto candidate = before;
    candidate.sets.append( HighlighterSet::fromDescription( "agent-evidence", "Evidence", { rule() } ) );
    candidate.activeSetIds = QStringList{ QStringLiteral( "agent-evidence" ) };
    auto future = std::async( std::launch::async, [candidate] {
        return HighlighterSetCollection::prepareUpdate( candidate, RegexpEngine::QRegularExpression );
    } );
    REQUIRE( waitUiState( [&] { return future.wait_for( std::chrono::milliseconds( 0 ) ) == std::future_status::ready; } ) );
    auto prepared = future.get();
    REQUIRE_FALSE( prepared.error() );
    sameState( owner.snapshot(), before );
    Configuration::get().setRegexpEnging( RegexpEngine::Vectorscan );
    CHECK( owner.applyPrepared( std::move( prepared ), before.revision ) == Result::Applied );
    CHECK( owner.snapshot().revision == before.revision + 1 );
    CHECK( owner.snapshot().sets == candidate.sets );
    CHECK( owner.snapshot().activeSetIds == candidate.activeSetIds );
    HighlightedMatchRanges matches;
    CHECK( owner.currentActiveSet().matchLine( "ERROR evidence", matches ) == HighlighterMatchType::WordMatch );
    const auto committed = owner.snapshot();
    CHECK( owner.applyPrepared( std::move( prepared ), committed.revision ) == Result::InvalidArgument );
    sameState( owner.snapshot(), committed );
}

TEST_CASE( "Moving a prepared highlighter update transfers its installation authority",
           "[highlighter-publication][highlighter-publication-move]" )
{
    ScopedRegexpEngine engine( RegexpEngine::QRegularExpression );
    HighlighterSetCollection owner;
    owner.setHighlighterSets( { HighlighterSet::createNewSet( "Existing" ) } );
    const auto before = owner.snapshot();
    auto candidate = before;
    candidate.sets.clear();
    auto prepared = HighlighterSetCollection::prepareUpdate( candidate, RegexpEngine::QRegularExpression );
    REQUIRE_FALSE( prepared.error() );
    std::optional<HighlighterSetCollection::PreparedUpdate> moved;
    SECTION( "move construction" ) { moved.emplace( std::move( prepared ) ); }
    SECTION( "move assignment" ) {
        moved.emplace( HighlighterSetCollection::prepareUpdate( before, RegexpEngine::QRegularExpression ) );
        *moved = std::move( prepared );
    }
    CHECK( prepared.error() == Result::InvalidArgument );
    CHECK( owner.applyPrepared( std::move( prepared ), before.revision ) == Result::InvalidArgument );
    sameState( owner.snapshot(), before );
    CHECK( owner.applyPrepared( std::move( *moved ), before.revision ) == Result::Applied );
    CHECK( owner.snapshot().sets.isEmpty() );
    CHECK( owner.snapshot().revision == before.revision + 1 );
}

TEST_CASE( "Prepared highlighter publication is revision guarded and no-op neutral",
           "[highlighter-publication]" )
{
    ScopedRegexpEngine engine( RegexpEngine::QRegularExpression );
    HighlighterSetCollection owner;
    const auto set = HighlighterSet::createNewSet( "Existing" );
    owner.setHighlighterSets( { set } );
    const auto before = owner.snapshot();
    auto prepared = HighlighterSetCollection::prepareUpdate( before, RegexpEngine::QRegularExpression );
    REQUIRE_FALSE( prepared.error() );
    SECTION( "equal content does not advance revision" ) {
        CHECK( owner.applyPrepared( std::move( prepared ), before.revision ) == Result::Unchanged );
        sameState( owner.snapshot(), before );
    }
    SECTION( "human ABA edits reject the captured revision" ) {
        owner.activateSet( set.id() );
        owner.deactivateSet( set.id() );
        const auto human = owner.snapshot();
        REQUIRE( human.revision == before.revision + 2 );
        CHECK( owner.applyPrepared( std::move( prepared ), before.revision ) == Result::Conflict );
        sameState( owner.snapshot(), human );
    }
}

TEST_CASE( "Invalid highlighter candidates cannot replace committed state",
           "[highlighter-publication]" )
{
    ScopedRegexpEngine engine( RegexpEngine::QRegularExpression );
    HighlighterSetCollection owner;
    const auto set = HighlighterSet::createNewSet( "Existing" );
    owner.setHighlighterSets( { set } );
    const auto before = owner.snapshot();
    auto candidate = before;
    SECTION( "duplicate set IDs" ) { candidate.sets.append( set ); }
    SECTION( "unknown active set" ) { candidate.activeSetIds = QStringList{ QStringLiteral( "missing" ) }; }
    SECTION( "duplicate active IDs" ) { candidate.activeSetIds = QStringList{ set.id(), set.id() }; }
    SECTION( "empty set identity" ) {
        candidate.sets = { HighlighterSet::fromDescription( {}, "Invalid", {} ) };
    }
    SECTION( "invalid regular expression" ) {
        candidate.sets = { HighlighterSet::fromDescription( "invalid", "Invalid", { rule( "(" ) } ) };
    }
    auto prepared = HighlighterSetCollection::prepareUpdate( candidate, RegexpEngine::QRegularExpression );
    REQUIRE( prepared.error() == Result::InvalidArgument );
    CHECK( owner.applyPrepared( std::move( prepared ), before.revision ) == Result::InvalidArgument );
    sameState( owner.snapshot(), before );
}

TEST_CASE( "Highlighter preparation bounds collection work before compiling",
           "[highlighter-publication]" )
{
    HighlighterSetCollection::Snapshot candidate;
    SECTION( "set count" ) {
        for ( int index = 0; index < 129; ++index )
            candidate.sets.append( HighlighterSet::createNewSet( "Bounded" ) );
    }
    SECTION( "rule count" ) {
        QList<Highlighter> rules;
        for ( int index = 0; index < 1025; ++index ) rules.append( rule() );
        candidate.sets = { HighlighterSet::fromDescription( "bounded", "Bounded", rules ) };
    }
    auto prepared = HighlighterSetCollection::prepareUpdate( candidate, RegexpEngine::QRegularExpression );
    CHECK( prepared.error() == Result::ResourceLimit );
}
