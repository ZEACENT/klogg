#include <catch2/catch.hpp>
#include <QJsonArray>
#include "reviewhighlightercodec.h"

using namespace klogg::review;

namespace {
QJsonObject ruleJson()
{
    return { { "pattern", "ERROR" }, { "foreground", "#ff000000" },
             { "background", "#ffffff00" } };
}
QJsonObject setJson( const QJsonObject& rule = ruleJson() )
{
    return { { "id", "shared" }, { "name", "Shared evidence" }, { "rules", QJsonArray{ rule } } };
}
HighlighterDecodeError error( const DecodedHighlighterReplacement& result )
{
    REQUIRE( std::holds_alternative<HighlighterDecodeError>( result ) );
    return std::get<HighlighterDecodeError>( result );
}
}

TEST_CASE( "Highlighter wire state preserves identities order colors and all rule flags",
           "[control-highlighter-codec]" )
{
    Highlighter first( "ERROR", true, true, QColor( "#12345678" ), QColor( Qt::yellow ) );
    first.setUseRegex( false );
    first.setVariateColors( true );
    first.setColorVariance( 27 );
    HighlighterSetCollection::Snapshot state;
    state.revision = 18446744073709551615ULL;
    state.sets = { HighlighterSet::fromDescription( "first", "First", { first } ),
                   HighlighterSet::fromDescription( "second", "Second", {} ) };
    state.activeSetIds = QStringList{ QStringLiteral( "second" ), QStringLiteral( "first" ) };
    state.quickHighlighters = { { "Label", { QColor( Qt::black ), QColor( Qt::yellow ) }, true } };
    state.quickHighlighterDefaults = { false, true };
    const auto json = encodeHighlighters( state );
    CHECK( json.value( "revision" ) == QLatin1String( "18446744073709551615" ) );
    const auto sets = json.value( "sets" ).toArray();
    REQUIRE( sets.size() == 2 );
    const auto wireRule = sets.first().toObject().value( "rules" ).toArray().first().toObject();
    CHECK( wireRule.value( "foreground" ) == QLatin1String( "#12345678" ) );
    CHECK( wireRule.value( "ignore_case" ).toBool() );
    CHECK_FALSE( wireRule.value( "use_regexp" ).toBool( true ) );
    CHECK( wireRule.value( "match_only" ).toBool() );
    CHECK( wireRule.value( "variate_colors" ).toBool() );
    CHECK( wireRule.value( "color_variance" ).toInt() == 27 );
    const auto decoded = decodeHighlighters( json.value( "sets" ), json.value( "active_set_ids" ) );
    REQUIRE( std::holds_alternative<HighlighterReplacement>( decoded ) );
    CHECK( std::get<HighlighterReplacement>( decoded ).sets == state.sets );
    CHECK( std::get<HighlighterReplacement>( decoded ).activeSetIds == state.activeSetIds );
    REQUIRE( json.value( "label_palette" ).toArray().size() == 1 );
    CHECK( json.value( "label_defaults" ).toObject().value( "whole_word" ).toBool() );
}

TEST_CASE( "Highlighter wire decoding handles empty replacement and leaves compilation private",
           "[control-highlighter-codec]" )
{
    const auto empty = decodeHighlighters( QJsonArray{}, QJsonArray{} );
    REQUIRE( std::holds_alternative<HighlighterReplacement>( empty ) );
    CHECK( std::get<HighlighterReplacement>( empty ).sets.isEmpty() );
    auto rule = ruleJson();
    rule.insert( "pattern", "(" );
    const auto parsed = decodeHighlighters( QJsonArray{ setJson( rule ) }, QJsonArray{ "shared" } );
    REQUIRE( std::holds_alternative<HighlighterReplacement>( parsed ) );
    const auto rules = std::get<HighlighterReplacement>( parsed ).sets.first().highlighters();
    REQUIRE( rules.size() == 1 );
    CHECK( rules.first().pattern() == QStringLiteral( "(" ) );
    CHECK( rules.first().useRegex() );
    CHECK_FALSE( rules.first().ignoreCase() );
    CHECK_FALSE( rules.first().highlightOnlyMatch() );
}

TEST_CASE( "Highlighter wire fields reject coercion unknown keys and invalid identities",
           "[control-highlighter-codec]" )
{
    QJsonValue sets = QJsonArray{ setJson() };
    QJsonValue active = QJsonArray{ "shared" };
    SECTION( "non-array sets" ) { sets = QJsonObject{}; }
    SECTION( "non-array activation" ) { active = "shared"; }
    SECTION( "unknown active ID" ) { active = QJsonArray{ "missing" }; }
    SECTION( "duplicate set ID" ) { sets = QJsonArray{ setJson(), setJson() }; }
    SECTION( "duplicate activation" ) { active = QJsonArray{ "shared", "shared" }; }
    SECTION( "missing set ID" ) { auto set = setJson(); set.remove( "id" ); sets = QJsonArray{ set }; }
    SECTION( "unknown set field" ) { auto set = setJson(); set.insert( "ai_owned", true ); sets = QJsonArray{ set }; }
    SECTION( "unknown rule field" ) { auto rule = ruleJson(); rule.insert( "execute", "never" ); sets = QJsonArray{ setJson( rule ) }; }
    SECTION( "non-string pattern" ) { auto rule = ruleJson(); rule.insert( "pattern", 42 ); sets = QJsonArray{ setJson( rule ) }; }
    SECTION( "invalid color" ) { auto rule = ruleJson(); rule.insert( "foreground", "not-a-color" ); sets = QJsonArray{ setJson( rule ) }; }
    SECTION( "non-boolean flag" ) { auto rule = ruleJson(); rule.insert( "ignore_case", 1 ); sets = QJsonArray{ setJson( rule ) }; }
    SECTION( "fractional variation" ) { auto rule = ruleJson(); rule.insert( "color_variance", 5.5 ); sets = QJsonArray{ setJson( rule ) }; }
    SECTION( "variation below GUI range" ) { auto rule = ruleJson(); rule.insert( "color_variance", 0 ); sets = QJsonArray{ setJson( rule ) }; }
    CHECK( error( decodeHighlighters( sets, active ) ) == HighlighterDecodeError::InvalidArgument );
}

TEST_CASE( "Highlighter wire decoding admits bounded collection work", "[control-highlighter-codec]" )
{
    QJsonArray sets;
    for ( size_t index = 0; index <= HighlighterSetCollection::MaxPreparedSets; ++index ) sets.append( setJson() );
    CHECK( error( decodeHighlighters( sets, QJsonArray{} ) ) == HighlighterDecodeError::ResourceLimit );
    QJsonArray rules;
    for ( size_t index = 0; index <= HighlighterSetCollection::MaxPreparedRules; ++index ) rules.append( ruleJson() );
    auto set = setJson();
    set.insert( "rules", rules );
    CHECK( error( decodeHighlighters( QJsonArray{ set }, QJsonArray{} ) ) == HighlighterDecodeError::ResourceLimit );
}
