/*  InputLeap -- mouse and keyboard sharing utility
    Copyright (C) InputLeap contributors

    This package is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    found in the file LICENSE that should have accompanied this file.

    This package is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <QObject>

/*! Tells the InputLeap core which files are on the clipboard.

    On Windows the core usually runs with higher privileges than the applications that copy
    files (the service starts it as SYSTEM), and Windows hides the file list that such
    applications put on the clipboard from more privileged processes. The GUI runs as the
    user and can read the list, so it sends the paths to the core, which reads the files.
    Does nothing on other platforms.
*/
class ClipboardFileForwarder : public QObject {
    Q_OBJECT
public:
    explicit ClipboardFileForwarder(QObject* parent = nullptr);

private slots:
    void onClipboardChanged();
};
