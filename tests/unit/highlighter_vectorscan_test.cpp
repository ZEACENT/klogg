/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
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

#include <catch2/catch.hpp>
#include <algorithm>
#include <type_traits>
#include <utility>

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QSettings>
#include <QTemporaryDir>

#include "configuration.h"
#include "highlighterset.h"
#include "highlightersdialog.h"
#include "hsregularexpression.h"
#include "test_utils.h"

namespace {

klogg::vector<RegularExpressionPattern> makeRegressionPatterns()
{
    klogg::vector<RegularExpressionPattern> patterns;

    RegularExpressionPattern errorPattern( QStringLiteral( R"(\bERROR\b)" ), true, false, false,
                                           false );
    errorPattern.isPrefilter = true;
    patterns.emplace_back( errorPattern );

    RegularExpressionPattern urlPattern( QStringLiteral( R"(https?://[A-Za-z0-9._~:/?#\[\]@!$&'()*+,;=%-]+)" ),
                                         true, false, false, false );
    urlPattern.isPrefilter = true;
    patterns.emplace_back( urlPattern );

    return patterns;
}

void writeHighlighterSet( QSettings& settings,
                          const klogg::vector<RegularExpressionPattern>& patterns )
{
    settings.beginGroup( QStringLiteral( "HighlighterSet" ) );
    settings.setValue( QStringLiteral( "version" ), 3 );
    settings.setValue( QStringLiteral( "name" ), QStringLiteral( "Vectorscan Regression" ) );
    settings.setValue( QStringLiteral( "id" ), QStringLiteral( "vectorscan-regression" ) );
    settings.beginWriteArray( QStringLiteral( "highlighters" ) );

    int arrayIndex = 0;
    for ( const auto& pattern : patterns ) {
        settings.setArrayIndex( arrayIndex++ );
        settings.setValue( QStringLiteral( "regexp" ), pattern.pattern );
        settings.setValue( QStringLiteral( "ignore_case" ), !pattern.isCaseSensitive );
        settings.setValue( QStringLiteral( "match_only" ), true );
        settings.setValue( QStringLiteral( "use_regex" ), true );
        settings.setValue( QStringLiteral( "variate_colors" ), false );
        settings.setValue( QStringLiteral( "color_variance" ), 15 );
        settings.setValue( QStringLiteral( "fore_colour" ),
                           QColor( Qt::black ).name( QColor::HexArgb ) );
        settings.setValue( QStringLiteral( "back_colour" ),
                           QColor( Qt::yellow ).name( QColor::HexArgb ) );
    }

    settings.endArray();
    settings.endGroup();
}

HighlighterSet loadHighlighterSet( const QString& settingsPath,
                                   const klogg::vector<RegularExpressionPattern>& patterns )
{
    QSettings settings( settingsPath, QSettings::IniFormat );
    settings.clear();
    writeHighlighterSet( settings, patterns );
    settings.sync();

    HighlighterSet set;
    set.retrieveFromStorage( settings );
    return set;
}

HighlighterSetCollection loadHighlighterSetCollection(
    const QString& settingsPath, const klogg::vector<RegularExpressionPattern>& patterns )
{
    auto set = loadHighlighterSet( settingsPath + QStringLiteral( ".set.ini" ), patterns );

    QSettings settings( settingsPath, QSettings::IniFormat );
    settings.clear();
    settings.beginGroup( QStringLiteral( "HighlighterSetCollection" ) );
    settings.setValue( QStringLiteral( "version" ), 2 );
    settings.setValue( QStringLiteral( "active_sets" ),
                       QStringList{ QStringLiteral( "vectorscan-regression" ) } );
    settings.beginWriteArray( QStringLiteral( "sets" ) );
    settings.setArrayIndex( 0 );
    set.saveToStorage( settings );
    settings.endArray();
    settings.endGroup();
    settings.sync();

    HighlighterSetCollection collection;
    collection.retrieveFromStorage( settings );
    return collection;
}

struct RestoreHighlighterCollection {
    HighlighterSetCollection saved;
    ~RestoreHighlighterCollection()
    {
        HighlighterSetCollection::get() = saved;
        HighlighterSetCollection::get().save();
    }
};

void requireSameState( const HighlighterSetCollection::Snapshot& actual,
                       const HighlighterSetCollection::Snapshot& expected )
{
    REQUIRE( actual.sets == expected.sets );
    REQUIRE( actual.activeSetIds == expected.activeSetIds );
    REQUIRE( actual.quickHighlighters == expected.quickHighlighters );
    REQUIRE( actual.quickHighlighterDefaults == expected.quickHighlighterDefaults );
}

} // namespace

