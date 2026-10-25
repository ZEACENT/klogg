#include "abstractlogview.h"

#include <QElapsedTimer>
#include <QProgressDialog>
#include <QTimer>
#include <algorithm>
#include <exception>

#include "log.h"
#include "matchprojection.h"

namespace {
using TabDeltas = std::vector<std::pair<klogg::ContainerIndex, klogg::ContainerIndex>>;

TabDeltas tabDeltas( const QString& text )
{
    TabDeltas result;
    klogg::ContainerIndex delta = 0;
    auto position = text.indexOf( QChar::Tabulation );
    while ( position >= 0 ) {
        delta += TabStop - ( ( position + delta ) % TabStop ) - 1;
        result.emplace_back( position, delta );
        position = text.indexOf( QChar::Tabulation, position + 1 );
    }
    return result;
}

LineColumn expandedColumn( LineColumn column, const TabDeltas& deltas )
{
    const auto next
        = std::lower_bound( deltas.begin(), deltas.end(), column.get(),
                            []( const auto& tab, auto offset ) { return tab.first < offset; } );
    return LineColumn{ column.get() + ( next == deltas.begin() ? 0 : std::prev( next )->second ) };
}

// Merge captures only inside this occurrence. Separate lookahead occurrences
// may overlap and must still produce separate clipboard entries.
std::vector<std::pair<LineColumn, LineColumn>>
occurrenceFragments( const QRegularExpressionMatch& match, klogg::MatchProjection projection )
{
    std::vector<std::pair<LineColumn, LineColumn>> fragments;
    klogg::visitMatchFragments( match, projection, [ & ]( LineColumn start, LineLength length ) {
        fragments.emplace_back( start, start + length - 1_length );
    } );
    std::sort( fragments.begin(), fragments.end() );
    size_t count = 0;
    for ( const auto& fragment : fragments ) {
        if ( count > 0 && fragment.first <= fragments[ count - 1 ].second ) {
            fragments[ count - 1 ].second
                = std::max( fragments[ count - 1 ].second, fragment.second );
        }
        else {
            fragments[ count++ ] = fragment;
        }
    }
    fragments.resize( count );
    return fragments;
}
} // namespace

QRegularExpression AbstractLogView::matchSelectionExpression( bool quickFind ) const
{
    if ( quickFind ) {
        return quickFindPattern_ != nullptr && quickFindPattern_->isActive()
                   ? quickFindPattern_->regularExpression()
                   : QRegularExpression{};
    }
    if ( searchPattern_.isBoolean || searchPattern_.isExclude
         || searchPattern_.pattern.isEmpty() ) {
        return {};
    }
    auto expression = static_cast<QRegularExpression>( searchPattern_ );
    expression.setPatternOptions( expression.patternOptions()
                                  & ~QRegularExpression::DontCaptureOption );
    return expression;
}

bool AbstractLogView::canSelectAllMatches( bool quickFind ) const
{
    const auto expression = matchSelectionExpression( quickFind );
    return !expression.pattern().isEmpty() && expression.isValid();
}

bool AbstractLogView::selectAllMatches( bool quickFind, bool copy )
{
    if ( !canSelectAllMatches( quickFind ) || logData_->getNbLine() == 0_lcount ) {
        return false;
    }
    cancelMatchSelection();
    quickFind_->stopSearch();
    acceptQuickFindResults_ = false;
    disableFollow();

    auto operation = std::make_unique<MatchSelection>();
    operation->expression = matchSelectionExpression( quickFind );
    operation->expression.optimize();
    operation->quickFind = quickFind;
    operation->copyWhenFinished = copy;
    operation->totalRows = logData_->getNbLine();
    // Qt parent ownership covers view teardown; normal completion uses deleteLater.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    operation->progress = new QProgressDialog( tr( "Selecting match fragments..." ), tr( "Cancel" ),
                                               0, 1000, this );
    operation->progress->setWindowModality( Qt::NonModal );
    operation->progress->setMinimumDuration( 750 );
    operation->progress->setAutoClose( false );
    operation->progress->setAutoReset( false );
    connect( operation->progress.data(), &QProgressDialog::canceled, this,
             &AbstractLogView::cancelMatchSelection );
    matchSelection_ = std::move( operation );
    const auto generation = ++matchSelectionGeneration_;
    QTimer::singleShot( 0, this, [ this, generation ] { continueMatchSelection( generation ); } );
    return true;
}

