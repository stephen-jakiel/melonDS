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

#include "LuaFormsManager.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCursor>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMutexLocker>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QWidget>

// A plain (non-QObject) QWidget subclass: it only needs to override
// paintEvent, so it doesn't need Qt's meta-object system/MOC at all. Draws
// whatever forms.drawText/drawRectangle/etc. queued for its handle, reusing
// the same LuaDrawCommand type the gui.* overlay uses.
class LuaPictureBox : public QWidget
{
public:
    LuaPictureBox(LuaFormsManager* owner, int handle, QWidget* parent)
        : QWidget(parent), owner(owner), handle(handle)
    {}

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);

        for (const auto& cmd : owner->drawCommandsFor(handle))
        {
            switch (cmd.kind)
            {
            case LuaDrawCommand::Text:
                painter.setPen(cmd.color);
                painter.drawText(QRectF(cmd.x1, cmd.y1, 1000, 20), Qt::AlignLeft | Qt::AlignTop, cmd.text);
                break;
            case LuaDrawCommand::Rect:
                painter.setPen(cmd.color);
                painter.setBrush(cmd.fillColor.alpha() > 0 ? QBrush(cmd.fillColor) : Qt::NoBrush);
                painter.drawRect(QRectF(cmd.x1, cmd.y1, cmd.x2, cmd.y2));
                break;
            case LuaDrawCommand::Line:
                painter.setPen(cmd.color);
                painter.drawLine(QPointF(cmd.x1, cmd.y1), QPointF(cmd.x2, cmd.y2));
                break;
            case LuaDrawCommand::Pixel:
                painter.setPen(cmd.color);
                painter.drawPoint(QPointF(cmd.x1, cmd.y1));
                break;
            }
        }
    }

private:
    LuaFormsManager* owner;
    int handle;
};

LuaFormsManager::LuaFormsManager()
{
}

LuaFormsManager::~LuaFormsManager()
{
    destroyAll();
}

