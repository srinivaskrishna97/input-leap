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

namespace inputleap {

/*! Converts between IClipboard::kFiles and CF_HDROP, the list of files that Explorer puts on
    the clipboard. Files received from another screen are written below the profile directory
    and the list on the clipboard refers to those copies.
*/
class MSWindowsClipboardFileConverter : public IMSWindowsClipboardConverter {
public:
    IClipboard::EFormat getFormat() const override;
    UINT getWin32Format() const override;
    HANDLE fromIClipboard(const std::string&) const override;
    std::string toIClipboard(HANDLE) const override;
};

} // namespace inputleap