TEST_CASE( "Highlighter state snapshots expose detached complete semantic values",
           "[highlighter][highlighter-state]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    auto collection = loadHighlighterSetCollection( dir.filePath( "collection.ini" ),
                                                    makeRegressionPatterns() );
    collection.setQuickHighlighterDefaults( { false, true } );
    const auto before = collection.snapshot();
    REQUIRE( before.sets.size() == 1 );
    const auto& set = before.sets.front();
    REQUIRE( set.name() == QStringLiteral( "Vectorscan Regression" ) );
    REQUIRE( set.id() == QStringLiteral( "vectorscan-regression" ) );
    REQUIRE( before.activeSetIds == QStringList{ set.id() } );
    auto rules = set.highlighters();
    REQUIRE( rules.size() == 2 );
    REQUIRE( rules.front().pattern() == makeRegressionPatterns().front().pattern );
    REQUIRE_FALSE( rules.front().ignoreCase() );
    REQUIRE( rules.front().useRegex() );
    REQUIRE( rules.front().highlightOnlyMatch() );
    REQUIRE_FALSE( rules.front().variateColors() );
    REQUIRE( rules.front().colorVariance() == 15 );
    REQUIRE( rules.front().foreColor() == QColor( Qt::black ) );
    REQUIRE( rules.front().backColor() == QColor( Qt::yellow ) );
    REQUIRE( before.quickHighlighters.size() == 9 );
    REQUIRE_FALSE( before.quickHighlighterDefaults.ignoreCase );
    REQUIRE( before.quickHighlighterDefaults.wholeWord );

    rules.front().setPattern( QStringLiteral( "detached" ) );
    auto detached = before;
    detached.sets.clear();
    detached.activeSetIds.clear();
    detached.quickHighlighters.front().name = QStringLiteral( "detached" );
    detached.quickHighlighterDefaults = {};
    const HighlighterSetCollection& readOnly = collection;
    requireSameState( readOnly.snapshot(), before );
    REQUIRE( readOnly.snapshot().revision == before.revision );
    REQUIRE( before.revision > 0 );
}

TEST_CASE( "Highlighter collection revisions cover every rule and set semantic field",
           "[highlighter][highlighter-state]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto path = dir.filePath( "set.ini" );
    const auto original = loadHighlighterSet( path, makeRegressionPatterns() );
    HighlighterSetCollection collection;
    collection.setHighlighterSets( { original } );
    const auto before = collection.snapshot();

    const QList<QPair<QString, QVariant>> edits{
        { "name", "renamed" }, { "id", "replacement-id" },
        { "highlighters/1/regexp", "changed-pattern" },
        { "highlighters/1/ignore_case", true },
        { "highlighters/1/match_only", false },
        { "highlighters/1/use_regex", false },
        { "highlighters/1/variate_colors", true },
        { "highlighters/1/color_variance", 22 },
        { "highlighters/1/fore_colour", "#80123456" },
        { "highlighters/1/back_colour", "#40765432" } };
    for ( const auto& edit : edits ) {
        CAPTURE( edit.first );
        QSettings settings( path, QSettings::IniFormat );
        original.saveToStorage( settings );
        settings.setValue( QStringLiteral( "HighlighterSet/" ) + edit.first, edit.second );
        HighlighterSet changed;
        changed.retrieveFromStorage( settings );
        const auto revision = collection.snapshot().revision;
        collection.setHighlighterSets( { changed } );
        REQUIRE( collection.snapshot().sets.front() != original );
        REQUIRE( collection.snapshot().revision == revision + 1 );
        collection.setHighlighterSets( { changed } );
        REQUIRE( collection.snapshot().revision == revision + 1 );
        collection.setHighlighterSets( { original } );
        REQUIRE( collection.snapshot().revision == revision + 2 );
    }
    requireSameState( collection.snapshot(), before );

    const auto second = HighlighterSet::createNewSet( QStringLiteral( "Second" ) );
    collection.setHighlighterSets( { original, second } );
    const auto ordered = collection.snapshot();
    collection.setHighlighterSets( { second, original } );
    REQUIRE( collection.snapshot().revision == ordered.revision + 1 );
    collection.setHighlighterSets( { original, second } );
    REQUIRE( collection.snapshot().revision == ordered.revision + 2 );

    auto reversedPatterns = makeRegressionPatterns();
    std::reverse( reversedPatterns.begin(), reversedPatterns.end() );
    const auto reversed = loadHighlighterSet( path, reversedPatterns );
    collection.setHighlighterSets( { reversed, second } );
    REQUIRE( collection.snapshot().revision == ordered.revision + 3 );
}

