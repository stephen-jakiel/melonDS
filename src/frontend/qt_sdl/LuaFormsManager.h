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

#ifndef LUAFORMSMANAGER_H
#define LUAFORMSMANAGER_H

#include <QColor>
#include <QEvent>
#include <QMap>
#include <QMetaObject>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>
#include <vector>

#include "LuaScriptManager.h" // LuaDrawCommand

class QWidget;

// Owns every Lua-created form/control (forms.newform/button/label/etc.) and
// lives on the UI thread, since QWidgets can only be touched from there.
// The Lua script thread calls into it via runOnUI(), which posts a
// Qt::BlockingQueuedConnection call and blocks the calling (script) thread
// until it completes -- the standard Qt pattern for a worker thread that
// needs to synchronously drive UI-thread objects.
//
// Handles are plain incrementing ints (matching how BizHawk's forms.* API
// treats them), mapped internally to the actual QWidget.
class LuaFormsManager : public QObject
{
    Q_OBJECT

public:
    LuaFormsManager();
    ~LuaFormsManager() override;

    bool eventFilter(QObject* watched, QEvent* event) override;

    template<typename F>
    auto runOnUI(F&& func) -> decltype(func())
    {
        using Ret = decltype(func());
        if (QThread::currentThread() == this->thread())
            return func(); // already on the UI thread (e.g. during shutdown)

        Ret result{};
        QMetaObject::invokeMethod(this, [&]() { result = func(); }, Qt::BlockingQueuedConnection);
        return result;
    }

    // Runs on the UI thread already; only call from there (or via runOnUI).
    int newForm(int width, int height, const QString& title, int onCloseRef);
    int addButton(int formHandle, const QString& caption, int onClickRef, int x, int y, int w, int h);
    int addLabel(int formHandle, const QString& caption, int x, int y, int w, int h);
    int addCheckbox(int formHandle, const QString& caption, int x, int y);
    int addTextbox(int formHandle, const QString& caption, int w, int h, int x, int y, bool multiline);
    int addDropdown(int formHandle, const QStringList& items, int x, int y, int w, int h);
    int addPictureBox(int formHandle, int x, int y, int w, int h);

    void setProperty(int handle, const QString& prop, const QString& value);
    void setLocation(int handle, int x, int y);
    void setText(int handle, const QString& text);
    QString getText(int handle);
    bool isChecked(int handle);
    void setDropdownItems(int handle, const QStringList& items);
    // Used both by button/checkbox creation (their onclick/addclick params)
    // and by forms.addclick() called later on an existing control. Wires
    // the actual Qt signal the first time a handle gets a callback; safe to
    // call again later to just replace which Lua function fires.
    void setClickCallback(int handle, int ref);
    void destroyHandle(int handle);
    void destroyAll();
    int getMouseX(int handle);
    int getMouseY(int handle);
    QString openFile(const QString& initialDir, const QString& filter, const QString& title);
    void refresh(int handle);
    void addDrawCommand(int handle, const LuaDrawCommand& cmd);
    // Clears a pictureBox's prior draw commands and fills it with color
    // (matches forms.clear's semantics: wipe the canvas to a flat color).
    void clearPictureBox(int handle, const QColor& color);
    // Called from LuaPictureBox::paintEvent, always on the UI thread (same
    // as addDrawCommand, via runOnUI), so no locking needed between them.
    std::vector<LuaDrawCommand> drawCommandsFor(int handle) const;

    // Thread-safe: pending click-handler Lua registry refs, drained by the
    // script thread once per frameadvance() (Lua state must only ever be
    // touched from the script thread, never directly from a Qt signal
    // handler running on the UI thread).
    std::vector<int> takePendingCallbacks();

private:
    int nextHandle = 1;

    QMap<int, QWidget*> widgets;
    QSet<int> topLevelForms;
    QMap<int, int> clickCallbackRefs; // handle -> luaL_ref in LUA_REGISTRYINDEX
    QSet<int> connectedClickHandles;
    QMap<int, int> closeCallbackRefs;
    QMap<int, std::vector<LuaDrawCommand>> pictureBoxCommands;

    QMutex pendingMutex;
    std::vector<int> pendingCallbacks;

    void onControlClicked(int handle);
};

#endif // LUAFORMSMANAGER_H
