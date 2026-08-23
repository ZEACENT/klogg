/*
 * Copyright (C) 2010, 2013 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
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

#include "log.h"

#include <limits>
#include <QCompleter>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QStringListModel>
#include <QToolButton>
#include <qcheckbox.h>
#include <qkeysequence.h>
#include <qlineedit.h>
#include <qregularexpression.h>

#include "configuration.h"
#include "iconloader.h"
#include "qfnotifications.h"
#include "savedsearches.h"
#include "shortcuts.h"

#include "quickfindwidget.h"

static constexpr int NotificationTimeout = 5000;

const QString QFNotification::REACHED_EOF = "Reached end of file, no occurrence found.";
const QString QFNotification::REACHED_BOF = "Reached beginning of file, no occurrence found.";
const QString QFNotification::INTERRUPTED = "Search interrupted";

QuickFindWidget::QuickFindWidget( QWidget* parent )
    : QWidget( parent )
{
    // ui_.setupUi( this );
    // setFocusProxy(ui_.findEdit);
    // setProperty("topBorder", true);
    auto* layout = new QHBoxLayout( this );

    layout->setContentsMargins( 6, 0, 6, 6 );
    layout->setSpacing( 4 );

    closeButton_
        = setupToolButton( QLatin1String( "" ), QLatin1String( "darkclosebutton" ) );
    closeButton_->setShortcut( QKeySequence::Cancel );
    layout->addWidget( closeButton_ );

    searchLineCompleter_ = new QCompleter( SavedSearches::getSynced().recentSearches(), this );
    editQuickFind_ = new QComboBox( this );
    editQuickFind_->setEditable( true );
    editQuickFind_->setCompleter( searchLineCompleter_ );
    editQuickFind_->addItems( SavedSearches::getSynced().recentSearches() );
    // FIXME: set MinimumSize might be too constraining
    editQuickFind_->setMinimumSize( QSize( 150, 0 ) );
    editQuickFind_->lineEdit()->setMaxLength( std::numeric_limits<int>::max() / 1024 );
    layout->addWidget( editQuickFind_, 1 );

    ignoreCaseCheck_ = new QCheckBox( "Ignore &case" );
    ignoreCaseCheck_->setChecked( Configuration::get().qfIgnoreCase() );
    layout->addWidget( ignoreCaseCheck_ );

    matchWholeWordCheck_ = new QCheckBox( "Whole &word" );
    matchWholeWordCheck_->setChecked( false ); // TODO: persist in config?
    layout->addWidget( matchWholeWordCheck_ );

    useRegexpCheck_ = new QCheckBox( "Re&gex" );
    useRegexpCheck_->setChecked( Configuration::get().quickfindRegexpType() == SearchRegexpType::ExtendedRegexp );
    layout->addWidget( useRegexpCheck_ );

    previousButton_
        = setupToolButton( QLatin1String( "Previous" ), QLatin1String( "arrowup" ) );
    previousButton_->setShortcut( QKeySequence::FindPrevious );
    layout->addWidget( previousButton_ );

    nextButton_
        = setupToolButton( QLatin1String( "Next" ), QLatin1String( "arrowdown" ) );
    const auto findNextBindings = findNextShortcutBindings();
    nextButton_->setShortcut(
        QKeySequence( findNextBindings.front(), QKeySequence::PortableText ) );
    layout->addWidget( nextButton_ );

    notificationText_ = new QLabel( "" );
    // FIXME: set MinimumSize might be too constraining
    int width = QFNotification::maxWidth( notificationText_ );
    notificationText_->setMinimumSize( width, 0 );
    layout->addWidget( notificationText_ );

    setMinimumWidth( minimumSizeHint().width() );

    // Behaviour
    connect( closeButton_, &QToolButton::clicked, this, &QuickFindWidget::closeHandler );
    connect( editQuickFind_->lineEdit(), &QLineEdit::textEdited, this,
             &QuickFindWidget::textChanged );
    connect( editQuickFind_->lineEdit(), &QLineEdit::returnPressed, this,
             &QuickFindWidget::returnHandler );

// Qt compatibility:
// - QCheckBox::stateChanged exists in Qt5 and Qt6.
// - QCheckBox::checkStateChanged exists only in newer Qt6 versions.
#if ( QT_VERSION >= QT_VERSION_CHECK( 6, 7, 0 ) )
    connect( ignoreCaseCheck_, &QCheckBox::checkStateChanged, this, [ this ] {
#else
    connect( ignoreCaseCheck_, &QCheckBox::stateChanged, this, [ this ] {
#endif
        textChanged();
        Configuration::get().setQfIgnoreCase( ignoreCaseCheck_->isChecked() );
        Configuration::get().save();
    } );

#if ( QT_VERSION >= QT_VERSION_CHECK( 6, 7, 0 ) )
    connect( matchWholeWordCheck_, &QCheckBox::checkStateChanged, this, [ this ] {
        textChanged();
    } );

    connect( useRegexpCheck_, &QCheckBox::checkStateChanged, this, [ this ] {
        textChanged();
        // Configuration::get().setQuickfindRegexpType(...); // Optional: persist
    } );
#else
    connect( matchWholeWordCheck_, &QCheckBox::stateChanged, this, [ this ] {
        textChanged();
    } );

    connect( useRegexpCheck_, &QCheckBox::stateChanged, this, [ this ] {
        textChanged();
    } );
#endif

    connect( previousButton_, &QToolButton::clicked, this, &QuickFindWidget::doSearchBackward );
    connect( nextButton_, &QToolButton::clicked, this, &QuickFindWidget::doSearchForward );

    // Notification timer:
    notificationTimer_ = new QTimer( this );
    notificationTimer_->setSingleShot( true );
    connect( notificationTimer_, SIGNAL( timeout() ), this, SLOT( notificationTimeout() ) );
}

void QuickFindWidget::userActivate()
{
    updateSearchHistory();
    userRequested_ = true;
    QWidget::show();
    editQuickFind_->lineEdit()->setFocus( Qt::ShortcutFocusReason );
    editQuickFind_->lineEdit()->selectAll();
}

//
// Q_SLOTS:
//

void QuickFindWidget::changeDisplayedPattern( const QString& newPattern, bool ignoreCase, bool isRegex, bool isWholeWord )
{
    // pattern is raw text here
    editQuickFind_->setEditText( newPattern );
    editQuickFind_->lineEdit()->setCursorPosition( patternCursorPosition_ );
    
    // Update checkboxes without triggering signals loop
    bool oldState = ignoreCaseCheck_->blockSignals(true);
    ignoreCaseCheck_->setChecked(ignoreCase);
    ignoreCaseCheck_->blockSignals(oldState);
    
    oldState = useRegexpCheck_->blockSignals(true);
    useRegexpCheck_->setChecked(isRegex);
    useRegexpCheck_->blockSignals(oldState);
    
    oldState = matchWholeWordCheck_->blockSignals(true);
    matchWholeWordCheck_->setChecked(isWholeWord);
    matchWholeWordCheck_->blockSignals(oldState);
}

void QuickFindWidget::notify( const QFNotification& message )
{
    LOG_DEBUG << "QuickFindWidget::notify()";

    notificationText_->setText( message.message() );
    QWidget::show();
    notificationTimer_->start( NotificationTimeout );
}

void QuickFindWidget::clearNotification()
{
    LOG_DEBUG << "QuickFindWidget::clearNotification()";

    notificationText_->setText( "" );
}

// User clicks forward arrow
void QuickFindWidget::doSearchForward()
{
    LOG_DEBUG << "QuickFindWidget::doSearchForward()";

    // The user has clicked on a button, so we assume she wants
    // the widget to stay visible.
    userRequested_ = true;

    recordSearchHistory( editQuickFind_->currentText() );
    Q_EMIT patternConfirmed( editQuickFind_->currentText(), isIgnoreCase(), isRegexSearch(), isWholeWord() );
    Q_EMIT searchForward();
}

// User clicks backward arrow
void QuickFindWidget::doSearchBackward()
{
    LOG_DEBUG << "QuickFindWidget::doSearchBackward()";

    // The user has clicked on a button, so we assume she wants
    // the widget to stay visible.
    userRequested_ = true;

    recordSearchHistory( editQuickFind_->currentText() );
    Q_EMIT patternConfirmed( editQuickFind_->currentText(), isIgnoreCase(), isRegexSearch(), isWholeWord() );
    Q_EMIT searchBackward();
}

// Same as user clicks backward arrow
void QuickFindWidget::returnHandler()
{
    doSearchForward();
}

// Close and reset flag when the user clicks 'close'
void QuickFindWidget::closeHandler()
{
    userRequested_ = false;
    this->hide();
    Q_EMIT close();
    Q_EMIT cancelSearch();
}

void QuickFindWidget::notificationTimeout()
{
    // We close the widget if the user hasn't explicitely requested it.
    if ( !userRequested_ )
        this->hide();
}

void QuickFindWidget::textChanged()
{
    patternCursorPosition_ = editQuickFind_->lineEdit()->cursorPosition();
    Q_EMIT patternUpdated( editQuickFind_->currentText(), isIgnoreCase(), isRegexSearch(),
                           isWholeWord() );
}

void QuickFindWidget::updateSearchHistory()
{
    const auto currentText = editQuickFind_->currentText();
    const auto history = SavedSearches::getSynced().recentSearches();

    editQuickFind_->blockSignals( true );
    editQuickFind_->clear();
    editQuickFind_->addItems( history );
    editQuickFind_->setEditText( currentText );
    editQuickFind_->blockSignals( false );

    searchLineCompleter_->setModel( new QStringListModel( history, searchLineCompleter_ ) );
}

void QuickFindWidget::recordSearchHistory( const QString& text )
{
    auto& searches = SavedSearches::getSynced();
    searches.addRecent( text );
    searches.save();
    updateSearchHistory();
}

void QuickFindWidget::changeEvent( QEvent* event )
{
    if ( event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange ) {
        reloadIcons();
    }
    QWidget::changeEvent( event );
}

//
// Private functions
//
void QuickFindWidget::reloadIcons()
{
    if ( !closeButton_ || !previousButton_ || !nextButton_ ) {
        return;
    }
    IconLoader iconLoader{ this };
    closeButton_->setIcon( iconLoader.load( "darkclosebutton" ) );
    previousButton_->setIcon( iconLoader.load( "arrowup" ) );
    nextButton_->setIcon( iconLoader.load( "arrowdown" ) );
}

QToolButton* QuickFindWidget::setupToolButton( const QString& text, const QString& iconName )
{
    auto* toolButton = new QToolButton( this );

    toolButton->setAutoRaise( true );
    IconLoader iconLoader{ this };
    toolButton->setIcon( iconLoader.load( iconName ) );

    if ( text.size() > 0 ) {
        toolButton->setText( text );
        toolButton->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );
    }
    else {
        toolButton->setToolButtonStyle( Qt::ToolButtonIconOnly );
    }

    return toolButton;
}

bool QuickFindWidget::isIgnoreCase() const
{
    return ( ignoreCaseCheck_->checkState() == Qt::Checked );
}

bool QuickFindWidget::isRegexSearch() const
{
    return ( useRegexpCheck_->checkState() == Qt::Checked );
}

bool QuickFindWidget::isWholeWord() const
{
    return ( matchWholeWordCheck_->checkState() == Qt::Checked );
}