TEST_CASE( "Highlighter activation palette and defaults track semantic ABA without reads",
           "[highlighter][highlighter-state]" )
{
    HighlighterSetCollection collection;
    const auto first = HighlighterSet::createNewSet( QStringLiteral( "First" ) );
    const auto second = HighlighterSet::createNewSet( QStringLiteral( "Second" ) );
    collection.setHighlighterSets( { first, second } );
    const auto before = collection.snapshot();
    collection.activateSet( first.id() );
    collection.deactivateSet( first.id() );
    requireSameState( collection.snapshot(), before );
    REQUIRE( collection.snapshot().revision == before.revision + 2 );
    const auto unchanged = collection.snapshot().revision;
    collection.activateSet( QStringLiteral( "missing" ) );
    collection.deactivateSet( QStringLiteral( "missing" ) );
    collection.deactivateAll();
    REQUIRE( collection.snapshot().revision == unchanged );
    collection.activateSet( first.id() );
    collection.activateSet( first.id() );
    REQUIRE( collection.snapshot().revision == unchanged + 1 );
    collection.activateSet( second.id() );
    collection.setHighlighterSets( { second } );
    REQUIRE( collection.snapshot().activeSetIds == QStringList{ second.id() } );
    REQUIRE( collection.snapshot().revision == unchanged + 3 );
    collection.deactivateAll();
    REQUIRE( collection.snapshot().revision == unchanged + 4 );

    const QList<QuickHighlighter> palette{
        { QStringLiteral( "Label" ), { QColor( Qt::red ), QColor( Qt::blue ) }, true } };
    collection.setQuickHighlighters( palette );
    for ( int field = 0; field < 4; ++field ) {
        auto changed = palette;
        if ( field == 0 ) changed.front().name = QStringLiteral( "Renamed" );
        if ( field == 1 ) changed.front().color.foreColor = QColor( Qt::green );
        if ( field == 2 ) changed.front().color.backColor = QColor( Qt::yellow );
        if ( field == 3 ) changed.front().useInCycle = false;
        const auto revision = collection.snapshot().revision;
        collection.setQuickHighlighters( changed );
        collection.setQuickHighlighters( changed );
        collection.setQuickHighlighters( palette );
        REQUIRE( collection.snapshot().revision == revision + 2 );
    }
    const auto defaults = collection.snapshot();
    collection.setQuickHighlighterDefaults( { false, false } );
    collection.setQuickHighlighterDefaults( { false, false } );
    collection.setQuickHighlighterDefaults( { false, true } );
    collection.setQuickHighlighterDefaults( defaults.quickHighlighterDefaults );
    requireSameState( collection.snapshot(), defaults );
    REQUIRE( collection.snapshot().revision == defaults.revision + 3 );
}