void AbstractLogView::continueMatchSelection( quint64 generation )
{
    if ( !matchSelection_ || generation != matchSelectionGeneration_ ) {
        return;
    }
    try {
        auto& operation = *matchSelection_;
        if ( logData_->getNbLine() < operation.totalRows ) {
            invalidateMatchSelection();
            return;
        }
        QElapsedTimer elapsed;
        elapsed.start();
        unsigned rows = 0;
        unsigned occurrences = 0;
        // Bound both rows and occurrences: one long line must not monopolize
        // the event loop. The existing live-presentation cadence is unchanged.
        while ( operation.nextRow < operation.totalRows && rows < 128 && occurrences < 1024
                && elapsed.elapsed() < 8 ) {
            if ( !operation.readingLine ) {
                if ( !logData_->isLineCopyable( operation.nextRow )
                     || ( !operation.quickFind
                          && !lineType( operation.nextRow )
                                  .testFlag( AbstractLogData::LineTypeFlags::Match ) ) ) {
                    ++operation.nextRow;
                    ++rows;
                    continue;
                }
                operation.text = operation.quickFind
                                     ? logData_->getExpandedLineString( operation.nextRow )
                                     : logData_->getLineString( operation.nextRow );
                operation.tabDeltas
                    = operation.quickFind ? TabDeltas{} : tabDeltas( operation.text );
                operation.iterator = operation.expression.globalMatch( operation.text );
                operation.readingLine = true;
            }
            if ( !operation.iterator.hasNext() ) {
                operation.readingLine = false;
                ++operation.nextRow;
                ++rows;
                continue;
            }
            const auto projection = operation.quickFind
                                        ? klogg::MatchProjection::FullMatch
                                        : klogg::MatchProjection::CapturesOrFullMatch;
            for ( const auto& fragment :
                  occurrenceFragments( operation.iterator.next(), projection ) ) {
                if ( operation.quickFind ) {
                    operation.portions.emplace_back( operation.nextRow, fragment.first,
                                                     fragment.second );
                }
                else {
                    operation.portions.emplace_back(
                        operation.nextRow, expandedColumn( fragment.first, operation.tabDeltas ),
                        expandedColumn( fragment.second + 1_length, operation.tabDeltas )
                            - 1_length,
                        fragment.first, fragment.second );
                }
            }
            ++occurrences;
        }
        if ( operation.nextRow >= operation.totalRows ) {
            finishMatchSelection( true );
            return;
        }
        const auto processed = operation.nextRow.get();
        const auto total = operation.totalRows.get();
        operation.progress->setValue( static_cast<int>( 1000.0 * static_cast<double>( processed )
                                                        / static_cast<double>( total ) ) );
        Q_EMIT matchSelectionProgress( processed, total );
        // A progress listener may cancel or replace the operation synchronously.
        if ( matchSelection_ && generation == matchSelectionGeneration_ ) {
            QTimer::singleShot( 0, this,
                                [ this, generation ] { continueMatchSelection( generation ); } );
        }
    } catch ( const std::exception& error ) {
        LOG_ERROR << "Match selection failed: " << error.what();
        cancelMatchSelection();
    }
}

void AbstractLogView::finishMatchSelection( bool completed )
{
    if ( !matchSelection_ ) {
        return;
    }
    if ( completed ) {
        Selection selected;
        selected.selectPortions( std::move( matchSelection_->portions ),
                                 Selection::PortionOverlap::Preserve );
        selection_ = std::move( selected );
        matchSelectionRows_ = matchSelection_->totalRows;
        if ( !selection_.isEmpty() ) {
            selectionStartPos_ = selection_.getPreviousPosition();
            selectionCurrentEndPos_ = selection_.getNextPosition();
        }
    }
    auto operation = std::move( matchSelection_ );
    ++matchSelectionGeneration_;
    disconnect( operation->progress.data(), nullptr, this, nullptr );
    operation->progress->hide();
    operation->progress->deleteLater();
    if ( completed ) {
        setFocus( Qt::ShortcutFocusReason );
        forceRefresh();
        if ( operation->copyWhenFinished ) {
            copy();
        }
    }
    Q_EMIT matchSelectionFinished( completed );
}

bool AbstractLogView::deferCopyUntilMatchSelection()
{
    if ( !matchSelection_ ) {
        return false;
    }
    matchSelection_->copyWhenFinished = true;
    return true;
}

void AbstractLogView::cancelMatchSelection()
{
    finishMatchSelection( false );
}

void AbstractLogView::invalidateMatchSelection()
{
    cancelMatchSelection();
    if ( selection_.isMultiPortion() ) {
        selection_.clear();
        forceRefresh();
    }
}
