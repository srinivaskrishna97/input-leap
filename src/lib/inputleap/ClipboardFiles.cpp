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

#include "inputleap/ClipboardFiles.h"

#include "base/Log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace inputleap {

namespace {

const char kMagic[4] = {'I', 'L', 'F', 'L'};
const std::uint32_t kVersion = 1;           // with contents
const std::uint32_t kListingVersion = 2;    // without contents
const std::size_t kHeaderSize = 4 + 4 + 4;
const std::size_t kEntryFixedSize = 1 + 4 + 8 + 8;
const std::size_t kMaxPathLength = 4096;
const std::size_t kMaxComponentLength = 255;

void put_u32(std::string& out, std::uint32_t value)
{
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

void put_u64(std::string& out, std::uint64_t value)
{
    put_u32(out, static_cast<std::uint32_t>(value >> 32));
    put_u32(out, static_cast<std::uint32_t>(value & 0xffffffffu));
}

std::uint32_t get_u32(const char* p)
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(p[3]));
}

std::uint64_t get_u64(const char* p)
{
    return (static_cast<std::uint64_t>(get_u32(p)) << 32) | get_u32(p + 4);
}

// Names that Windows reserves for devices, with or without an extension
bool is_reserved_name(const std::string& component)
{
    std::string base = component.substr(0, component.find('.'));
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") {
        return true;
    }
    if (base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) &&
        base[3] >= '1' && base[3] <= '9') {
        return true;
    }
    return false;
}

bool is_safe_component(const std::string& component)
{
    if (component.empty() || component == "." || component == ".." ||
        component.size() > kMaxComponentLength) {
        return false;
    }
    for (unsigned char c : component) {
        if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    // Windows silently strips these, which could make two entries collide
    if (component.back() == ' ' || component.back() == '.') {
        return false;
    }
    return !is_reserved_name(component);
}

} // namespace

bool ClipboardFileList::is_safe_path(const std::string& path)
{
    if (path.empty() || path.size() > kMaxPathLength || path.front() == '/' ||
        path.back() == '/') {
        return false;
    }
    std::size_t start = 0;
    while (start < path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string::npos) {
            end = path.size();
        }
        if (!is_safe_component(path.substr(start, end - start))) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

bool ClipboardFileList::parse(const std::string& data)
{
    entries_.clear();
    file_count_ = 0;
    total_size_ = 0;
    data_offset_ = 0;

    if (data.size() < kHeaderSize || std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) {
        return false;
    }
    const char* p = data.data() + sizeof(kMagic);
    const char* end = data.data() + data.size();
    std::uint32_t version = get_u32(p);
    if (version != kVersion && version != kListingVersion) {
        return false;
    }
    p += 4;
    std::uint32_t count = get_u32(p);
    p += 4;
    if (count > kClipboardFilesMaxEntries) {
        return false;
    }

    std::vector<ClipboardFileEntry> entries;
    std::set<std::string> paths;
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (static_cast<std::size_t>(end - p) < kEntryFixedSize) {
            return false;
        }
        unsigned char kind = static_cast<unsigned char>(*p++);
        if (kind > 1) {
            return false;
        }
        std::uint32_t length = get_u32(p);
        p += 4;
        if (length > kMaxPathLength || static_cast<std::size_t>(end - p) < length + 16) {
            return false;
        }
        ClipboardFileEntry entry;
        entry.path.assign(p, length);
        p += length;
        entry.is_directory = kind == 1;
        entry.size = get_u64(p);
        p += 8;
        entry.offset = get_u64(p);
        p += 8;
        if (!is_safe_path(entry.path) || !paths.insert(entry.path).second) {
            return false;
        }
        entries.push_back(std::move(entry));
    }

    std::size_t data_offset = static_cast<std::size_t>(p - data.data());
    std::uint64_t data_size = data.size() - data_offset;
    bool has_contents = version == kVersion;
    std::size_t file_count = 0;
    std::uint64_t total_size = 0;
    for (const auto& entry : entries) {
        if (entry.is_directory) {
            continue;
        }
        if (has_contents && (entry.offset > data_size || entry.size > data_size - entry.offset)) {
            return false;
        }
        if (!has_contents && entry.offset != 0) {
            return false;
        }
        if (entry.size > total_size + entry.size) {
            return false;
        }
        ++file_count;
        total_size += entry.size;
    }
    if (!has_contents && data_size != 0) {
        return false;
    }

    entries_ = std::move(entries);
    file_count_ = file_count;
    total_size_ = total_size;
    data_offset_ = data_offset;
    has_contents_ = has_contents;
    return true;
}

bool ClipboardFileList::contents(const std::string& data, const ClipboardFileEntry& entry,
                                 const char*& begin, std::size_t& size) const
{
    if (entry.is_directory || !has_contents_ || entry.offset > data.size() - data_offset_ ||
        entry.size > data.size() - data_offset_ - entry.offset) {
        return false;
    }
    begin = data.data() + data_offset_ + static_cast<std::size_t>(entry.offset);
    size = static_cast<std::size_t>(entry.size);
    return true;
}