TEST_CASE( "Highlighter snapshot save and compiled caches are revision neutral",
           "[highlighter][highlighter-state]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    auto collection = loadHighlighterSetCollection( dir.filePath( "collection.ini" ),
                                                    makeRegressionPatterns() );
    // The legacy default palette contains an invalid foreground color whose
    // existing string persistence normalizes to black. Establish stored form
    // before testing a semantically unchanged reload.
    QSettings saved( dir.filePath( "saved.ini" ), QSettings::IniFormat );
    collection.saveToStorage( saved );
    collection.retrieveFromStorage( saved );
    const auto before = collection.snapshot();
    auto sets = collection.highlighterSets();
    sets.front().compile();
    auto rules = sets.front().highlighters();
    rules.front().compile();
    REQUIRE( rules == before.sets.front().highlighters() );
    HighlightedMatchRanges matches;
    REQUIRE( collection.currentActiveSet().matchLine( QStringLiteral( "ERROR" ), matches )
             == HighlighterMatchType::WordMatch );
    collection.setHighlighterSets( sets );
    collection.saveToStorage( saved );
    const auto keys = saved.allKeys();
    QSignalSpy focus( qApp, &QApplication::focusChanged );
    requireSameState( collection.snapshot(), before );
    REQUIRE( collection.snapshot().revision == before.revision );
    REQUIRE( saved.allKeys() == keys );
    REQUIRE( focus.isEmpty() );
    REQUIRE_FALSE( keys.contains( QStringLiteral( "HighlighterSetCollection/revision" ) ) );

    collection.retrieveFromStorage( saved );
    requireSameState( collection.snapshot(), before );
    REQUIRE( collection.snapshot().revision == before.revision );
}

TEST_CASE( "Highlighter storage reload advances once and never resets the owner revision",
           "[highlighter][highlighter-state]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    const auto path = dir.filePath( "collection.ini" );
    auto collection = loadHighlighterSetCollection( path, makeRegressionPatterns() );
    QSettings settings( path, QSettings::IniFormat );
    collection.saveToStorage( settings );
    collection.retrieveFromStorage( settings );
    const auto before = collection.snapshot();
    settings.setValue( "HighlighterSetCollection/sets/1/HighlighterSet/name", "External" );
    settings.setValue( "HighlighterSetCollection/quick_defaults/whole_word", true );
    // A read must not pull external settings into application state.
    requireSameState( collection.snapshot(), before );
    collection.retrieveFromStorage( settings );
    REQUIRE( collection.snapshot().sets.front().name() == QStringLiteral( "External" ) );
    REQUIRE( collection.snapshot().quickHighlighterDefaults.wholeWord );
    REQUIRE( collection.snapshot().revision == before.revision + 1 );
    collection.retrieveFromStorage( settings );
    REQUIRE( collection.snapshot().revision == before.revision + 1 );

    settings.setValue( "HighlighterSetCollection/sets/1/HighlighterSet/name", before.sets.front().name() );
    settings.setValue( "HighlighterSetCollection/quick_defaults/whole_word", false );
    collection.retrieveFromStorage( settings );
    requireSameState( collection.snapshot(), before );
    REQUIRE( collection.snapshot().revision == before.revision + 2 );
}

TEST_CASE( "Highlighter collection assignment commits content not the draft revision",
           "[highlighter][highlighter-state]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    auto owner = loadHighlighterSetCollection( dir.filePath( "collection.ini" ),
                                               makeRegressionPatterns() );
    const auto original = owner.snapshot();
    auto draft = owner;
    REQUIRE( draft.snapshot().revision == 0 );
    for ( int edit = 0; edit < 3; ++edit ) {
        draft.setQuickHighlighterDefaults( { false, true } );
        draft.setQuickHighlighterDefaults( original.quickHighlighterDefaults );
    }
    owner = draft;
    REQUIRE( owner.snapshot().revision == original.revision );
    draft.setQuickHighlighterDefaults( { false, true } );
    owner = draft;
    REQUIRE( owner.snapshot().revision == original.revision + 1 );
    requireSameState( owner.snapshot(), draft.snapshot() );
    owner = std::move( draft );
    REQUIRE( owner.snapshot().revision == original.revision + 1 );

    HighlighterSetCollection replacement;
    replacement.setHighlighterSets( original.sets );
    replacement.setQuickHighlighters( original.quickHighlighters );
    owner = std::move( replacement );
    REQUIRE( owner.snapshot().revision == original.revision + 2 );
    REQUIRE( owner.currentActiveSet().isEmpty() );
    const auto& sameOwner = owner;
    owner = sameOwner;
    REQUIRE( owner.snapshot().revision == original.revision + 2 );
}

