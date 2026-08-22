/*
 * Copyright (C) 2026 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef KLOGG_UIMESSAGE_H
#define KLOGG_UIMESSAGE_H

#include <cstdint>
#include <functional>

#include <QString>

class QDialog;
class QWidget;

namespace klogg::ui {

enum class MessageKind : std::uint8_t {
    Warning,
    Information,
};

using MessageHandler
    = std::function<void( MessageKind, QWidget*, const QString&, const QString& )>;

void warning( QWidget* parent, const QString& title, const QString& text );
void information( QWidget* parent, const QString& title, const QString& text );

using DialogHandler = std::function<int( QDialog& )>;
int execDialog( QDialog& dialog );

class ScopedMessageHandler final {
  public:
    explicit ScopedMessageHandler( MessageHandler handler );
    ~ScopedMessageHandler();

    ScopedMessageHandler( const ScopedMessageHandler& ) = delete;
    ScopedMessageHandler& operator=( const ScopedMessageHandler& ) = delete;
    ScopedMessageHandler( ScopedMessageHandler&& ) = delete;
    ScopedMessageHandler& operator=( ScopedMessageHandler&& ) = delete;

  private:
    std::uint64_t token_;
};

class ScopedDialogHandler final {
  public:
    explicit ScopedDialogHandler( DialogHandler handler );
    ~ScopedDialogHandler();

    ScopedDialogHandler( const ScopedDialogHandler& ) = delete;
    ScopedDialogHandler& operator=( const ScopedDialogHandler& ) = delete;
    ScopedDialogHandler( ScopedDialogHandler&& ) = delete;
    ScopedDialogHandler& operator=( ScopedDialogHandler&& ) = delete;

  private:
    std::uint64_t token_;
};

} // namespace klogg::ui

#endif // KLOGG_UIMESSAGE_H
