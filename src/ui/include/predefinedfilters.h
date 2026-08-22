/*
 * Copyright (C) 2009, 2010, 2011, 2013, 2014, 2015 Nicolas Bonnefon
 * and other contributors
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

#ifndef PREDEFINEDFILTERS_H_
#define PREDEFINEDFILTERS_H_

#include <QList>
#include <QString>

#include "persistable.h"

struct PredefinedFilter {
    QString name;
    QString pattern;
    bool useRegex;
};

inline bool operator==( const PredefinedFilter& left, const PredefinedFilter& right )
{
    return left.name == right.name && left.pattern == right.pattern
           && left.useRegex == right.useRegex;
}

inline bool operator!=( const PredefinedFilter& left, const PredefinedFilter& right )
{
    return !( left == right );
}

// Represents collection of filters read from settings file.
class PredefinedFiltersCollection final : public Persistable<PredefinedFiltersCollection> {
  public:
    using Collection = QList<PredefinedFilter>;

    enum class LoadStatus {
        Success,
        MissingFile,
        MalformedFile,
        UnsupportedVersion,
    };

    struct LoadResult {
        LoadStatus status;
        Collection filters;
    };

    static const char* persistableName()
    {
        return "PredefinedFiltersCollection";
    }

    Collection getSyncedFilters();
    Collection getFilters() const;
    void setFilters( const Collection& filters );
    static LoadResult tryLoadFromFile( const QString& file );
    static Collection loadFromFile( const QString& file );
    static bool saveToFile( const QString& file, const Collection& filters );

    void retrieveFromStorage( QSettings& settings );
    void saveToStorage( QSettings& settings ) const;
    void saveToStorage( const Collection& filters );

  private:
    static constexpr int PredefinedFiltersCollection_VERSION = 2;

    Collection filters_;
};

Q_DECLARE_METATYPE(PredefinedFilter)

#endif