TEST_CASE( "Highlighter dialog Apply OK and Cancel share the committed revision owner",
           "[highlighter][highlighter-state]" )
{
    auto& owner = HighlighterSetCollection::getSynced();
    RestoreHighlighterCollection restore{ owner };
    owner.setHighlighterSets( {} );
    owner.save();
    HighlighterSetCollection::getSynced(); // Normalize the legacy palette's stored color form.
    const auto original = owner.snapshot();
    HighlightersDialog dialog;
    // Constructor posts icon loading to the GUI dispatcher; drain it while
    // the dialog is alive, without sleeping or opening a modal event loop.
    QCoreApplication::sendPostedEvents( QAbstractEventDispatcher::instance(), QEvent::MetaCall );
    REQUIRE( owner.snapshot().revision == original.revision );
    auto* const name = qobject_cast<QLineEdit*>( dialog.quickHighlightLayout->itemAtPosition( 1, 0 )->widget() );
    REQUIRE( name != nullptr );
    auto* const apply = dialog.buttonBox->button( QDialogButtonBox::Apply );
    REQUIRE( apply != nullptr );
    auto published = owner.snapshot();
    QObject::connect( &dialog, &HighlightersDialog::optionsChanged, &dialog,
                      [ & ] { published = owner.snapshot(); } );
    name->selectAll();
    QTest::keyClicks( name, "Human draft" );
    requireSameState( owner.snapshot(), original );
    apply->click();
    REQUIRE( published.quickHighlighters.front().name == QStringLiteral( "Human draft" ) );
    REQUIRE( published.revision == original.revision + 1 );
    apply->click();
    REQUIRE( owner.snapshot().revision == published.revision );
    name->setText( original.quickHighlighters.front().name );
    apply->click();
    requireSameState( owner.snapshot(), original );
    REQUIRE( owner.snapshot().revision == original.revision + 2 );

    name->setText( QStringLiteral( "Cancelled draft" ) );
    dialog.buttonBox->button( QDialogButtonBox::Cancel )->click();
    requireSameState( owner.snapshot(), original );
    REQUIRE( owner.snapshot().revision == original.revision + 2 );

    HighlightersDialog accepted;
    QCoreApplication::sendPostedEvents( QAbstractEventDispatcher::instance(), QEvent::MetaCall );
    auto* const acceptedName = qobject_cast<QLineEdit*>(
        accepted.quickHighlightLayout->itemAtPosition( 1, 0 )->widget() );
    REQUIRE( acceptedName != nullptr );
    acceptedName->setText( QStringLiteral( "Accepted draft" ) );
    accepted.buttonBox->button( QDialogButtonBox::Ok )->click();
    REQUIRE( owner.snapshot().quickHighlighters.front().name == QStringLiteral( "Accepted draft" ) );
    REQUIRE( owner.snapshot().revision == original.revision + 3 );
    REQUIRE( accepted.result() == QDialog::Accepted );
}

TEST_CASE( "Highlighter semantic edits invalidate previously compiled matchers",
           "[highlighter][highlighter-state]" )
{
    Highlighter rule( QStringLiteral( "e.r" ), false, true, Qt::black, Qt::yellow );
    klogg::vector<HighlightedMatch> matches;
    REQUIRE( rule.matchLine( QStringLiteral( "ear" ), matches ) );

    SECTION( "pattern" )
    {
        rule.setPattern( QStringLiteral( "warning" ) );
        REQUIRE( rule.matchLine( QStringLiteral( "warning" ), matches ) );
    }
    SECTION( "case mode" )
    {
        rule.setIgnoreCase( true );
        REQUIRE( rule.matchLine( QStringLiteral( "EAR" ), matches ) );
    }
    SECTION( "literal mode" )
    {
        rule.setUseRegex( false );
        REQUIRE_FALSE( rule.matchLine( QStringLiteral( "ear" ), matches ) );
        REQUIRE( rule.matchLine( QStringLiteral( "e.r" ), matches ) );
    }
    SECTION( "rule storage replacement" )
    {
        QTemporaryDir dir;
        REQUIRE( dir.isValid() );
        QSettings settings( dir.filePath( "rule.ini" ), QSettings::IniFormat );
        rule.saveToStorage( settings );
        settings.setValue( "regexp", "warning" );
        rule.retrieveFromStorage( settings );
        REQUIRE( rule.matchLine( QStringLiteral( "warning" ), matches ) );
    }
    SECTION( "set storage replacement" )
    {
        QTemporaryDir dir;
        REQUIRE( dir.isValid() );
        const auto path = dir.filePath( "set.ini" );
        auto set = loadHighlighterSet( path, makeRegressionPatterns() );
        HighlightedMatchRanges ranges;
        REQUIRE( set.matchLine( QStringLiteral( "ERROR" ), ranges ) == HighlighterMatchType::WordMatch );
        QSettings settings( path, QSettings::IniFormat );
        settings.setValue( "HighlighterSet/highlighters/1/regexp", "warning" );
        set.retrieveFromStorage( settings );
        REQUIRE( set.matchLine( QStringLiteral( "warning" ), ranges ) == HighlighterMatchType::WordMatch );
    }
}

