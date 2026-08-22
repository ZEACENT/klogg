/*
 * Copyright (C) 2024 Anton Filimonov and other contributors
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

#include "savefavoritedialog.h"

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

SaveFavoriteDialog::SaveFavoriteDialog(
    const QString& defaultName, const PredefinedFiltersCollection::Collection& existingFilters,
    QWidget* parent )
    : QDialog( parent )
    , existingFilters_( existingFilters )
{
    setObjectName( QStringLiteral( "SaveFavoriteDialog" ) );
    setWindowTitle( tr( "Save Favorite" ) );
    setMinimumWidth( 400 );

    auto* layout = new QVBoxLayout( this );

    // Create button group for mutual exclusivity
    modeButtonGroup_ = new QButtonGroup( this );

    // Create new section
    auto* createNewLayout = new QHBoxLayout();
    createNewRadio_ = new QRadioButton( tr( "Create new:" ), this );
    createNewRadio_->setObjectName( QStringLiteral( "createNewFavoriteRadioButton" ) );
    createNewRadio_->setChecked( true );
    newNameEdit_ = new QLineEdit( defaultName, this );
    newNameEdit_->setObjectName( QStringLiteral( "favoriteNameLineEdit" ) );
    createNewLayout->addWidget( createNewRadio_ );
    createNewLayout->addWidget( newNameEdit_, 1 );

    // Overwrite existing section
    auto* overwriteLayout = new QHBoxLayout();
    overwriteRadio_ = new QRadioButton( tr( "Overwrite existing:" ), this );
    overwriteRadio_->setObjectName( QStringLiteral( "overwriteFavoriteRadioButton" ) );
    existingCombo_ = new QComboBox( this );
    existingCombo_->setObjectName( QStringLiteral( "existingFavoriteComboBox" ) );
    existingCombo_->setEnabled( false );

    for ( const auto& filter : existingFilters_ ) {
        existingCombo_->addItem( filter.name );
    }

    overwriteLayout->addWidget( overwriteRadio_ );
    overwriteLayout->addWidget( existingCombo_, 1 );

    // Add radio buttons to button group for mutual exclusivity
    modeButtonGroup_->addButton( createNewRadio_, 0 );
    modeButtonGroup_->addButton( overwriteRadio_, 1 );

    // Enable overwrite option only if there are existing favorites
    overwriteRadio_->setEnabled( !existingFilters_.isEmpty() );
    if ( existingFilters_.isEmpty() ) {
        existingCombo_->addItem( tr( "(no favorites)" ) );
    }

    layout->addLayout( createNewLayout );
    layout->addLayout( overwriteLayout );

    // Button box
    auto* buttonBox
        = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
    buttonBox->setObjectName( QStringLiteral( "saveFavoriteButtonBox" ) );
    layout->addWidget( buttonBox );

    connect( buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept );
    connect( buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject );

    connect( createNewRadio_, &QRadioButton::toggled, this, &SaveFavoriteDialog::onModeChanged );
    connect( overwriteRadio_, &QRadioButton::toggled, this, &SaveFavoriteDialog::onModeChanged );
    connect( newNameEdit_, &QLineEdit::textChanged, this,
             &SaveFavoriteDialog::updateOkButtonState );

    updateOkButtonState();
}

bool SaveFavoriteDialog::isCreateNew() const
{
    return createNewRadio_->isChecked();
}

QString SaveFavoriteDialog::favoriteName() const
{
    if ( isCreateNew() ) {
        return newNameEdit_->text().trimmed();
    }
    else {
        return existingCombo_->currentText();
    }
}

int SaveFavoriteDialog::selectedExistingIndex() const
{
    if ( isCreateNew() || existingFilters_.isEmpty() ) {
        return -1;
    }
    return existingCombo_->currentIndex();
}

void SaveFavoriteDialog::updateOkButtonState()
{
    auto* okButton = findChild<QDialogButtonBox*>()->button( QDialogButtonBox::Ok );
    if ( isCreateNew() ) {
        okButton->setEnabled( !newNameEdit_->text().trimmed().isEmpty() );
    }
    else {
        okButton->setEnabled( !existingFilters_.isEmpty() );
    }
}

void SaveFavoriteDialog::onModeChanged()
{
    newNameEdit_->setEnabled( createNewRadio_->isChecked() );
    existingCombo_->setEnabled( overwriteRadio_->isChecked() && !existingFilters_.isEmpty() );
    updateOkButtonState();
}
