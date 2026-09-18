/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "LuaConsoleDialog.h"
#include "LuaScriptManager.h"

#include <QPlainTextEdit>
#include <QVBoxLayout>

LuaConsoleDialog::LuaConsoleDialog(LuaScriptManager* script, QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Lua Console");
    resize(500, 300);
    setAttribute(Qt::WA_DeleteOnClose);

    log = new QPlainTextEdit(this);
    log->setReadOnly(true);
    log->setMaximumBlockCount(2000);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(log);

    connect(script, &LuaScriptManager::consoleOutput, this, &LuaConsoleDialog::onConsoleOutput);
    connect(script, &LuaScriptManager::scriptStopped, this, &LuaConsoleDialog::onScriptStopped);
    connect(script, &LuaScriptManager::consoleCleared, this, &LuaConsoleDialog::onConsoleCleared);
}

void LuaConsoleDialog::onConsoleOutput(QString text)
{
    log->appendPlainText(text);
}

void LuaConsoleDialog::onScriptStopped()
{
    log->appendPlainText("[script stopped]");
}

void LuaConsoleDialog::onConsoleCleared()
{
    log->clear();
}
