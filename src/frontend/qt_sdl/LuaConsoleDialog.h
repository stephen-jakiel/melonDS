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

#ifndef LUACONSOLEDIALOG_H
#define LUACONSOLEDIALOG_H

#include <QDialog>

class QPlainTextEdit;
class LuaScriptManager;

// A simple non-modal log window for Lua script print() output and errors.
// Not the tracker's real UI (that's forms.*, not yet implemented) -- this is
// primarily a debugging aid for testing scripts against memory.*/emu.*.
class LuaConsoleDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LuaConsoleDialog(LuaScriptManager* script, QWidget* parent = nullptr);

private slots:
    void onConsoleOutput(QString text);
    void onScriptStopped();

private:
    QPlainTextEdit* log;
};

#endif // LUACONSOLEDIALOG_H
