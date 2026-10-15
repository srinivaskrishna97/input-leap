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

#include "inputleap/ClipboardData.h"
#include "inputleap/ClipboardFiles.h"
#include "common/DataDirectories.h"
#include "base/Log.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

namespace inputleap {

namespace {

// How many sets of received files are kept; older ones are removed when new ones arrive.
const std::size_t kKeptReceivedFileSets = 3;

fs::path received_files_root()
{
    return DataDirectories::profile() / "clipboard";
}

// What toIClipboard() listed and where fromIClipboard() finds received files, both by the
// hash of the list.  A few are kept, as a list may be needed after the clipboard changed.
const std::size_t kRememberedFileLists = 4;

std::mutex g_file_lists_mutex;
std::map<std::uint64_t, std::vector<fs::path>> g_listed_paths;
std::deque<std::uint64_t> g_listed_order;
std::map<std::uint64_t, fs::path> g_received_directories;
std::deque<std::uint64_t> g_received_order;

std::uint64_t listing_hash(const std::string& listing)
{
    return MarshalledClipboard::compute_hash(listing.data(), listing.size());
}

// Returns a new directory for received files, removing the oldest sets.
fs::path make_received_directory()
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

fs::path MSWindowsClipboardFileConverter::new_received_directory()
{
    return make_received_directory();
}

bool MSWindowsClipboardFileConverter::take_listed_paths(const std::string& listing,
                                                        std::vector<fs::path>& paths)
{
    std::lock_guard<std::mutex> lock(g_file_lists_mutex);
    auto found = g_listed_paths.find(listing_hash(listing));
    if (found == g_listed_paths.end()) {
        return false;
    }
    paths = found->second;
    return true;
}

void MSWindowsClipboardFileConverter::register_received_files(const std::string& listing,
                                                              const fs::path& directory)
{
    std::lock_guard<std::mutex> lock(g_file_lists_mutex);
    std::uint64_t hash = listing_hash(listing);
    g_received_directories[hash] = directory;
    g_received_order.push_back(hash);
    while (g_received_order.size() > kRememberedFileLists) {
        g_received_directories.erase(g_received_order.front());
        g_received_order.pop_front();
    }
}

HANDLE MSWindowsClipboardFileConverter::fromIClipboard(const std::string& data) const
{
    ClipboardFileList list;
    if (!list.parse(data)) {
        LOG_WARN("the files on the clipboard are malformed");
        return nullptr;
    }

    std::vector<fs::path> paths;
    if (list.has_contents()) {
        // the contents came along, as from an older version
        paths = write_clipboard_files(data, make_received_directory());
    }
    else {
        // the clipboard worker fetched the files before this, see MSWindowsClipboardWorker
        fs::path directory;
        {
            std::lock_guard<std::mutex> lock(g_file_lists_mutex);
            auto found = g_received_directories.find(listing_hash(data));
            if (found != g_received_directories.end()) {
                directory = found->second;
            }
        }
        if (directory.empty()) {
            LOG_WARN("the files from another screen have not arrived");
            return nullptr;
        }
        for (const auto& entry : list.entries()) {
            // the top-level entries; the rest is below them
            if (entry.path.find('/') == std::string::npos) {
                paths.push_back(directory / fs::u8path(entry.path));
            }
        }
    }
    if (paths.empty()) {
        return nullptr;
    }

    // a DROPFILES structure followed by the paths as a double null terminated list
    std::wstring names;
    for (const auto& path : paths) {
        names += path.wstring();
        names.push_back(L'\0');
    }
    names.push_back(L'\0');

    std::size_t bytes = sizeof(DROPFILES) + names.size() * sizeof(wchar_t);
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
    std::memcpy(static_cast<char*>(memory) + sizeof(DROPFILES), names.data(),
                names.size() * sizeof(wchar_t));
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

    // the files are listed with their sizes; the contents are sent when they are pasted
    std::string out;
    std::vector<fs::path> file_paths;
    if (!read_clipboard_file_listing(paths, out, file_paths)) {
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(g_file_lists_mutex);
        std::uint64_t hash = listing_hash(out);
        g_listed_paths[hash] = std::move(file_paths);
        g_listed_order.push_back(hash);
        while (g_listed_order.size() > kRememberedFileLists) {
            g_listed_paths.erase(g_listed_order.front());
            g_listed_order.pop_front();
        }
    }
    ClipboardFileList list;
    list.parse(out);
    LOG_INFO("files on clipboard: %s, %llu bytes", describe_clipboard_files(list).c_str(),
             static_cast<unsigned long long>(list.total_size()));
    return out;
}

} // namespace inputleap
