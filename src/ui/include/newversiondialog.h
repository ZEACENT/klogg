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

#ifndef KLOGG_NEWVERSIONDIALOG_H
#define KLOGG_NEWVERSIONDIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>

class QTextBrowser;

// Dialog shown when a new version of klogg is available.
// Displays version information and a scrollable, height-limited
// changelog area so that long release notes do not make the
// dialog taller than the screen.
class NewVersionDialog : public QDialog {
    Q_OBJECT

  public:
    enum Button {
        Download,
        RemindLater,
        SkipVersion,
    };

    explicit NewVersionDialog( const QString& version, const QString& url,
                               const QStringList& changes,
                               QWidget* parent = nullptr );

    Button clickedButton() const
    {
        return clickedButton_;
    }

    // Exposed for testing: the maximum height of the changelog text area.
    static constexpr int kMaxChangesHeight = 200;

  private:
    void setupUi( const QString& version, const QString& url,
                  const QStringList& changes );

    Button clickedButton_ = RemindLater;
    QTextBrowser* changesBrowser_ = nullptr;
};

#endif // KLOGG_NEWVERSIONDIALOG_H
