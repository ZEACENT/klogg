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

#include "uimessage.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <QDialog>
#include <QMessageBox>

namespace klogg::ui {
namespace {
struct HandlerEntry {
    std::uint64_t token = 0;
    MessageHandler handler;
};

struct DialogHandlerEntry {
    std::uint64_t token = 0;
    DialogHandler handler;
};

std::vector<HandlerEntry>& handlerStack()
{
    static thread_local std::vector<HandlerEntry> handlers;
    return handlers;
}

std::vector<DialogHandlerEntry>& dialogHandlerStack()
{
    static thread_local std::vector<DialogHandlerEntry> handlers;
    return handlers;
}

std::uint64_t nextToken()
{
    static thread_local std::uint64_t token = 0;
    return ++token;
}

void present( MessageKind kind, QWidget* parent, const QString& title, const QString& text )
{
    auto handler = handlerStack().empty() ? MessageHandler{} : handlerStack().back().handler;
    if ( handler ) {
        handler( kind, parent, title, text );
        return;
    }

    if ( kind == MessageKind::Warning ) {
        QMessageBox::warning( parent, title, text );
    }
    else {
        QMessageBox::information( parent, title, text );
    }
}
} // namespace

void warning( QWidget* parent, const QString& title, const QString& text )
{
    present( MessageKind::Warning, parent, title, text );
}

void information( QWidget* parent, const QString& title, const QString& text )
{
    present( MessageKind::Information, parent, title, text );
}

int execDialog( QDialog& dialog )
{
    auto handler = dialogHandlerStack().empty() ? DialogHandler{}
                                                : dialogHandlerStack().back().handler;
    return handler ? handler( dialog ) : dialog.exec();
}

ScopedMessageHandler::ScopedMessageHandler( MessageHandler handler )
    : token_( nextToken() )
{
    handlerStack().push_back( { token_, std::move( handler ) } );
}

ScopedMessageHandler::~ScopedMessageHandler()
{
    auto& handlers = handlerStack();
    const auto entry = std::find_if( handlers.begin(), handlers.end(), [ this ]( const auto& item ) {
        return item.token == token_;
    } );
    if ( entry != handlers.end() ) {
        handlers.erase( entry );
    }
}

ScopedDialogHandler::ScopedDialogHandler( DialogHandler handler )
    : token_( nextToken() )
{
    dialogHandlerStack().push_back( { token_, std::move( handler ) } );
}

ScopedDialogHandler::~ScopedDialogHandler()
{
    auto& handlers = dialogHandlerStack();
    const auto entry = std::find_if( handlers.begin(), handlers.end(), [ this ]( const auto& item ) {
        return item.token == token_;
    } );
    if ( entry != handlers.end() ) {
        handlers.erase( entry );
    }
}

} // namespace klogg::ui