TEST_CASE( "Highlighter dialog publishes edited active rules with their effective matcher",
           "[highlighter][highlighter-state]" )
{
    QTemporaryDir dir;
    REQUIRE( dir.isValid() );
    auto& owner = HighlighterSetCollection::getSynced();
    RestoreHighlighterCollection restore{ owner };
    owner = loadHighlighterSetCollection( dir.filePath( "collection.ini" ), makeRegressionPatterns() );
    owner.save();
    HighlighterSetCollection::getSynced();
    const auto before = owner.snapshot();
    HighlightedMatchRanges matches;
    // Populate both set-level and rule-level caches before editing the copy.
    REQUIRE( before.sets.front().matchLine( QStringLiteral( "ERROR" ), matches )
             == HighlighterMatchType::WordMatch );
    REQUIRE( owner.currentActiveSet().matchLine( QStringLiteral( "ERROR" ), matches )
             == HighlighterMatchType::WordMatch );

    HighlightersDialog dialog;
    auto* const pattern = dialog.findChild<QLineEdit*>( QStringLiteral( "patternEdit" ) );
    REQUIRE( pattern != nullptr );
    QElapsedTimer deadline;
    deadline.start();
    while ( pattern->text() != makeRegressionPatterns().front().pattern && deadline.elapsed() < 500 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
    }
    REQUIRE( pattern->text() == makeRegressionPatterns().front().pattern );
    auto published = before;
    QObject::connect( &dialog, &HighlightersDialog::optionsChanged, &dialog,
                      [ & ] { published = owner.snapshot(); } );
    pattern->selectAll();
    QTest::keyClicks( pattern, "WARNING" );
    requireSameState( owner.snapshot(), before );
    dialog.buttonBox->button( QDialogButtonBox::Apply )->click();
    REQUIRE( published.revision == before.revision + 1 );
    REQUIRE( published.sets.front().highlighters().front().pattern() == QStringLiteral( "WARNING" ) );
    matches.clear();
    CHECK( published.sets.front().matchLine( QStringLiteral( "WARNING" ), matches )
           == HighlighterMatchType::WordMatch );
    matches.clear();
    CHECK( published.sets.front().matchLine( QStringLiteral( "ERROR" ), matches )
           == HighlighterMatchType::NoMatch );
    matches.clear();
    REQUIRE( owner.currentActiveSet().matchLine( QStringLiteral( "WARNING" ), matches )
             == HighlighterMatchType::WordMatch );
    matches.clear();
    REQUIRE( owner.currentActiveSet().matchLine( QStringLiteral( "ERROR" ), matches )
             == HighlighterMatchType::NoMatch );
}

#ifdef KLOGG_HAS_VECTORSCAN
TEST_CASE( "Vectorscan match contexts require explicit pattern counts", "[vectorscan][construction]" )
{
    REQUIRE_FALSE( ( std::is_convertible_v<std::size_t, HsMatcherContext> ) );
    REQUIRE( std::is_default_constructible_v<HsMatcherContext> );
    HsMatcherContext context{ 3 };
    REQUIRE( context.matchingPatterns.size() == 3 );
}
#endif

TEST_CASE( "Configured regex backend matches the built product", "[vectorscan][backend]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

#ifdef KLOGG_HAS_VECTORSCAN
    REQUIRE( config.regexpEngine() == RegexpEngine::Vectorscan );
#else
    REQUIRE( config.regexpEngine() == RegexpEngine::QRegularExpression );
#endif
}

