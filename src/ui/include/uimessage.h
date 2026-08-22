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

} // namespace klogg::ui

#endif // KLOGG_UIMESSAGE_H
