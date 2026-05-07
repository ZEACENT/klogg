#ifndef ANSICOLORPROCESSOR_H
#define ANSICOLORPROCESSOR_H

#include <optional>

#include <QString>
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

#endif // ANSICOLORPROCESSOR_H
