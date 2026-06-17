/*
 * Copyright (C) 2019 Anton Filimonov and other contributors
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

#include "newversiondialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

NewVersionDialog::NewVersionDialog( const QString& version, const QString& url,
                                    const QStringList& changes, QWidget* parent )
    : QDialog( parent )
{
    setWindowTitle( tr( "New Version Available" ) );
    setupUi( version, url, changes );
}

void NewVersionDialog::setupUi( const QString& version, const QString& url,
                                const QStringList& changes )
{
    auto* mainLayout = new QVBoxLayout( this );

    // Header: version info with clickable link
    auto* headerLabel = new QLabel( this );
    headerLabel->setTextFormat( Qt::RichText );
    headerLabel->setTextInteractionFlags( Qt::TextBrowserInteraction );
    headerLabel->setOpenExternalLinks( true );
    headerLabel->setText(
        tr( "<p>A new version of klogg (%1) is available for download.</p>"
            "<p><a href=\"%2\">%2</a></p>" )
            .arg( version, url ) );
    headerLabel->setWordWrap( true );
    mainLayout->addWidget( headerLabel );

    // Changelog area (scrollable, height-limited)
    if ( !changes.isEmpty() ) {
        auto* changesLabel = new QLabel( tr( "Important changes:" ), this );
        mainLayout->addWidget( changesLabel );

        changesBrowser_ = new QTextBrowser( this );
        changesBrowser_->setReadOnly( true );
        changesBrowser_->setOpenExternalLinks( true );
        changesBrowser_->setMaximumHeight( kMaxChangesHeight );
        changesBrowser_->setMinimumHeight( kMaxChangesHeight / 2 );

        // Build the changelog text. The release body from GitHub is
        // markdown; use setMarkdown when available (Qt >= 5.14),
        // otherwise fall back to plain text.
        QString changelogText;
        for ( const auto& change : changes ) {
            if ( !changelogText.isEmpty() ) {
                changelogText.append( QLatin1String( "\n\n" ) );
            }
            changelogText.append( change );
        }

#if ( QT_VERSION >= QT_VERSION_CHECK( 5, 14, 0 ) )
        changesBrowser_->setMarkdown( changelogText );
#else
        changesBrowser_->setPlainText( changelogText );
#endif

        mainLayout->addWidget( changesBrowser_, /*stretch=*/1 );
    }

    // Buttons
    auto* buttonBox = new QDialogButtonBox( this );

    auto* downloadButton
        = buttonBox->addButton( tr( "Download" ), QDialogButtonBox::AcceptRole );
    auto* remindButton
        = buttonBox->addButton( tr( "Remind Later" ), QDialogButtonBox::RejectRole );
    auto* skipButton
        = buttonBox->addButton( tr( "Skip This Version" ),
                                QDialogButtonBox::DestructiveRole );

    buttonBox->setCenterButtons( false );

    mainLayout->addWidget( buttonBox );

    // Connect buttons
    connect( downloadButton, &QPushButton::clicked, this, [ this ] {
        clickedButton_ = Download;
        accept();
    } );
    connect( remindButton, &QPushButton::clicked, this, [ this ] {
        clickedButton_ = RemindLater;
        reject();
    } );
    connect( skipButton, &QPushButton::clicked, this, [ this ] {
        clickedButton_ = SkipVersion;
        reject();
    } );

    // Prevent the dialog from growing beyond the screen
    setMaximumHeight( 600 );
}
