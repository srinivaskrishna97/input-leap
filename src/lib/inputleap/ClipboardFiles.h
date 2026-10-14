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

#include "io/filesystem.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace inputleap {

/// At most this many files and directories are put on the clipboard.
constexpr std::size_t kClipboardFilesMaxEntries = 10000;

/// Files that together are larger than this are not put on the clipboard.
constexpr std::uint64_t kClipboardFilesMaxBytes = 256ull * 1024 * 1024;

/*! An entry of a file list on the clipboard: a file or a directory. Paths are relative to the
    list, use '/' as separator and cannot leave the list, see ClipboardFileList::is_safe_path().
*/
struct ClipboardFileEntry {
    std::string path;
    bool is_directory = false;
    std::uint64_t size = 0;    ///< of the contents, 0 for directories
    std::uint64_t offset = 0;  ///< of the contents within the data section
};

/*! The clipboard format IClipboard::kFiles: files and directories together with the contents
    of the files, so that files copied on one screen can be pasted on another.

    Version 2 lists the files without their contents: the sizes are given but the data section
    is empty, and the contents are fetched file by file when they are pasted (see
    kMsgQClipboardFile).

    Layout, with all integers big-endian:
      - "ILFL", u32 version (1 with contents, 2 without), u32 entry count
      - per entry: u8 kind (0 file, 1 directory), u32 path length, the path (UTF-8),
        u64 size, u64 offset
      - the contents of all files, at the given offsets after the entries
*/
class ClipboardFileList {
public:
    /// Parses \p data. Returns false if it is malformed; the list is empty then.
    bool parse(const std::string& data);

    const std::vector<ClipboardFileEntry>& entries() const { return entries_; }
    std::size_t file_count() const { return file_count_; }
    std::size_t directory_count() const { return entries_.size() - file_count_; }
    /// The size of the contents of all files.
    std::uint64_t total_size() const { return total_size_; }
    /// Whether the contents are included (version 1) or only listed (version 2).
    bool has_contents() const { return has_contents_; }

    /*! Locates the contents of \p entry within \p data, the string that was parsed. Returns
        false for directories. */
    bool contents(const std::string& data, const ClipboardFileEntry& entry,
                  const char*& begin, std::size_t& size) const;

    /*! Returns true if \p path is acceptable: relative, with '/' separators, without "." or
        ".." components and without characters that are not allowed in file names. */
    static bool is_safe_path(const std::string& path);

    //! Builds a file list
    class Builder {
    public:
        /// Returns false if the path is not safe, is a duplicate or there are too many entries.
        bool add_directory(const std::string& path);
        bool add_file(const std::string& path, const char* contents, std::size_t size);
        bool add_file(const std::string& path, const std::string& contents)
        {
            return add_file(path, contents.data(), contents.size());
        }

        std::size_t count() const { return entries_.size(); }
        std::uint64_t total_size() const { return contents_.size(); }

        /// Adds a file by its size only, for a list without contents.
        bool add_file_size(const std::string& path, std::uint64_t size);

        /// Returns the marshalled list and empties the builder.
        std::string finish();

        /// Returns the marshalled list without contents (version 2) and empties the builder.
        std::string finish_listing();

    private:
        bool add(ClipboardFileEntry entry);

        std::vector<ClipboardFileEntry> entries_;
        std::set<std::string> paths_;
        std::string contents_;
    };

private:
    std::vector<ClipboardFileEntry> entries_;
    std::size_t file_count_ = 0;
    std::uint64_t total_size_ = 0;
    std::size_t data_offset_ = 0;
    bool has_contents_ = true;
};

/// A short description of the list for messages, e.g. "report.pdf" or "3 files and 1 folder".
std::string describe_clipboard_files(const ClipboardFileList& list);

/*! Reads the files and directories at \p paths (directories recursively) into a marshalled
    file list. The entries are named after the last component of each path. Returns false,
    and logs why, if something cannot be read, if there are too many entries or if the
    contents exceed \p max_bytes. */
bool read_clipboard_files(const std::vector<fs::path>& paths, std::uint64_t max_bytes,
                          std::string& out);

/*! Like read_clipboard_files(), but lists the files with their sizes only (version 2) and
    returns in \p file_paths, for each entry of the list, the path of the file on this
    computer, or an empty path for a directory. */
bool read_clipboard_file_listing(const std::vector<fs::path>& paths, std::string& out,
                                 std::vector<fs::path>& file_paths);

/*! Writes the files of the marshalled list \p data into \p directory, which is created.
    Returns the paths of the top-level entries, or an empty vector if anything failed, in
    which case the directory is removed again. */
std::vector<fs::path> write_clipboard_files(const std::string& data, const fs::path& directory);

} // namespace inputleap
