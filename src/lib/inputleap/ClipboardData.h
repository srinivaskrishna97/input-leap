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

#include <cstdint>
#include <memory>
#include <string>

namespace inputleap {

class IClipboard;

/*! Clipboard contents in the marshalled wire format (see IClipboard::marshall()).

Clipboard contents can be large (a screenshot of a few monitors is tens of megabytes), so the
data is immutable and shared through MarshalledClipboardPtr. This allows passing it between
the screens, the server, the connections and worker threads without copying it.
*/
class MarshalledClipboard {
public:
    explicit MarshalledClipboard(std::string data);

    const std::string& data() const { return data_; }
    std::size_t size() const { return data_.size(); }

    /// A 64-bit fingerprint of the data. Equal contents always have equal hashes. The hash is
    /// not cryptographic, so equal hashes only mean that the contents are equal with very high
    /// probability.
    std::uint64_t hash() const { return hash_; }

    /// Returns true if \p other contains the same data.
    bool same_contents(const MarshalledClipboard& other) const;

    /// The hash function used for hash(). It produces the same result on all platforms.
    static std::uint64_t compute_hash(const char* data, std::size_t size);

private:
    std::string data_;
    std::uint64_t hash_ = 0;
};

using MarshalledClipboardPtr = std::shared_ptr<const MarshalledClipboard>;

/// Wraps already marshalled clipboard data.
MarshalledClipboardPtr make_marshalled_clipboard(std::string data);

/// Marshalls the contents of \p clipboard.
MarshalledClipboardPtr marshall_clipboard(const IClipboard& clipboard);

/// Returns the marshalled form of a clipboard without any data.
const MarshalledClipboardPtr& empty_marshalled_clipboard();

/// Returns true if both contain the same clipboard data. Null pointers are equal only to each
/// other.
bool same_clipboard_contents(const MarshalledClipboardPtr& a, const MarshalledClipboardPtr& b);

} // namespace inputleap
