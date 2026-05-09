#include "ansicolorprocessor.h"

#include <QChar>

namespace {
constexpr ushort Escape = 0x1B;
constexpr ushort Csi = '[';

struct AttrState {
    std::optional<quint32> foreground;
    std::optional<quint32> background;

    bool hasColor() const
    {
        return foreground.has_value() || background.has_value();
    }
};

bool isCsiFinalByte( const QChar ch )
{
    const auto value = ch.unicode();
    return value >= 0x40 && value <= 0x7e;
}

std::optional<int> parseInt( QStringView value )
{
    if ( value.isEmpty() ) {
        return 0;
    }

    int number = 0;
    for ( const auto ch : value ) {
        if ( !ch.isDigit() ) {
            return {};
        }
        number = number * 10 + ch.digitValue();
    }
    return number;
}

klogg::vector<int> parseParameters( QStringView parameters )
{
    klogg::vector<int> parsed;
    if ( parameters.isEmpty() ) {
        parsed.push_back( 0 );
        return parsed;
    }

    qsizetype start = 0;
    for ( qsizetype i = 0; i <= parameters.size(); ++i ) {
        if ( i == parameters.size() || parameters[ i ] == QLatin1Char( ';' )
             || parameters[ i ] == QLatin1Char( ':' ) ) {
            const auto value = parseInt( parameters.mid( start, i - start ) );
            parsed.push_back( value.value_or( -1 ) );
            start = i + 1;
        }
    }

    return parsed;
}

quint32 rgb( int r, int g, int b )
{
    return ( static_cast<quint32>( r ) << 16 ) | ( static_cast<quint32>( g ) << 8 )
           | static_cast<quint32>( b );
}

std::optional<quint32> standardColor( int code )
{
    static constexpr quint32 normal[] = {
        0x010101, 0xde382b, 0x39b54a, 0xffc706, 0x006fb8, 0x762671, 0x2cb5e9,
        0xcccccc,
    };
    static constexpr quint32 bright[] = {
        0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff,
    };

    if ( code >= 30 && code <= 37 ) {
        return normal[ code - 30 ];
    }
    if ( code >= 40 && code <= 47 ) {
        return normal[ code - 40 ];
    }
    if ( code >= 90 && code <= 97 ) {
        return bright[ code - 90 ];
    }
    if ( code >= 100 && code <= 107 ) {
        return bright[ code - 100 ];
    }
    return {};
}

std::optional<quint32> extendedColor( const klogg::vector<int>& params, size_t& index )
{
    if ( index + 1 >= params.size() ) {
        return {};
    }

    const auto colorMode = params[ index + 1 ];
    if ( colorMode == 5 ) {
        if ( index + 2 >= params.size() ) {
            return {};
        }
        const auto value = params[ index + 2 ];
        index += 2;
        if ( value < 0 || value > 255 ) {
            return {};
        }

        if ( value <= 7 ) {
            return standardColor( 30 + value );
        }
        if ( value <= 15 ) {
            return standardColor( 90 + value - 8 );
        }
        if ( value >= 232 ) {
            const auto gray = ( value - 232 ) * 10 + 8;
            return rgb( gray, gray, gray );
        }

        static constexpr int values[] = { 0, 95, 135, 175, 215, 255 };
        const auto colorIndex = value - 16;
        const auto remainder = colorIndex % 36;
        return rgb( values[ colorIndex / 36 ], values[ remainder / 6 ],
                    values[ remainder % 6 ] );
    }

    if ( colorMode == 2 ) {
        if ( index + 4 >= params.size() ) {
            return {};
        }
        const auto r = params[ index + 2 ];
        const auto g = params[ index + 3 ];
        const auto b = params[ index + 4 ];
        index += 4;
        if ( r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255 ) {
            return {};
        }
        return rgb( r, g, b );
    }

    return {};
}

void applySgr( QStringView parameters, AttrState& state )
{
    const auto params = parseParameters( parameters );
    for ( size_t i = 0; i < params.size(); ++i ) {
        const auto code = params[ i ];
        if ( code == 0 ) {
            state = {};
        }
        else if ( code == 39 ) {
            state.foreground.reset();
        }
        else if ( code == 49 ) {
            state.background.reset();
        }
        else if ( ( code >= 30 && code <= 37 ) || ( code >= 90 && code <= 97 ) ) {
            state.foreground = standardColor( code );
        }
        else if ( ( code >= 40 && code <= 47 ) || ( code >= 100 && code <= 107 ) ) {
            state.background = standardColor( code );
        }
        else if ( code == 38 || code == 48 ) {
            const auto color = extendedColor( params, i );
            if ( color.has_value() ) {
                if ( code == 38 ) {
                    state.foreground = color;
                }
                else {
                    state.background = color;
                }
            }
        }
    }
}

void appendColorSpan( klogg::vector<AnsiColorSpan>& spans, const AttrState& state, int start,
                      int end )
{
    if ( !state.hasColor() || end <= start ) {
        return;
    }

    spans.push_back( AnsiColorSpan{ LineColumn{ start }, LineLength{ end - start },
                                    state.foreground, state.background } );
}
} // namespace

ProcessedAnsiLine processAnsiSequences( QString line, AnsiProcessingMode mode )
{
    if ( mode == AnsiProcessingMode::Plain || line.isEmpty() ) {
        return { std::move( line ), {} };
    }

    ProcessedAnsiLine processed;
    processed.text.reserve( line.size() );

    AttrState state;
    qsizetype colorSpanStart = 0;

    for ( int i = 0; i < line.size(); ) {
        const auto ch = line[ i ];
        if ( ch.unicode() != Escape || i + 1 >= line.size() || line[ i + 1 ].unicode() != Csi ) {
            processed.text.append( ch );
            ++i;
            continue;
        }

        int end = i + 2;
        while ( end < line.size() && !isCsiFinalByte( line[ end ] ) ) {
            ++end;
        }

        if ( end >= line.size() ) {
            processed.text.append( ch );
            ++i;
            continue;
        }

        if ( mode == AnsiProcessingMode::Render ) {
            appendColorSpan( processed.colorSpans, state, static_cast<int>( colorSpanStart ),
                             static_cast<int>( processed.text.size() ) );

            if ( line[ end ] == QLatin1Char( 'm' ) ) {
                applySgr( QStringView{ line }.mid( i + 2, end - i - 2 ), state );
            }
            colorSpanStart = processed.text.size();
        }

        i = end + 1;
    }

    if ( mode == AnsiProcessingMode::Render ) {
        appendColorSpan( processed.colorSpans, state, static_cast<int>( colorSpanStart ),
                         static_cast<int>( processed.text.size() ) );
    }

    return processed;
}
