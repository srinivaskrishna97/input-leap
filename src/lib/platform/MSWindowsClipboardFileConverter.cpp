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

#include "platform/MSWindowsClipboardFileConverter.h"

#include "inputleap/ClipboardFiles.h"
#include "common/DataDirectories.h"
#include "base/Log.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace inputleap {

namespace {

// How many sets of received files are kept; older ones are removed when new ones arrive.
const std::size_t kKeptReceivedFileSets = 3;

fs::path received_files_root()
{
    return DataDirectories::profile() / "clipboard";
}

// Returns a new directory for received files, removing the oldest sets.
fs::path new_received_directory()
{
    fs::path root = received_files_root();
    std::error_code ec;
    fs::create_directories(root, ec);

    std::vector<fs::path> sets;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            sets.push_back(it->path());
        }
    }
    // the names are zero-padded timestamps, so sorting them sorts by age
    std::sort(sets.begin(), sets.end());
    while (sets.size() >= kKeptReceivedFileSets) {
        fs::remove_all(sets.front(), ec);
        sets.erase(sets.begin());
    }

    auto now = std::chrono::system_clock::now().time_since_epoch();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    char name[32];
    std::snprintf(name, sizeof(name), "%020lld", ms);
    return root / name;
}

} // namespace

IClipboard::EFormat MSWindowsClipboardFileConverter::getFormat() const
{
    return IClipboard::kFiles;
}

UINT MSWindowsClipboardFileConverter::getWin32Format() const
{
    return CF_HDROP;
}

HANDLE MSWindowsClipboardFileConverter::fromIClipboard(const std::string& data) const
{
    std::vector<fs::path> paths = write_clipboard_files(data, new_received_directory());
    if (paths.empty()) {
        return nullptr;
    }

    // a DROPFILES structure followed by the paths as a double null terminated list
    std::wstring list;
    for (const auto& path : paths) {
        list += path.wstring();
        list.push_back(L'\0');
    }
    list.push_back(L'\0');

    std::size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, bytes);
    if (global == nullptr) {
        return nullptr;
    }
    void* memory = GlobalLock(global);
    if (memory == nullptr) {
        GlobalFree(global);
        return nullptr;
    }
    DROPFILES* drop = static_cast<DROPFILES*>(memory);
    std::memset(drop, 0, sizeof(DROPFILES));
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    std::memcpy(static_cast<char*>(memory) + sizeof(DROPFILES), list.data(),
                list.size() * sizeof(wchar_t));
    GlobalUnlock(global);

    LOG_INFO("put %zu files from another screen on the clipboard", paths.size());
    return global;
}

std::string MSWindowsClipboardFileConverter::toIClipboard(HANDLE data) const
{
    HDROP drop = static_cast<HDROP>(data);
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count == 0) {
        LOG_DEBUG("no files in the drop list on the clipboard (error %lu)", GetLastError());
    }
    std::vector<fs::path> paths;
    for (UINT i = 0; i < count; ++i) {
        UINT length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, i, &path[0], length + 1);
        path.resize(length);
        paths.emplace_back(path);
    }
    if (paths.empty()) {
        return {};
    }

    std::string out;
    if (!read_clipboard_files(paths, kClipboardFilesMaxBytes, out)) {
        return {};
    }
    LOG_INFO("files on clipboard: %zu items, %zu bytes", paths.size(), out.size());
    return out;
}

} // namespace inputleap