TEST_CASE( "Prepared highlighter matchers keep their captured backend across preference changes",
           "[highlighter][review-highlighters][matcher-backend]" )
{
    ScopedRegexpEngine engine( RegexpEngine::QRegularExpression );
    const auto patterns = makeRegressionPatterns();
    MultiRegularExpression prepared( patterns );
    REQUIRE( prepared.isValid() );
    Configuration::get().setRegexpEnging( RegexpEngine::Vectorscan );
    auto matcher = prepared.createMatcher();
    const auto result = matcher->match( "ERROR https://example.invalid/evidence" );
    REQUIRE( result.size() == patterns.size() );
    CHECK( result[ 0 ].second );
    CHECK( result[ 1 ].second );
}

TEST_CASE( "MultiRegularExpression matches representative highlighter patterns",
           "[vectorscan][backend]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    const auto patterns = makeRegressionPatterns();
    MultiRegularExpression expression( patterns );

    REQUIRE( expression.isValid() );

    const auto matcher = expression.createMatcher();
    const auto matches = matcher->match( std::string_view{ "ERROR https://example.com" } );

    REQUIRE( matches.size() == patterns.size() );
    REQUIRE( matches[ 0 ].second );
    REQUIRE( matches[ 1 ].second );
}

TEST_CASE( "HighlighterSet compile and match follow the product backend",
           "[vectorscan][highlighter]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    QTemporaryDir tempDir( QDir::tempPath() + QStringLiteral( "/vectorscan-unit-XXXXXX" ) );
    REQUIRE( tempDir.isValid() );

    auto set = loadHighlighterSet( tempDir.filePath( QStringLiteral( "highlighter.ini" ) ),
                                   makeRegressionPatterns() );
    REQUIRE_FALSE( set.isEmpty() );

    set.compile();

    HighlightedMatchRanges matches;
    const auto matchType = set.matchLine( QStringLiteral( "ERROR https://example.com" ), matches );

    REQUIRE( matchType == HighlighterMatchType::WordMatch );
    REQUIRE_FALSE( matches.empty() );
}

TEST_CASE( "HighlighterSetCollection restore compiles the active set",
           "[vectorscan][highlighter]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    QTemporaryDir tempDir( QDir::tempPath() + QStringLiteral( "/vectorscan-collection-XXXXXX" ) );
    REQUIRE( tempDir.isValid() );

    auto collection = loadHighlighterSetCollection(
        tempDir.filePath( QStringLiteral( "collection.ini" ) ), makeRegressionPatterns() );

    REQUIRE( collection.hasSet( QStringLiteral( "vectorscan-regression" ) ) );

    HighlightedMatchRanges matches;
    const auto matchType = collection.currentActiveSet().matchLine(
        QStringLiteral( "ERROR https://example.com" ), matches );

    REQUIRE( matchType == HighlighterMatchType::WordMatch );
    REQUIRE_FALSE( matches.empty() );
}

TEST_CASE( "HighlighterSet matchLine produces matches regardless of selection context",
           "[vectorscan][highlighter]" )
{
    auto& config = Configuration::getSynced();
    configureProductLikeRegexpEngine( config );

    QTemporaryDir tempDir( QDir::tempPath() + QStringLiteral( "/vectorscan-selection-XXXXXX" ) );
    REQUIRE( tempDir.isValid() );

    auto collection = loadHighlighterSetCollection(
        tempDir.filePath( QStringLiteral( "collection.ini" ) ), makeRegressionPatterns() );

    const auto& set = collection.currentActiveSet();

    // matchLine should always return matches -- it has no knowledge of selection state
    const auto testLine = QStringLiteral( "ERROR at https://example.com/path?q=1" );

    HighlightedMatchRanges matches1;
    const auto type1 = set.matchLine( testLine, matches1 );
    REQUIRE( type1 == HighlighterMatchType::WordMatch );
    REQUIRE( matches1.matches().size() == 2 );

    // Call again -- results should be consistent
    HighlightedMatchRanges matches2;
    const auto type2 = set.matchLine( testLine, matches2 );
    REQUIRE( type2 == type1 );
    REQUIRE( matches2.matches().size() == 2 );
}
