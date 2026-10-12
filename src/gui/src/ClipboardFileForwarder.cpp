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

#include "ClipboardFileForwarder.h"

#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QMimeData>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

// these must match MSWindowsClipboardWorker in the core
const wchar_t* const kWorkerWindowClass = L"InputLeapClipboardWorker";
const ULONG_PTR kFileListMagic = 0x494C4650; // "ILFP"

// contents that InputLeap itself put on the clipboard came from another computer
bool clipboardOwnedByInputLeap()
{
    static const UINT ownership = RegisterClipboardFormatW(L"InputLeapOwnership");
    return ownership != 0 && IsClipboardFormatAvailable(ownership);
}

} // namespace
#endif

ClipboardFileForwarder::ClipboardFileForwarder(QObject* parent) :
    QObject(parent)
{
#ifdef Q_OS_WIN
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
            this, &ClipboardFileForwarder::onClipboardChanged);
#endif
}

void ClipboardFileForwarder::onClipboardChanged()
{
#ifdef Q_OS_WIN
    if (clipboardOwnedByInputLeap()) {
        return;
    }
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (mime == nullptr || !mime->hasUrls()) {
        return;
    }

    // the paths, each terminated by a null and the list by another one
    std::wstring list;
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        list += QDir::toNativeSeparators(url.toLocalFile()).toStdWString();
        list.push_back(L'\0');
    }
    if (list.empty()) {
        return;
    }
    list.push_back(L'\0');

    // the sequence number lets the core match the paths to the clipboard contents it reads
    DWORD sequence = GetClipboardSequenceNumber();
    std::vector<char> payload(sizeof(DWORD) + list.size() * sizeof(wchar_t));
    std::memcpy(payload.data(), &sequence, sizeof(DWORD));
    std::memcpy(payload.data() + sizeof(DWORD), list.data(), list.size() * sizeof(wchar_t));

    COPYDATASTRUCT data = {};
    data.dwData = kFileListMagic;
    data.cbData = static_cast<DWORD>(payload.size());
    data.lpData = payload.data();

    // the core's clipboard window is a message-only window
    HWND window = nullptr;
    while ((window = FindWindowExW(HWND_MESSAGE, window, kWorkerWindowClass, nullptr)) != nullptr) {
        DWORD_PTR result = 0;
        SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result);
    }
#endif
}
