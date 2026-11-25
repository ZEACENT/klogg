#include <catch2/catch.hpp>

#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QLineEdit>
#include <QSignalSpy>
#include <QToolButton>

#include "configuration.h"
#include "searchtoolbar.h"
#include "test_utils.h"

TEST_CASE( "Filter draft installation is one quiet revision without focus or search",
           "[searchtoolbar][filter-draft-publication]" )
{
    const auto previousAutoRun = Configuration::get().autoRunSearchOnPatternChange();
    struct RestoreAutoRun {
        bool previous;
        ~RestoreAutoRun() { Configuration::get().setAutoRunSearchOnPatternChange( previous ); }
    } restore{ previousAutoRun };
    Configuration::get().setAutoRunSearchOnPatternChange( true );
    SearchToolbar toolbar;
    toolbar.show();
    toolbar.searchButton()->setFocus();
    REQUIRE( waitUiState( [&] { return QApplication::focusWidget() == toolbar.searchButton(); } ) );
    const auto before = toolbar.filterDraftSnapshot();
    auto candidate = before.filter;
    candidate.text = QStringLiteral( "\"ERROR\" | \"WARN\"" );
    candidate.matchCase = true;
    candidate.useRegexp = true;
    candidate.inverse = true;
    candidate.boolean = true;
    candidate.autoRefresh = true;
    candidate.keepResults = true;
    QSignalSpy search{ &toolbar, &SearchToolbar::searchRequested };
    QSignalSpy text{ &toolbar, &SearchToolbar::searchTextChanged };
    QSignalSpy options{ &toolbar, &SearchToolbar::optionsChanged };
    QSignalSpy matchCase{ &toolbar, &SearchToolbar::matchCaseChanged };
    QSignalSpy refresh{ &toolbar, &SearchToolbar::autoRefreshChanged };
    QSignalSpy input{ toolbar.searchLineEdit()->lineEdit(), &QLineEdit::textChanged };
    QSignalSpy focus{ qApp, &QApplication::focusChanged };
    REQUIRE( toolbar.installFilterDraft( before, candidate ) );
    const auto after = toolbar.filterDraftSnapshot();
    CHECK( after.filter == candidate );
    CHECK( after.revision == before.revision + 1 );
    CHECK( toolbar.currentSearchText() == candidate.text );
    CHECK( toolbar.currentRegularExpressionPattern() == candidate.regularExpressionPattern() );
    CHECK( toolbar.searchLineCompleter()->caseSensitivity() == Qt::CaseSensitive );
    CHECK( toolbar.isAutoRefresh() );
    CHECK( toolbar.isKeepResultsChecked() );
    CHECK( QApplication::focusWidget() == toolbar.searchButton() );
    CHECK( search.empty() );
    CHECK( text.empty() );
    CHECK( options.empty() );
    CHECK( matchCase.empty() );
    CHECK( refresh.empty() );
    CHECK( input.empty() );
    CHECK( focus.empty() );
    toolbar.setMatchCase( false );
    CHECK( options.size() == 1 );
    CHECK( matchCase.size() == 1 );
    CHECK( toolbar.filterDraftSnapshot().revision == after.revision + 1 );
}

TEST_CASE( "Filter draft compare-and-install rejects stale edits and preserves no-op selection",
           "[searchtoolbar][filter-draft-publication]" )
{
    SearchToolbar toolbar;
    auto* edit = toolbar.searchLineEdit()->lineEdit();
    edit->setText( QStringLiteral( "before" ) );
    const auto before = toolbar.filterDraftSnapshot();
    SECTION( "human ABA" ) {
        edit->setText( QStringLiteral( "other" ) );
        edit->setText( QStringLiteral( "before" ) );
        const auto current = toolbar.filterDraftSnapshot();
        auto candidate = before.filter;
        candidate.text = QStringLiteral( "after" );
        CHECK_FALSE( toolbar.installFilterDraft( before, candidate ) );
        CHECK( toolbar.filterDraftSnapshot().revision == current.revision );
        CHECK( toolbar.currentSearchText() == "before" );
    }
    SECTION( "no-op" ) {
        edit->setSelection( 1, 3 );
        const auto cursor = edit->cursorPosition();
        REQUIRE( toolbar.installFilterDraft( before, before.filter ) );
        CHECK( toolbar.filterDraftSnapshot().revision == before.revision );
        CHECK( edit->cursorPosition() == cursor );
        CHECK( edit->selectedText() == "efo" );
    }
}

TEST_CASE( "Filter draft installation rejects text the actual editor would truncate",
           "[searchtoolbar][filter-draft-publication][filter-draft-length]" )
{
    SearchToolbar toolbar;
    toolbar.searchLineEdit()->lineEdit()->setMaxLength( 4 );
    const auto before = toolbar.filterDraftSnapshot();
    auto candidate = before.filter;
    candidate.text = QStringLiteral( "ERROR" );
    CHECK_FALSE( toolbar.canInstallFilterDraft( before, candidate ) );
    CHECK_FALSE( toolbar.installFilterDraft( before, candidate ) );
    CHECK( toolbar.filterDraftSnapshot().filter == before.filter );
    CHECK( toolbar.filterDraftSnapshot().revision == before.revision );
}

TEST_CASE( "Detached filter specifications construct the native matching recipe",
           "[searchtoolbar][filter-draft-publication]" )
{
    SearchToolbar::FilterSpec spec;
    spec.text = QStringLiteral( "a.*b" );
    spec.matchCase = true;
    spec.inverse = true;
    spec.boolean = true;
    const auto literal = spec.regularExpressionPattern();
    CHECK( literal.pattern == spec.text );
    CHECK( literal.isCaseSensitive );
    CHECK( literal.isExclude );
    CHECK( literal.isBoolean );
    CHECK( literal.isPlainText );
    spec.useRegexp = true;
    CHECK_FALSE( spec.regularExpressionPattern().isPlainText );
}
