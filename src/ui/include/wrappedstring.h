/*
 * Copyright (C) 2023 -- 2024 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QString>
#include <cstddef>
#include <qchar.h>
#include <qglobal.h>

#include <QStringView>
#include <functional>

#include "containers.h"
#include "linetypes.h"

class WrappedString {
public:
    using WrappedStringPart = QStringView;
    using TextWidthFn = std::function<int( QStringView )>;

    static WrappedStringPart makeWrappedStringPart(const QString& lineText,
        LineColumn firstCol, LineLength length ) {
        return QStringView( lineText ).mid( firstCol.get(), length.get() );
    }

    // Character-count-based wrapping (legacy, used in non-wrap mode)
    explicit WrappedString( QString longLine, LineLength visibleColumns )
    {
        unwrappedLine_ = longLine;
        if ( longLine.isEmpty() ) {
            wrappedLines_.push_back( WrappedStringPart{} );
        }
        else {
            WrappedStringPart lineToWrap( longLine );
            while ( lineToWrap.size() > visibleColumns.get() ) {
                WrappedStringPart stringToWrap = lineToWrap.left( visibleColumns.get() );
                auto lastSpaceIt = std::find_if( stringToWrap.rbegin(), stringToWrap.rend(),
                                                 []( QChar c ) { return c.isSpace(); } );
                if ( lastSpaceIt == stringToWrap.rend() ) {
                    wrappedLines_.push_back( lineToWrap.left( visibleColumns.get() ) );
                    lineToWrap = lineToWrap.mid( visibleColumns.get() );
                }
                else {
                    auto spacePos = std::distance( stringToWrap.begin(), lastSpaceIt.base() );
                    wrappedLines_.push_back( lineToWrap.left( spacePos ) );
                    lineToWrap = lineToWrap.mid( spacePos );
                }
            }
            if ( lineToWrap.size() > 0 ) {
                wrappedLines_.push_back( lineToWrap );
            }
        }
    }

    // Pixel-based wrapping: uses actual text width measurement for accurate wrapping
    explicit WrappedString( QString longLine, int availablePixelWidth, TextWidthFn textWidthFn )
    {
        unwrappedLine_ = longLine;
        if ( longLine.isEmpty() ) {
            wrappedLines_.push_back( WrappedStringPart{} );
        }
        else {
            WrappedStringPart lineToWrap( longLine );
            while ( !lineToWrap.isEmpty() ) {
                int totalWidth = textWidthFn( lineToWrap );
                if ( totalWidth <= availablePixelWidth ) {
                    wrappedLines_.push_back( lineToWrap );
                    break;
                }

                // Binary search for the maximum number of characters that fit
                auto maxChars = lineToWrap.size();
                auto low = static_cast<qsizetype>( 1 );
                auto high = maxChars;
                qsizetype fitCount = 1;

                while ( low <= high ) {
                    auto mid = low + ( high - low ) / 2;
                    auto candidate = lineToWrap.left( mid );
                    int candidateWidth = textWidthFn( candidate );
                    if ( candidateWidth <= availablePixelWidth ) {
                        fitCount = mid;
                        low = mid + 1;
                    }
                    else {
                        high = mid - 1;
                    }
                }

                // Try word-boundary wrap within the fit window
                WrappedStringPart fittingPart = lineToWrap.left( fitCount );
                auto lastSpaceIt = std::find_if( fittingPart.rbegin(), fittingPart.rend(),
                                                 []( QChar c ) { return c.isSpace(); } );
                if ( lastSpaceIt != fittingPart.rend() ) {
                    auto spacePos = std::distance( fittingPart.begin(), lastSpaceIt.base() );
                    wrappedLines_.push_back( lineToWrap.left( spacePos ) );
                    lineToWrap = lineToWrap.mid( spacePos );
                }
                else {
                    // No space found; hard-wrap at pixel boundary
                    if ( fitCount > 0 ) {
                        wrappedLines_.push_back( lineToWrap.left( fitCount ) );
                        lineToWrap = lineToWrap.mid( fitCount );
                    }
                    else {
                        // At least one character per line to avoid infinite loop
                        wrappedLines_.push_back( lineToWrap.left( 1 ) );
                        lineToWrap = lineToWrap.mid( 1 );
                    }
                }
            }
        }
    }

    size_t wrappedLinesCount() const
    {
        return wrappedLines_.size();
    }

    klogg::vector<WrappedStringPart> mid( LineColumn start, LineLength length ) const
    {
        auto getLength = []( const auto& view ) -> LineLength::UnderlyingType {
            return type_safe::narrow_cast<LineLength::UnderlyingType>( view.size() );
        };

        klogg::vector<WrappedStringPart> resultChunks;
        if ( wrappedLines_.size() == 1 ) {
            auto& wrappedLine = wrappedLines_.front();
            auto len = std::min( length.get(), getLength( wrappedLine ) - start.get() );
            resultChunks.push_back( wrappedLine.mid( start.get(), ( len > 0 ? len : 0 ) ) );
            return resultChunks;
        }

        size_t wrappedLineIndex = 0;
        auto positionInWrappedLine = start.get();
        while ( positionInWrappedLine > getLength( wrappedLines_[ wrappedLineIndex ] ) ) {
            positionInWrappedLine -= getLength( wrappedLines_[ wrappedLineIndex ] );
            wrappedLineIndex++;
            if ( wrappedLineIndex >= wrappedLines_.size() ) {
                return resultChunks;
            }
        }

        auto chunkLength = length.get();
        while ( positionInWrappedLine + chunkLength
                > getLength( wrappedLines_[ wrappedLineIndex ] ) ) {
            resultChunks.push_back(
                wrappedLines_[ wrappedLineIndex ].mid( positionInWrappedLine ) );
            wrappedLineIndex++;
            positionInWrappedLine = 0;
            chunkLength -= getLength( resultChunks.back() );
            if ( wrappedLineIndex >= wrappedLines_.size() ) {
                return resultChunks;
            }
        }

        if ( chunkLength > 0 ) {
            auto& wrappedLine = wrappedLines_[ wrappedLineIndex ];
            auto len = std::min( chunkLength, getLength( wrappedLine ) - positionInWrappedLine );
            resultChunks.push_back(
                wrappedLine.mid( positionInWrappedLine, ( len > 0 ? len : 0 ) ) );
        }

        return resultChunks;
    }

    bool isEmpty() const
    {
        return unwrappedLine_.isEmpty();
    }

    WrappedStringPart unwrappedLine() const {
        return WrappedStringPart{unwrappedLine_};
    }

    WrappedStringPart wrappedLine(size_t index) const {
        return wrappedLines_[index];
    }

private:
    klogg::vector<WrappedStringPart> wrappedLines_;
    QString unwrappedLine_;
};