bool LuaFormsManager::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Close || event->type() == QEvent::MouseButtonPress)
    {
        for (auto it = widgets.constBegin(); it != widgets.constEnd(); ++it)
        {
            if (it.value() != watched)
                continue;

            if (event->type() == QEvent::Close && closeCallbackRefs.contains(it.key()))
            {
                QMutexLocker locker(&pendingMutex);
                pendingCallbacks.push_back(closeCallbackRefs[it.key()]);
            }
            else if (event->type() == QEvent::MouseButtonPress)
            {
                onControlClicked(it.key());
            }
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void LuaFormsManager::setClickCallback(int handle, int ref)
{
    clickCallbackRefs[handle] = ref;
    if (connectedClickHandles.contains(handle))
        return;
    connectedClickHandles.insert(handle);

    QWidget* w = widgets.value(handle);
    if (!w) return;

    if (auto* btn = qobject_cast<QPushButton*>(w))
        connect(btn, &QPushButton::clicked, this, [this, handle]() { onControlClicked(handle); });
    else if (auto* cb = qobject_cast<QCheckBox*>(w))
        connect(cb, &QCheckBox::clicked, this, [this, handle]() { onControlClicked(handle); });
    else
        w->installEventFilter(this); // generic click via eventFilter's MouseButtonPress handling
}

void LuaFormsManager::onControlClicked(int handle)
{
    if (!clickCallbackRefs.contains(handle))
        return;
    QMutexLocker locker(&pendingMutex);
    pendingCallbacks.push_back(clickCallbackRefs[handle]);
}

std::vector<int> LuaFormsManager::takePendingCallbacks()
{
    QMutexLocker locker(&pendingMutex);
    std::vector<int> result = std::move(pendingCallbacks);
    pendingCallbacks.clear();
    return result;
}

int LuaFormsManager::newForm(int width, int height, const QString& title, int onCloseRef)
{
    auto* w = new QWidget(nullptr);
    w->setWindowTitle(title);
    w->resize(width, height);
    w->setAttribute(Qt::WA_DeleteOnClose, false); // we manage lifetime via destroy()/destroyAll()
    w->installEventFilter(this);
    w->show();

    int handle = nextHandle++;
    widgets[handle] = w;
    topLevelForms.insert(handle);
    if (onCloseRef != 0)
        closeCallbackRefs[handle] = onCloseRef;
    return handle;
}

int LuaFormsManager::addButton(int formHandle, const QString& caption, int onClickRef, int x, int y, int w, int h)
{
    QWidget* form = widgets.value(formHandle);
    if (!form) return 0;

    auto* btn = new QPushButton(caption, form);
    if (w > 0 && h > 0) btn->setGeometry(x, y, w, h);
    else btn->move(x, y);
    btn->show();

    int handle = nextHandle++;
    widgets[handle] = btn;
    if (onClickRef != 0)
        setClickCallback(handle, onClickRef);
    return handle;
}

int LuaFormsManager::addLabel(int formHandle, const QString& caption, int x, int y, int w, int h)
{
    QWidget* form = widgets.value(formHandle);
    if (!form) return 0;

    auto* lbl = new QLabel(caption, form);
    if (w > 0 && h > 0) lbl->setGeometry(x, y, w, h);
    else lbl->move(x, y);
    lbl->show();

    int handle = nextHandle++;
    widgets[handle] = lbl;
    return handle;
}

int LuaFormsManager::addCheckbox(int formHandle, const QString& caption, int x, int y)
{
    QWidget* form = widgets.value(formHandle);
    if (!form) return 0;

    auto* cb = new QCheckBox(caption, form);
    cb->move(x, y);
    cb->show();

    int handle = nextHandle++;
    widgets[handle] = cb;
    return handle;
}

int LuaFormsManager::addTextbox(int formHandle, const QString& caption, int w, int h, int x, int y, bool multiline)
{
    QWidget* form = widgets.value(formHandle);
    if (!form) return 0;

    QWidget* box;
    if (multiline)
    {
        auto* edit = new QPlainTextEdit(caption, form);
        box = edit;
    }
    else
    {
        auto* edit = new QLineEdit(caption, form);
        box = edit;
    }
    if (w > 0 && h > 0) box->setGeometry(x, y, w, h);
    else box->move(x, y);
    box->show();

    int handle = nextHandle++;
    widgets[handle] = box;
    return handle;
}

int LuaFormsManager::addDropdown(int formHandle, const QStringList& items, int x, int y, int w, int h)
{
    QWidget* form = widgets.value(formHandle);
    if (!form) return 0;

    auto* combo = new QComboBox(form);
    combo->addItems(items);
    if (w > 0 && h > 0) combo->setGeometry(x, y, w, h);
    else combo->move(x, y);
    combo->show();

    int handle = nextHandle++;
    widgets[handle] = combo;
    return handle;
}

int LuaFormsManager::addPictureBox(int formHandle, int x, int y, int w, int h)
{
    QWidget* form = widgets.value(formHandle);
    if (!form) return 0;

    int handle = nextHandle++;
    auto* box = new LuaPictureBox(this, handle, form);
    box->setGeometry(x, y, w, h);
    box->show();

    widgets[handle] = box;
    return handle;
}

void LuaFormsManager::setProperty(int handle, const QString& prop, const QString& value)
{
    QWidget* w = widgets.value(handle);
    if (!w) return;

    bool boolVal = (value == "true" || value == "1");

    if (prop == "Visible")
        w->setVisible(boolVal);
    else if (prop == "Enabled")
        w->setEnabled(boolVal);
    else if (prop == "AutoSize")
    {
        if (boolVal) w->adjustSize();
    }
    else if (prop == "Checked")
    {
        if (auto* cb = qobject_cast<QCheckBox*>(w))
            cb->setChecked(boolVal);
    }
    else if (prop == "TabStop")
        w->setFocusPolicy(boolVal ? Qt::StrongFocus : Qt::NoFocus);
    else if (prop == "ListItems")
    {
        if (auto* combo = qobject_cast<QComboBox*>(w))
        {
            combo->clear();
            combo->addItems(value.split('\n', Qt::SkipEmptyParts));
        }
    }
    // AutoCompleteSource/AutoCompleteMode/Append/Broadcaster and anything
    // else unrecognized: silently ignored. These are edge-case WinForms
    // properties (textbox autocomplete, a tracker-specific network flag)
    // not worth modeling for a first pass.
}

void LuaFormsManager::setLocation(int handle, int x, int y)
{
    if (QWidget* w = widgets.value(handle))
        w->move(x, y);
}

void LuaFormsManager::setText(int handle, const QString& text)
{
    QWidget* w = widgets.value(handle);
    if (!w) return;

    if (auto* edit = qobject_cast<QLineEdit*>(w)) edit->setText(text);
    else if (auto* edit = qobject_cast<QPlainTextEdit*>(w)) edit->setPlainText(text);
    else if (auto* lbl = qobject_cast<QLabel*>(w)) lbl->setText(text);
    else if (auto* btn = qobject_cast<QPushButton*>(w)) btn->setText(text);
    else if (auto* cb = qobject_cast<QCheckBox*>(w)) cb->setText(text);
    else w->setWindowTitle(text); // top-level form
}

QString LuaFormsManager::getText(int handle)
{
    QWidget* w = widgets.value(handle);
    if (!w) return QString();

    if (auto* edit = qobject_cast<QLineEdit*>(w)) return edit->text();
    if (auto* edit = qobject_cast<QPlainTextEdit*>(w)) return edit->toPlainText();
    if (auto* lbl = qobject_cast<QLabel*>(w)) return lbl->text();
    if (auto* btn = qobject_cast<QPushButton*>(w)) return btn->text();
    if (auto* cb = qobject_cast<QCheckBox*>(w)) return cb->text();
    if (auto* combo = qobject_cast<QComboBox*>(w)) return combo->currentText();
    return w->windowTitle();
}

bool LuaFormsManager::isChecked(int handle)
{
    if (auto* cb = qobject_cast<QCheckBox*>(widgets.value(handle)))
        return cb->isChecked();
    return false;
}

void LuaFormsManager::setDropdownItems(int handle, const QStringList& items)
{
    if (auto* combo = qobject_cast<QComboBox*>(widgets.value(handle)))
    {
        combo->clear();
        combo->addItems(items);
    }
}

void LuaFormsManager::destroyHandle(int handle)
{
    QWidget* w = widgets.value(handle);
    if (!w) return;

    widgets.remove(handle);
    topLevelForms.remove(handle);
    clickCallbackRefs.remove(handle);
    closeCallbackRefs.remove(handle);
    pictureBoxCommands.remove(handle);
    w->deleteLater();
}

void LuaFormsManager::destroyAll()
{
    // Destroying each top-level form cascades to its child controls via Qt's
    // normal parent/child ownership, so we don't need to walk those
    // separately.
    for (int handle : topLevelForms)
    {
        if (QWidget* w = widgets.value(handle))
            w->deleteLater();
    }
    widgets.clear();
    topLevelForms.clear();
    clickCallbackRefs.clear();
    closeCallbackRefs.clear();
    pictureBoxCommands.clear();
}

int LuaFormsManager::getMouseX(int handle)
{
    QWidget* w = widgets.value(handle);
    if (!w) return 0;
    return w->mapFromGlobal(QCursor::pos()).x();
}

int LuaFormsManager::getMouseY(int handle)
{
    QWidget* w = widgets.value(handle);
    if (!w) return 0;
    return w->mapFromGlobal(QCursor::pos()).y();
}

QString LuaFormsManager::openFile(const QString& initialDir, const QString& filter, const QString& title)
{
    return QFileDialog::getOpenFileName(nullptr, title.isEmpty() ? "Open File" : title,
                                         initialDir, filter.isEmpty() ? "All files (*.*)" : filter);
}

void LuaFormsManager::refresh(int handle)
{
    if (QWidget* w = widgets.value(handle))
        w->update();
}

void LuaFormsManager::addDrawCommand(int handle, const LuaDrawCommand& cmd)
{
    pictureBoxCommands[handle].push_back(cmd);
}

std::vector<LuaDrawCommand> LuaFormsManager::drawCommandsFor(int handle) const
{
    return pictureBoxCommands.value(handle);
}
