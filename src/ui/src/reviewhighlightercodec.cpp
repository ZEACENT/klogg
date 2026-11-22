#include "reviewhighlightercodec.h"

#include <algorithm>
#include <utility>
#include <QJsonArray>

namespace klogg::review {
namespace {
bool onlyKeys( const QJsonObject& object, std::initializer_list<const char*> allowed )
{
    for ( auto it = object.begin(); it != object.end(); ++it ) {
        if ( !std::any_of( allowed.begin(), allowed.end(), [&]( const char* key ) {
                 return it.key() == QLatin1String( key );
             } ) ) return false;
    }
    return true;
}
QJsonValue colorJson( const QColor& color )
{
    return color.isValid() ? QJsonValue( color.name( QColor::HexArgb ) ) : QJsonValue( QJsonValue::Null );
}
}

QJsonObject encodeHighlighters( const HighlighterSetCollection::Snapshot& state )
{
    QJsonArray sets;
    for ( const auto& set : state.sets ) {
        QJsonArray rules;
        for ( const auto& rule : set.highlighters() ) {
            rules.append( QJsonObject{
                { "pattern", rule.pattern() }, { "ignore_case", rule.ignoreCase() },
                { "use_regexp", rule.useRegex() }, { "match_only", rule.highlightOnlyMatch() },
                { "foreground", colorJson( rule.foreColor() ) }, { "background", colorJson( rule.backColor() ) },
                { "variate_colors", rule.variateColors() }, { "color_variance", rule.colorVariance() } } );
        }
        sets.append( QJsonObject{ { "id", set.id() }, { "name", set.name() }, { "rules", rules } } );
    }
    QJsonArray active;
    for ( const auto& setId : state.activeSetIds ) active.append( setId );
    QJsonArray palette;
    for ( const auto& label : state.quickHighlighters ) {
        palette.append( QJsonObject{ { "name", label.name }, { "foreground", colorJson( label.color.foreColor ) },
            { "background", colorJson( label.color.backColor ) }, { "cycle", label.useInCycle } } );
    }
    return { { "revision", QString::number( state.revision ) }, { "sets", sets },
             { "active_set_ids", active }, { "label_palette", palette },
             { "label_defaults", QJsonObject{ { "ignore_case", state.quickHighlighterDefaults.ignoreCase },
                                               { "whole_word", state.quickHighlighterDefaults.wholeWord } } } };
}

DecodedHighlighterReplacement decodeHighlighters( const QJsonValue& setsValue, const QJsonValue& activeValue )
{
    using Error = HighlighterDecodeError;
    if ( !setsValue.isArray() || !activeValue.isArray() ) return Error::InvalidArgument;
    const auto sets = setsValue.toArray();
    const auto active = activeValue.toArray();
    if ( static_cast<size_t>( sets.size() ) > HighlighterSetCollection::MaxPreparedSets
         || static_cast<size_t>( active.size() ) > HighlighterSetCollection::MaxPreparedSets )
        return Error::ResourceLimit;
    HighlighterReplacement replacement;
    replacement.sets.reserve( sets.size() );
    size_t ruleCount = 0;
    size_t textUnits = 0;
    const auto admitText = [&]( const QString& text ) {
        const auto count = static_cast<size_t>( text.size() );
        if ( count > HighlighterSetCollection::MaxPreparedTextUnits - textUnits ) return false;
        textUnits += count;
        return true;
    };
    for ( const auto& setValue : sets ) {
        if ( !setValue.isObject() ) return Error::InvalidArgument;
        const auto set = setValue.toObject();
        if ( !onlyKeys( set, { "id", "name", "rules" } ) || !set.value( "id" ).isString()
             || set.value( "id" ).toString().isEmpty() || !set.value( "name" ).isString()
             || !set.value( "rules" ).isArray() ) return Error::InvalidArgument;
        const auto setId = set.value( "id" ).toString();
        const auto name = set.value( "name" ).toString();
        if ( !admitText( setId ) || !admitText( name ) ) return Error::ResourceLimit;
        const auto values = set.value( "rules" ).toArray();
        if ( static_cast<size_t>( values.size() ) > HighlighterSetCollection::MaxPreparedRules - ruleCount )
            return Error::ResourceLimit;
        ruleCount += static_cast<size_t>( values.size() );
        QList<Highlighter> rules;
        rules.reserve( values.size() );
        for ( const auto& value : values ) {
            if ( !value.isObject() ) return Error::InvalidArgument;
            const auto rule = value.toObject();
            if ( !onlyKeys( rule, { "pattern", "foreground", "background", "ignore_case", "use_regexp",
                                   "match_only", "variate_colors", "color_variance" } )
                 || !rule.value( "pattern" ).isString() || !rule.value( "foreground" ).isString()
                 || !rule.value( "background" ).isString() ) return Error::InvalidArgument;
            const auto pattern = rule.value( "pattern" ).toString();
            if ( !admitText( pattern ) ) return Error::ResourceLimit;
            for ( const auto* key : { "ignore_case", "use_regexp", "match_only", "variate_colors" } ) {
                if ( rule.contains( key ) && !rule.value( key ).isBool() ) return Error::InvalidArgument;
            }
            int variance = 15;
            if ( rule.contains( "color_variance" ) ) {
                const auto varianceValue = rule.value( "color_variance" );
                variance = varianceValue.toInt( -1 );
                // QSpinBox's configured minimum is 5 and its default maximum is 99.
                if ( !varianceValue.isDouble() || variance < 5 || variance > 99
                     || varianceValue.toDouble() != static_cast<double>( variance ) ) return Error::InvalidArgument;
            }
            const QColor foreground( rule.value( "foreground" ).toString() );
            const QColor background( rule.value( "background" ).toString() );
            if ( !foreground.isValid() || !background.isValid() ) return Error::InvalidArgument;
            Highlighter decoded( pattern, rule.value( "ignore_case" ).toBool( false ),
                                  rule.value( "match_only" ).toBool( false ), foreground, background );
            decoded.setUseRegex( rule.value( "use_regexp" ).toBool( true ) );
            decoded.setVariateColors( rule.value( "variate_colors" ).toBool( false ) );
            decoded.setColorVariance( variance );
            rules.append( std::move( decoded ) );
        }
        replacement.sets.append( HighlighterSet::fromDescription( setId, name, std::move( rules ) ) );
    }
    for ( const auto& value : active ) {
        if ( !value.isString() ) return Error::InvalidArgument;
        replacement.activeSetIds.append( value.toString() );
    }
    HighlighterSetCollection::Snapshot candidate;
    candidate.sets = replacement.sets;
    candidate.activeSetIds = replacement.activeSetIds;
    if ( const auto error = HighlighterSetCollection::validateUpdate( candidate ) ) {
        return *error == HighlighterSetCollection::UpdateResult::ResourceLimit ? Error::ResourceLimit : Error::InvalidArgument;
    }
    return replacement;
}

} // namespace klogg::review
