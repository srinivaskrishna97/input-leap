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

#include "inputleap/clipboard_types.h"

#include <cstdint>
#include <memory>
#include <mutex>
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

    /// Creates the contents with an already known run-length encoded form (see rle32()).
    MarshalledClipboard(std::string data, std::shared_ptr<const std::string> rle32);

    MarshalledClipboard(const MarshalledClipboard&) = delete;
    MarshalledClipboard& operator=(const MarshalledClipboard&) = delete;

    const std::string& data() const { return data_; }
    std::size_t size() const { return data_.size(); }

    /// A 64-bit fingerprint of the data. Equal contents always have equal hashes. The hash is
    /// not cryptographic, so equal hashes only mean that the contents are equal with very high
    /// probability.
    std::uint64_t hash() const { return hash_; }

    /// Returns true if \p other contains the same data.
    bool same_contents(const MarshalledClipboard& other) const;

    /// The data encoded with rle32_encode(). Computed on first use and cached. Safe to call
    /// from several threads.
    std::shared_ptr<const std::string> rle32() const;

    /// The hash function used for hash(). It produces the same result on all platforms.
    static std::uint64_t compute_hash(const char* data, std::size_t size);

private:
    std::string data_;
    std::uint64_t hash_ = 0;

    mutable std::once_flag rle32_once_;
    mutable std::shared_ptr<const std::string> rle32_;
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

/// Returns the formats present in marshalled clipboard data as a bit mask with bit
/// (1 << IClipboard::EFormat) set for each format. Malformed data yields 0.
std::uint32_t marshalled_clipboard_formats(const std::string& data);

/*! Describes clipboard contents without the contents themselves. Used to announce the contents
    of a clipboard so that they are transferred only when needed.
*/
struct ClipboardOffer {
    /// bit (1 << IClipboard::EFormat) set for each format present
    std::uint32_t formats = 0;
    /// size of the marshalled contents
    std::uint32_t size = 0;
    /// MarshalledClipboard::hash() of the marshalled contents
    std::uint64_t hash = 0;

    /// Returns true if \p data are the described contents.
    bool describes(const MarshalledClipboard& data) const
    {
        return data.size() == size && data.hash() == hash;
    }

    bool operator==(const ClipboardOffer& other) const
    {
        return formats == other.formats && size == other.size && hash == other.hash;
    }
    bool operator!=(const ClipboardOffer& other) const { return !(*this == other); }

    static ClipboardOffer describe(const MarshalledClipboard& data);
};

/// Data of the CLIPBOARD_OFFERED and CLIPBOARD_DATA_REQUESTED events.
struct ClipboardOfferInfo {
    ClipboardID id = 0;
    std::uint32_t sequence = 0;
    /// kClipboardOfferDataFollows if the contents are sent right after the offer
    std::uint32_t flags = 0;
    ClipboardOffer offer;
};

} // namespace inputleap
