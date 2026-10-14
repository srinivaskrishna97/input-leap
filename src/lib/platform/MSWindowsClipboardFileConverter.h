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

#include "platform/MSWindowsClipboard.h"
#include "io/filesystem.h"

#include <string>
#include <vector>

namespace inputleap {

/*! Converts between IClipboard::kFiles and CF_HDROP, the list of files that Explorer puts on
    the clipboard.

    The files are listed without their contents (see ClipboardFileList); the converter remembers
    where the listed files are, so that their contents can be sent when they are pasted on
    another screen. Files received from another screen are written below the profile directory
    (by the clipboard worker, which fetches them when they are pasted) and the list on the
    clipboard refers to those copies.
*/
class MSWindowsClipboardFileConverter : public IMSWindowsClipboardConverter {
public:
    IClipboard::EFormat getFormat() const override;
    UINT getWin32Format() const override;
    HANDLE fromIClipboard(const std::string&) const override;
    std::string toIClipboard(HANDLE) const override;

    /// A new directory for files received from another screen; older sets are removed.
    static fs::path new_received_directory();

    /*! Returns the paths, on this computer, of the files that toIClipboard() listed in
        \p listing (IClipboard::kFiles data without contents); false if the list is not known. */
    static bool take_listed_paths(const std::string& listing, std::vector<fs::path>& paths);

    /*! Tells fromIClipboard() that the files listed in \p listing were received into
        \p directory, so that a list without contents can be put on the clipboard. */
    static void register_received_files(const std::string& listing, const fs::path& directory);
};

} // namespace inputleap