bool ClipboardFileList::Builder::add(ClipboardFileEntry entry)
{
    if (entries_.size() >= kClipboardFilesMaxEntries || !is_safe_path(entry.path) ||
        !paths_.insert(entry.path).second) {
        return false;
    }
    entries_.push_back(std::move(entry));
    return true;
}

bool ClipboardFileList::Builder::add_directory(const std::string& path)
{
    ClipboardFileEntry entry;
    entry.path = path;
    entry.is_directory = true;
    return add(std::move(entry));
}

bool ClipboardFileList::Builder::add_file(const std::string& path, const char* contents,
                                          std::size_t size)
{
    ClipboardFileEntry entry;
    entry.path = path;
    entry.size = size;
    entry.offset = contents_.size();
    if (!add(std::move(entry))) {
        return false;
    }
    contents_.append(contents, size);
    return true;
}

bool ClipboardFileList::Builder::add_file_size(const std::string& path, std::uint64_t size)
{
    ClipboardFileEntry entry;
    entry.path = path;
    entry.is_directory = false;
    entry.size = size;
    entry.offset = 0;
    return add(std::move(entry));
}

std::string ClipboardFileList::Builder::finish_listing()
{
    // a list with the sizes only: the contents, if any were added, are dropped
    for (auto& entry : entries_) {
        entry.offset = 0;
    }
    contents_.clear();
    std::string out;
    out.reserve(kHeaderSize + entries_.size() * (kEntryFixedSize + 32));
    out.append(kMagic, sizeof(kMagic));
    put_u32(out, kListingVersion);
    put_u32(out, static_cast<std::uint32_t>(entries_.size()));
    for (const auto& entry : entries_) {
        out.push_back(entry.is_directory ? 1 : 0);
        put_u32(out, static_cast<std::uint32_t>(entry.path.size()));
        out += entry.path;
        put_u64(out, entry.size);
        put_u64(out, entry.offset);
    }
    entries_.clear();
    paths_.clear();
    return out;
}

std::string ClipboardFileList::Builder::finish()
{
    std::string out;
    out.reserve(kHeaderSize + entries_.size() * (kEntryFixedSize + 32) + contents_.size());
    out.append(kMagic, sizeof(kMagic));
    put_u32(out, kVersion);
    put_u32(out, static_cast<std::uint32_t>(entries_.size()));
    for (const auto& entry : entries_) {
        out.push_back(entry.is_directory ? 1 : 0);
        put_u32(out, static_cast<std::uint32_t>(entry.path.size()));
        out += entry.path;
        put_u64(out, entry.size);
        put_u64(out, entry.offset);
    }
    out += contents_;

    entries_.clear();
    paths_.clear();
    contents_.clear();
    return out;
}

std::string describe_clipboard_files(const ClipboardFileList& list)
{
    std::size_t files = 0;
    std::size_t folders = 0;
    const ClipboardFileEntry* single = nullptr;
    for (const auto& entry : list.entries()) {
        if (entry.path.find('/') != std::string::npos) {
            continue; // not top-level
        }
        single = &entry;
        if (entry.is_directory) {
            ++folders;
        } else {
            ++files;
        }
    }
    if (files + folders == 1) {
        return single->path;
    }
    std::string text;
    if (files > 0) {
        text = std::to_string(files) + (files == 1 ? " file" : " files");
    }
    if (folders > 0) {
        if (!text.empty()) {
            text += " and ";
        }
        text += std::to_string(folders) + (folders == 1 ? " folder" : " folders");
    }
    return text.empty() ? "no files" : text;
}

namespace {

// Walks the files and directories, calling add_directory(relative path) and add_file(file,
// relative path, size) for each; stops at the first false. The directory entries come before
// their contents.
template <class AddDirectory, class AddFile>
bool walk_clipboard_paths(const std::vector<fs::path>& paths, AddDirectory add_directory,
                          AddFile add_file)
{
    auto file_size_of = [](const fs::path& file, std::uint64_t& size) {
        std::error_code ec;
        size = fs::file_size(file, ec);
        if (ec) {
            LOG_WARN("cannot read \"%s\" for the clipboard: %s", file.u8string().c_str(),
                     ec.message().c_str());
            return false;
        }
        return true;
    };

    for (const auto& path : paths) {
        std::error_code ec;
        fs::file_status status = fs::symlink_status(path, ec);
        if (ec) {
            LOG_WARN("cannot read \"%s\" for the clipboard: %s", path.u8string().c_str(),
                     ec.message().c_str());
            return false;
        }
        std::string name = path.filename().u8string();
        if (name.empty() || fs::is_symlink(status)) {
            LOG_WARN("cannot put \"%s\" on the clipboard", path.u8string().c_str());
            return false;
        }

        if (fs::is_directory(status)) {
            if (!add_directory(name)) {
                LOG_WARN("cannot put \"%s\" on the clipboard", name.c_str());
                return false;
            }
            fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end;
            for (; !ec && it != end; it.increment(ec)) {
                const fs::path& child = it->path();
                std::string relative = name + "/" + fs::relative(child, path, ec).generic_u8string();
                if (ec) {
                    break;
                }
                if (it->is_symlink(ec)) {
                    it.disable_recursion_pending();
                    continue;
                }
                bool ok;
                if (it->is_directory(ec)) {
                    ok = add_directory(relative);
                } else if (it->is_regular_file(ec)) {
                    std::uint64_t size = 0;
                    ok = file_size_of(child, size) && add_file(child, relative, size);
                } else {
                    continue;
                }
                if (!ok) {
                    return false;
                }
            }
            if (ec) {
                LOG_WARN("cannot read \"%s\" for the clipboard: %s", path.u8string().c_str(),
                         ec.message().c_str());
                return false;
            }
        }
        else if (fs::is_regular_file(status)) {
            std::uint64_t size = 0;
            if (!file_size_of(path, size) || !add_file(path, name, size)) {
                return false;
            }
        }
        else {
            LOG_WARN("cannot put \"%s\" on the clipboard: not a file or directory",
                     path.u8string().c_str());
            return false;
        }
    }
    return true;
}

} // namespace

