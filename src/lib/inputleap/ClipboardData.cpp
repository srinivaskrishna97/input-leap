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

#include "inputleap/ClipboardData.h"
#include "inputleap/Clipboard.h"
#include "inputleap/ClipboardCodec.h"

namespace inputleap {

namespace clipboard_data_detail {

inline std::uint64_t load_le64(const unsigned char* p)
{
    return static_cast<std::uint64_t>(p[0]) |
           (static_cast<std::uint64_t>(p[1]) << 8) |
           (static_cast<std::uint64_t>(p[2]) << 16) |
           (static_cast<std::uint64_t>(p[3]) << 24) |
           (static_cast<std::uint64_t>(p[4]) << 32) |
           (static_cast<std::uint64_t>(p[5]) << 40) |
           (static_cast<std::uint64_t>(p[6]) << 48) |
           (static_cast<std::uint64_t>(p[7]) << 56);
}

inline std::uint32_t load_be32(const unsigned char* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
            static_cast<std::uint32_t>(p[3]);
}

inline std::uint64_t mix64(std::uint64_t x)
{
    // finalizer of the splitmix64 generator
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

inline std::uint64_t rotl64(std::uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

} // namespace clipboard_data_detail

MarshalledClipboard::MarshalledClipboard(std::string data) :
    data_{std::move(data)},
    hash_{compute_hash(data_.data(), data_.size())}
{
}

MarshalledClipboard::MarshalledClipboard(std::string data,
                                         std::shared_ptr<const std::string> rle32) :
    MarshalledClipboard(std::move(data))
{
    if (rle32) {
        std::call_once(rle32_once_, [this, &rle32]() { rle32_ = std::move(rle32); });
    }
}

bool MarshalledClipboard::same_contents(const MarshalledClipboard& other) const
{
    return this == &other || (hash_ == other.hash_ && data_ == other.data_);
}

std::shared_ptr<const std::string> MarshalledClipboard::rle32() const
{
    std::call_once(rle32_once_, [this]() {
        rle32_ = std::make_shared<const std::string>(rle32_encode(data_));
    });
    return rle32_;
}

std::uint64_t MarshalledClipboard::compute_hash(const char* data, std::size_t size)
{
    using namespace clipboard_data_detail;

    // Processes the data in 8-byte little-endian words so that hashing tens of megabytes stays
    // fast and gives the same result on every platform.
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    std::uint64_t h = 0x9e3779b97f4a7c15ULL ^ mix64(static_cast<std::uint64_t>(size));

    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        h = rotl64(h ^ mix64(load_le64(p + i)), 29) * 0x9e3779b97f4a7c15ULL;
    }

    std::uint64_t tail = 0;
    for (std::size_t shift = 0; i < size; ++i, shift += 8) {
        tail |= static_cast<std::uint64_t>(p[i]) << shift;
    }
    h = rotl64(h ^ mix64(tail ^ 0x5851f42d4c957f2dULL), 29) * 0x9e3779b97f4a7c15ULL;

    return mix64(h);
}

MarshalledClipboardPtr make_marshalled_clipboard(std::string data)
{
    return std::make_shared<const MarshalledClipboard>(std::move(data));
}

MarshalledClipboardPtr marshall_clipboard(const IClipboard& clipboard)
{
    return make_marshalled_clipboard(IClipboard::marshall(&clipboard));
}

const MarshalledClipboardPtr& empty_marshalled_clipboard()
{
    static const MarshalledClipboardPtr empty = marshall_clipboard(Clipboard{});
    return empty;
}

bool same_clipboard_contents(const MarshalledClipboardPtr& a, const MarshalledClipboardPtr& b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    return a->same_contents(*b);
}

std::uint32_t marshalled_clipboard_formats(const std::string& data)
{
    using namespace clipboard_data_detail;

    // see IClipboard::marshall() for the format
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    const std::size_t size = data.size();
    if (size < 4) {
        return 0;
    }

    std::uint32_t count = load_be32(p);
    std::size_t pos = 4;
    std::uint32_t formats = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (size - pos < 8) {
            return 0;
        }
        std::uint32_t format = load_be32(p + pos);
        std::uint32_t format_size = load_be32(p + pos + 4);
        pos += 8;
        if (size - pos < format_size) {
            return 0;
        }
        pos += format_size;
        if (format < IClipboard::kNumFormats) {
            formats |= 1u << format;
        }
    }
    return formats;
}

ClipboardOffer ClipboardOffer::describe(const MarshalledClipboard& data)
{
    ClipboardOffer offer;
    offer.formats = marshalled_clipboard_formats(data.data());
    offer.size = static_cast<std::uint32_t>(data.size());
    offer.hash = data.hash();
    return offer;
}

} // namespace inputleap
