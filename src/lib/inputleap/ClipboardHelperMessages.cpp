/*
 * InputLeap -- mouse and keyboard sharing utility
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "inputleap/ClipboardHelperMessages.h"

#include <cstring>

namespace inputleap {

namespace {

void put_le(std::string& out, std::uint64_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    }
}

std::uint64_t get_le(const unsigned char* p, int bytes)
{
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) {
        value |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return value;
}

} // namespace

std::string encode_clipboard_capture(std::uint32_t sequence, const std::string& data)
{
    std::string out;
    out.reserve(4 + data.size());
    put_le(out, sequence, 4);
    out.append(data);
    return out;
}

bool decode_clipboard_capture(const void* bytes, std::size_t size, std::uint32_t& sequence,
                              std::string& data)
{
    if (bytes == nullptr || size < 4) {
        return false;
    }
    const auto* p = static_cast<const unsigned char*>(bytes);
    sequence = static_cast<std::uint32_t>(get_le(p, 4));
    data.assign(reinterpret_cast<const char*>(p + 4), size - 4);
    return true;
}

std::string encode_clipboard_file_paths(std::uint32_t sequence,
                                        const std::vector<std::string>& paths)
{
    std::string out;
    put_le(out, sequence, 4);
    for (const std::string& path : paths) {
        put_le(out, path.size(), 4);
        out.append(path);
    }
    return out;
}

bool decode_clipboard_file_paths(const void* bytes, std::size_t size, std::uint32_t& sequence,
                                 std::vector<std::string>& paths)
{
    if (bytes == nullptr || size < 4) {
        return false;
    }
    const auto* p = static_cast<const unsigned char*>(bytes);
    std::vector<std::string> result;
    std::size_t pos = 4;
    while (pos < size) {
        if (size - pos < 4) {
            return false;
        }
        std::size_t length = static_cast<std::size_t>(get_le(p + pos, 4));
        pos += 4;
        if (length > size - pos) {
            return false;
        }
        result.emplace_back(reinterpret_cast<const char*>(p + pos), length);
        pos += length;
    }
    sequence = static_cast<std::uint32_t>(get_le(p, 4));
    paths = std::move(result);
    return true;
}

std::string encode_clipboard_sequence(std::uint32_t sequence)
{
    std::string out;
    put_le(out, sequence, 4);
    return out;
}

bool decode_clipboard_sequence(const void* bytes, std::size_t size, std::uint32_t& sequence)
{
    if (bytes == nullptr || size != 4) {
        return false;
    }
    sequence = static_cast<std::uint32_t>(get_le(static_cast<const unsigned char*>(bytes), 4));
    return true;
}

std::string encode_clipboard_helper_hello(std::uint64_t window, std::uint32_t pid)
{
    std::string out;
    put_le(out, window, 8);
    put_le(out, pid, 4);
    return out;
}

bool decode_clipboard_helper_hello(const void* bytes, std::size_t size, std::uint64_t& window,
                                   std::uint32_t& pid)
{
    if (bytes == nullptr || size != 12) {
        return false;
    }
    const auto* p = static_cast<const unsigned char*>(bytes);
    window = get_le(p, 8);
    pid = static_cast<std::uint32_t>(get_le(p + 8, 4));
    return true;
}

} // namespace inputleap
