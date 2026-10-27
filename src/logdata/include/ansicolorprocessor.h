#ifndef ANSICOLORPROCESSOR_H
#define ANSICOLORPROCESSOR_H

#include <optional>

#include <QString>
#include <QRegularExpression>
#include <QtGlobal>

#include "containers.h"
#include "linetypes.h"

enum class AnsiProcessingMode {
    Plain,
    Strip,
    Render,
};

struct AnsiColorSpan {
    LineColumn startColumn;
    LineLength length;
    std::optional<quint32> foreground;
    std::optional<quint32> background;
};

struct ProcessedAnsiLine {
    QString text;
    klogg::vector<AnsiColorSpan> colorSpans;
};

ProcessedAnsiLine processAnsiSequences( QString line, AnsiProcessingMode mode );
// Common decoded-text normalization: ANSI first, then prefilter removal, with
// color coordinates expressed in the resulting text (before Tab expansion).
ProcessedAnsiLine processLogLine( QString line, AnsiProcessingMode mode,
                                  const QRegularExpression& prefilter );

#endif // ANSICOLORPROCESSOR_H
