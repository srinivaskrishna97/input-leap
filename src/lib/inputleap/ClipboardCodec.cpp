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

#include "inputleap/ClipboardCodec.h"

#include <algorithm>
#include <cstring>

namespace inputleap {

namespace {

constexpr std::size_t kRle32WordSize = 4;
constexpr std::size_t kRle32MaxLiteralWords = 128;
constexpr std::size_t kRle32MinRunWords = 3;
constexpr std::size_t kRle32MaxRunWords = 130;
constexpr unsigned kRle32RunBase = 125;

void rle32_append_literals(std::string& out, const char* data, std::size_t first_word,
                           std::size_t end_word)
{
    while (first_word < end_word) {
        std::size_t count = (std::min)(kRle32MaxLiteralWords, end_word - first_word);
        out.push_back(static_cast<char>(count - 1));
        out.append(data + first_word * kRle32WordSize, count * kRle32WordSize);
        first_word += count;
    }
}

} // namespace

std::size_t rle32_max_encoded_size(std::size_t size)
{
    std::size_t words = size / kRle32WordSize;
    std::size_t headers = (words + kRle32MaxLiteralWords - 1) / kRle32MaxLiteralWords;
    return size + headers;
}

std::string rle32_encode(const std::string& data)
{
    const char* p = data.data();
    const std::size_t words = data.size() / kRle32WordSize;

    std::string out;
    out.reserve(data.size() / 8 + 16);

    std::size_t literal_start = 0;
    std::size_t i = 0;
    while (i < words) {
        const char* word = p + i * kRle32WordSize;
        std::size_t run_end = i + 1;
        while (run_end < words && run_end - i < kRle32MaxRunWords &&
               std::memcmp(p + run_end * kRle32WordSize, word, kRle32WordSize) == 0) {
            ++run_end;
        }

        std::size_t run = run_end - i;
        if (run >= kRle32MinRunWords) {
            rle32_append_literals(out, p, literal_start, i);
            out.push_back(static_cast<char>(kRle32RunBase + run));
            out.append(word, kRle32WordSize);
            literal_start = run_end;
        }
        // shorter runs stay part of the pending literals
        i = run_end;
    }
    rle32_append_literals(out, p, literal_start, words);

    out.append(p + words * kRle32WordSize, data.size() - words * kRle32WordSize);
    return out;
}

bool rle32_decode(const std::string& encoded, std::size_t decoded_size, std::string& decoded)
{
    const std::size_t words_size = decoded_size - decoded_size % kRle32WordSize;
    const char* in = encoded.data();
    const std::size_t in_size = encoded.size();

    decoded.assign(decoded_size, '\0');
    char* out = decoded.empty() ? nullptr : &decoded[0];

    std::size_t pos = 0;
    std::size_t written = 0;
    while (written < words_size) {
        if (pos >= in_size) {
            return false;
        }
        unsigned header = static_cast<unsigned char>(in[pos++]);

        if (header < 128) {
            std::size_t bytes = (header + 1) * kRle32WordSize;
            if (bytes > in_size - pos || bytes > words_size - written) {
                return false;
            }
            std::memcpy(out + written, in + pos, bytes);
            pos += bytes;
            written += bytes;
        } else {
            std::size_t count = header - kRle32RunBase;
            if (kRle32WordSize > in_size - pos ||
                count * kRle32WordSize > words_size - written) {
                return false;
            }
            for (std::size_t k = 0; k < count; ++k) {
                std::memcpy(out + written, in + pos, kRle32WordSize);
                written += kRle32WordSize;
            }
            pos += kRle32WordSize;
        }
    }

    std::size_t tail = decoded_size - words_size;
    if (in_size - pos != tail) {
        return false;
    }
    if (tail > 0) {
        std::memcpy(out + written, in + pos, tail);
    }
    return true;
}

} // namespace inputleap