bool read_clipboard_files(const std::vector<fs::path>& paths, std::uint64_t max_bytes,
                          std::string& out)
{
    ClipboardFileList::Builder builder;
    std::uint64_t total = 0;

    auto add_directory = [&](const std::string& relative) {
        return builder.add_directory(relative);
    };
    auto add_file = [&](const fs::path& file, const std::string& relative, std::uint64_t size) {
        if (size > max_bytes - total) {
            LOG_NOTE("not putting files on the clipboard: they exceed %llu bytes",
                     static_cast<unsigned long long>(max_bytes));
            return false;
        }
        std::string contents(static_cast<std::size_t>(size), '\0');
        std::ifstream stream;
        open_utf8_path(stream, file, std::ios::in | std::ios::binary);
        if (!stream || (size > 0 && !stream.read(&contents[0], static_cast<std::streamsize>(size)))) {
            LOG_WARN("cannot read \"%s\" for the clipboard", file.u8string().c_str());
            return false;
        }
        if (!builder.add_file(relative, contents)) {
            LOG_WARN("cannot put \"%s\" on the clipboard", relative.c_str());
            return false;
        }
        total += size;
        return true;
    };

    if (!walk_clipboard_paths(paths, add_directory, add_file)) {
        return false;
    }
    out = builder.finish();
    return true;
}

bool read_clipboard_file_listing(const std::vector<fs::path>& paths, std::string& out,
                                 std::vector<fs::path>& file_paths)
{
    ClipboardFileList::Builder builder;
    std::vector<fs::path> entry_paths;

    auto add_directory = [&](const std::string& relative) {
        if (!builder.add_directory(relative)) {
            return false;
        }
        entry_paths.emplace_back();
        return true;
    };
    auto add_file = [&](const fs::path& file, const std::string& relative, std::uint64_t size) {
        if (!builder.add_file_size(relative, size)) {
            LOG_WARN("cannot put \"%s\" on the clipboard", relative.c_str());
            return false;
        }
        entry_paths.push_back(file);
        return true;
    };

    if (!walk_clipboard_paths(paths, add_directory, add_file)) {
        return false;
    }
    out = builder.finish_listing();
    file_paths = std::move(entry_paths);
    return true;
}

std::vector<fs::path> write_clipboard_files(const std::string& data, const fs::path& directory)
{
    ClipboardFileList list;
    if (!list.parse(data)) {
        LOG_WARN("the files on the clipboard are malformed");
        return {};
    }
    if (!list.has_contents()) {
        LOG_WARN("the files on the clipboard come without their contents");
        return {};
    }

    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        LOG_WARN("cannot create \"%s\" for the files on the clipboard: %s",
                 directory.u8string().c_str(), ec.message().c_str());
        return {};
    }

    std::vector<fs::path> top_level;
    bool ok = true;
    for (const auto& entry : list.entries()) {
        fs::path target = directory / fs::u8path(entry.path);
        if (entry.path.find('/') == std::string::npos) {
            top_level.push_back(target);
        }
        if (entry.is_directory) {
            fs::create_directories(target, ec);
            if (ec) {
                ok = false;
                break;
            }
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            ok = false;
            break;
        }
        const char* begin = nullptr;
        std::size_t size = 0;
        if (!list.contents(data, entry, begin, size)) {
            ok = false;
            break;
        }
        std::ofstream stream;
        open_utf8_path(stream, target, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!stream) {
            ok = false;
            break;
        }
        if (size > 0) {
            stream.write(begin, static_cast<std::streamsize>(size));
        }
        stream.close();
        if (!stream) {
            ok = false;
            break;
        }
    }

    if (!ok) {
        LOG_WARN("cannot write the files on the clipboard to \"%s\"", directory.u8string().c_str());
        fs::remove_all(directory, ec);
        return {};
    }
    LOG_DEBUG("wrote %zu files on the clipboard to \"%s\"", list.file_count(),
              directory.u8string().c_str());
    return top_level;
}

} // namespace inputleap
