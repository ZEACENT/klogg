/*
 * Copyright (C) 2009, 2010 Nicolas Bonnefon and other contributors
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

#include "predefinedfiltersdialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QTimer>
#include <QToolButton>
#include <qboxlayout.h>
#include <qcheckbox.h>
#include <qglobal.h>
#include <qwidget.h>

#include "filterfavoritesmodel.h"
#include "iconloader.h"
#include "log.h"
#include "predefinedfilters.h"

class CenteredCheckbox : public QWidget {
  public:
    explicit CenteredCheckbox( QWidget* parent = nullptr )
        : QWidget( parent )
    {
        auto layout = new QHBoxLayout;
        layout->setAlignment( Qt::AlignCenter );
        checkbox_ = new QCheckBox;
        layout->addWidget( checkbox_ );
        this->setLayout( layout );

        QPalette palette = this->palette();
        palette.setColor( QPalette::Base, palette.color( QPalette::Window ) );
        checkbox_->setPalette( palette );
    }

    bool isChecked() const
    {
        return checkbox_->isChecked();
    }

    void setChecked( bool isChecked )
    {
        checkbox_->setChecked( isChecked );
    }

  private:
    QCheckBox* checkbox_;
};

PredefinedFiltersDialog::PredefinedFiltersDialog( QWidget* parent )
    : QDialog( parent )
{
    setupUi( this );

    auto& favoritesModel = FilterFavoritesModel::instance();
    favoritesModel.synchronizeFromStorage();
    baseFavorites_ = favoritesModel.favorites();
    populateFiltersTable( baseFavorites_ );

    connect( addFilterButton, &QToolButton::clicked, this, &PredefinedFiltersDialog::addFilter );
    connect( removeFilterButton, &QToolButton::clicked, this,
             &PredefinedFiltersDialog::removeFilter );
    connect( upButton, &QToolButton::clicked, this, &PredefinedFiltersDialog::moveFilterUp );
    connect( downButton, &QToolButton::clicked, this, &PredefinedFiltersDialog::moveFilterDown );
    connect( importFilterButton, &QToolButton::clicked, this,
             &PredefinedFiltersDialog::importFilters );
    connect( exportFilterButton, &QToolButton::clicked, this,
             &PredefinedFiltersDialog::exportFilters );

    connect( buttonBox, &QDialogButtonBox::clicked, this,
             &PredefinedFiltersDialog::resolveStandardButton );

    connect( filtersTableWidget, &QTableWidget::currentCellChanged, this,
             &PredefinedFiltersDialog::onCurrentCellChanged );

    QTimer::singleShot( 0, this, [ this ] {
        IconLoader iconLoader( this );

        addFilterButton->setIcon( iconLoader.load( "icons8-plus" ) );
        removeFilterButton->setIcon( iconLoader.load( "icons8-minus" ) );
        upButton->setIcon( iconLoader.load( "icons8-up" ) );
        downButton->setIcon( iconLoader.load( "icons8-down-arrow" ) );
    } );
}

PredefinedFiltersDialog::PredefinedFiltersDialog( const QString& newFilter, QWidget* parent )
    : PredefinedFiltersDialog( parent )
{
    if ( !newFilter.isEmpty() ) {
        addFilterRow( newFilter );
    }
}

void PredefinedFiltersDialog::updateButtons()
{
    const auto filtersCount = filtersTableWidget->rowCount();
    removeFilterButton->setEnabled( filtersCount > 0 );

    updateUpDownButtons( filtersTableWidget->currentRow() );
}

void PredefinedFiltersDialog::onCurrentCellChanged( int currentRow, int currentColumn,
                                                    int previousRow, int previousColumn )
{
    Q_UNUSED( currentColumn )
    Q_UNUSED( previousRow )
    Q_UNUSED( previousColumn )

    updateUpDownButtons( currentRow );
}

void PredefinedFiltersDialog::updateUpDownButtons( int currentRow )
{
    upButton->setEnabled( currentRow > 0 );
    downButton->setEnabled( currentRow < filtersTableWidget->rowCount() - 1 );
}

void PredefinedFiltersDialog::populateFiltersTable(
    const PredefinedFiltersCollection::Collection& filters )
{
    filtersTableWidget->clear();

    filtersTableWidget->setRowCount( static_cast<int>( filters.size() ) );
    filtersTableWidget->setColumnCount( 3 );

    filtersTableWidget->setHorizontalHeaderLabels( QStringList() << tr( "Name" ) << tr( "Pattern" )
                                                                 << tr( "Regex" ) );

    int filterIndex = 0;
    for ( const auto& filter : filters ) {
        filtersTableWidget->setItem( filterIndex, 0, new QTableWidgetItem( filter.name ) );
        filtersTableWidget->setItem( filterIndex, 1, new QTableWidgetItem( filter.pattern ) );
        auto* regexCheckbox = new CenteredCheckbox;
        regexCheckbox->setChecked( filter.useRegex );
        filtersTableWidget->setCellWidget( filterIndex, 2, regexCheckbox );

        filterIndex++;
    }

    filtersTableWidget->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::ResizeToContents );
    filtersTableWidget->horizontalHeader()->setSectionResizeMode( 1, QHeaderView::Stretch );
    filtersTableWidget->verticalHeader()->setSectionResizeMode( QHeaderView::ResizeToContents );
    filtersTableWidget->setWordWrap( false );

    updateButtons();
}

bool PredefinedFiltersDialog::saveSettings()
{
    auto& favoritesModel = FilterFavoritesModel::instance();
    favoritesModel.synchronizeFromStorage();
    if ( favoritesModel.favorites() != baseFavorites_ ) {
        QMessageBox::warning(
            this, tr( "klogg" ),
            tr( "Filter favorites changed outside this dialog. Reopen the dialog and try again." ) );
        return false;
    }

    const auto updatedFavorites = readFiltersTable();
    favoritesModel.replaceFavorites( updatedFavorites );
    baseFavorites_ = updatedFavorites;
    return true;
}

PredefinedFiltersCollection::Collection PredefinedFiltersDialog::readFiltersTable() const
{
    const auto rows = filtersTableWidget->rowCount();

    PredefinedFiltersCollection::Collection currentFilters;

    for ( auto i = 0; i < rows; ++i ) {
        if ( nullptr == filtersTableWidget->item( i, 0 )
             || nullptr == filtersTableWidget->item( i, 1 ) ) {
            continue;
        }

        const auto name = filtersTableWidget->item( i, 0 )->text();
        const auto value = filtersTableWidget->item( i, 1 )->text();

        const auto useRegexCheckbox
            = static_cast<CenteredCheckbox*>( filtersTableWidget->cellWidget( i, 2 ) );
        const auto useRegex = useRegexCheckbox ? useRegexCheckbox->isChecked() : false;

        if ( !name.isEmpty() && !value.isEmpty() ) {
            currentFilters.push_back( { name, value, useRegex } );
        }
    }

    return currentFilters;
}

void PredefinedFiltersDialog::addFilter()
{
    addFilterRow( {} );
}

void PredefinedFiltersDialog::addFilterRow( const QString& newFilter )
{
    const auto newRow = filtersTableWidget->rowCount();
    filtersTableWidget->setRowCount( newRow + 1 );
    filtersTableWidget->setItem( newRow, 1, new QTableWidgetItem( newFilter ) );
    filtersTableWidget->setItem( newRow, 0, new QTableWidgetItem( "" ) );
    auto regexCheckBox = new CenteredCheckbox;
    filtersTableWidget->setCellWidget( newRow, 2, regexCheckBox );

    filtersTableWidget->scrollToItem( filtersTableWidget->item( newRow, 0 ) );
    filtersTableWidget->setCurrentCell( newRow, 0 );
    filtersTableWidget->editItem( filtersTableWidget->item( newRow, 0 ) );

    filtersTableWidget->resizeRowToContents( newRow );
}

void PredefinedFiltersDialog::removeFilter()
{
    filtersTableWidget->removeRow( filtersTableWidget->currentRow() );

    updateButtons();
}

void PredefinedFiltersDialog::moveFilterUp()
{
    const auto currentRow = filtersTableWidget->currentRow();
    const auto selectedColumn = filtersTableWidget->currentColumn();

    if ( currentRow >= 0 ) {
        swapFilters( currentRow, currentRow - 1, selectedColumn );
    }
}

void PredefinedFiltersDialog::moveFilterDown()
{
    const auto currentRow = filtersTableWidget->currentRow();
    const auto selectedColumn = filtersTableWidget->currentColumn();

    if ( currentRow >= 0 ) {
        swapFilters( currentRow, currentRow + 1, selectedColumn );
    }
}

void PredefinedFiltersDialog::swapFilters( int currentRow, int newRow, int selectedColumn )
{
    QTimer::singleShot( 0, this, [ this, currentRow, newRow, selectedColumn ] {
        for ( int column = 0; column < filtersTableWidget->columnCount(); ++column ) {
            auto currentUseRegex = static_cast<CenteredCheckbox*>(
                filtersTableWidget->cellWidget( currentRow, column ) );
            auto newUseRegex = static_cast<CenteredCheckbox*>(
                filtersTableWidget->cellWidget( newRow, column ) );

            if ( currentUseRegex && newUseRegex ) {
                const auto currentCheckState = currentUseRegex->isChecked();
                const auto newCheckState = newUseRegex->isChecked();
                currentUseRegex->setChecked( newCheckState );
                newUseRegex->setChecked( currentCheckState );
            }
            else {
                auto currentItem = filtersTableWidget->takeItem( currentRow, column );
                auto newItem = filtersTableWidget->takeItem( newRow, column );

                filtersTableWidget->setItem( newRow, column, currentItem );
                filtersTableWidget->setItem( currentRow, column, newItem );
            }
        }
        filtersTableWidget->setCurrentCell( newRow, selectedColumn );
    } );
}

void PredefinedFiltersDialog::importFilters()
{
    const auto file
        = QFileDialog::getOpenFileName( this, tr( "Select file to import" ), "",
                                        tr( "Filter favorites (*.conf);;All files (*)" ) );

    if ( file.isEmpty() ) {
        return;
    }

    importFiltersFromFile( file );
}

void PredefinedFiltersDialog::importFiltersFromFile( const QString& file )
{
    LOG_DEBUG << "Loading predefined filters from " << file;
    const auto result = PredefinedFiltersCollection::tryLoadFromFile( file );
    if ( result.status != PredefinedFiltersCollection::LoadStatus::Success ) {
        QMessageBox::warning( this, tr( "klogg" ),
                              tr( "Unable to import filter favorites from the selected file." ) );
        return;
    }

    populateFiltersTable( result.filters );
}

void PredefinedFiltersDialog::exportFilters()
{
    auto file = QFileDialog::getSaveFileName( this, tr( "Export filter favorites" ), "",
                                              tr( "Filter favorites (*.conf)" ) );

    if ( file.isEmpty() ) {
        return;
    }

    if ( !file.endsWith( ".conf" ) ) {
        file += ".conf";
    }

    exportFiltersToFile( file );
}

void PredefinedFiltersDialog::exportFiltersToFile( const QString& file )
{
    if ( !PredefinedFiltersCollection::saveToFile( file, readFiltersTable() ) ) {
        QMessageBox::warning( this, tr( "klogg" ),
                              tr( "Unable to export filter favorites to the selected file." ) );
    }
}

void PredefinedFiltersDialog::resolveStandardButton( QAbstractButton* button )
{
    LOG_DEBUG << "PredefinedFiltersDialog::resolveStandardButton";

    const auto role = buttonBox->buttonRole( button );

    switch ( role ) {
    case QDialogButtonBox::RejectRole:
        reject();
        return;

    case QDialogButtonBox::ApplyRole:
        saveSettings();
        break;

    case QDialogButtonBox::AcceptRole:
        if ( saveSettings() ) {
            accept();
        }
        break;
    default:
        LOG_ERROR << "PredefinedFiltersDialog::resolveStandardButton unhandled role: " << role;
        return;
    }
}
