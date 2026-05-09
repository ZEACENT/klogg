#ifndef COMMANDARGUMENTTOKENIZER_H
#define COMMANDARGUMENTTOKENIZER_H

#include <QString>
#include <QStringList>

namespace ui::internal {

inline bool canEscapeArgumentCharacter( const QChar nextChar, const QChar quoteChar )
{
    if ( quoteChar == QLatin1Char( '"' ) ) {
        return nextChar == QLatin1Char( '"' ) || nextChar == QLatin1Char( '\\' );
    }

    if ( quoteChar == QLatin1Char( '\'' ) ) {
        return false;
    }

    return nextChar.isSpace() || nextChar == QLatin1Char( '"' )
           || nextChar == QLatin1Char( '\'' ) || nextChar == QLatin1Char( '\\' );
}

inline QStringList splitCommandArguments( const QString& arguments )
{
    QStringList tokens;
    QString currentToken;
    QChar quoteChar;
    bool tokenStarted = false;

    for ( int i = 0; i < arguments.size(); ++i ) {
        const auto ch = arguments.at( i );
        if ( ch == QLatin1Char( '\\' ) ) {
            const auto nextIndex = i + 1;
            if ( nextIndex < arguments.size() ) {
                const auto nextChar = arguments.at( nextIndex );
                if ( canEscapeArgumentCharacter( nextChar, quoteChar ) ) {
                    tokenStarted = true;
                    currentToken.append( nextChar );
                    ++i;
                    continue;
                }
            }

            tokenStarted = true;
            currentToken.append( ch );
            continue;
        }

        if ( !quoteChar.isNull() ) {
            if ( ch == quoteChar ) {
                quoteChar = QChar{};
            }
            else {
                tokenStarted = true;
                currentToken.append( ch );
            }
            continue;
        }

        if ( ch == QLatin1Char( '"' ) || ch == QLatin1Char( '\'' ) ) {
            tokenStarted = true;
            quoteChar = ch;
            continue;
        }

        if ( ch.isSpace() ) {
            if ( tokenStarted ) {
                tokens.push_back( currentToken );
                currentToken.clear();
                tokenStarted = false;
            }
            continue;
        }

        tokenStarted = true;
        currentToken.append( ch );
    }

    if ( tokenStarted ) {
        tokens.push_back( currentToken );
    }

    return tokens;
}

} // namespace ui::internal

#endif // COMMANDARGUMENTTOKENIZER_H
